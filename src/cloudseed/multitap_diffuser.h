#pragma once

#include <math.h>

#include "config.h"
#include "memory_pool.h"
#include "sha_random.h"
#include "staged_io.h"
#include "utils.h"

namespace cloudseed {

// Multitap delay for the early reflections (CloudSeed's MultitapDiffuser):
// up to kMaxTaps taps at seeded random positions with seeded random gains,
// decaying towards the last tap.
//
// The taps are summed tap by tap over the whole block rather than sample by
// sample over all taps: every tap then reads its part of the buffer in one
// sequential run, so a cache line fetched from the delay memory serves eight
// consecutive samples and consecutive lines come from the same SDRAM row.
// The additions happen in the same order per sample, so the result is the
// same as the plugin's.
class MultitapDiffuser {
 public:
  bool Init(MemoryPool& pool, int buffer_size) {
    home_buffer_ = pool.Allocate(buffer_size);
    home_size_ = buffer_size;
    buffer_ = home_buffer_;
    len_ = buffer_size;
    index_ = 0;
    count_ = 1;
    length_ = 1;
    gain_ = 1.0;
    decay_ = 0.0;
    cross_seed_ = 0.0;
    seed_ = 0;
    utils::Zero(output_, kMaxBlockSize);
    UpdateSeeds();
    return buffer_ != nullptr;
  }

  void SetSeed(int seed) {
    seed_ = seed;
    UpdateSeeds();
  }

  void SetCrossSeed(double cross_seed) {
    cross_seed_ = cross_seed;
    UpdateSeeds();
  }

  const float* output() const { return output_; }

  void SetTapCount(int tap_count) {
    count_ = tap_count;
    Update();
  }

  void SetTapLength(int tap_length) {
    length_ = tap_length;
    Update();
  }

  void SetTapDecay(double tap_decay) {
    decay_ = tap_decay;
    Update();
  }

  void SetTapGain(double tap_gain) {
    gain_ = tap_gain;
    Update();
  }

  // Taps with a nonzero gain, i.e. the reads per sample.
  int active_count() const { return active_count_; }
  // Position (samples back) and gain of active tap i (0 <= i < active_count()).
  int tap_position(int i) const { return tap_positions_[i]; }
  float tap_gain(int i) const { return tap_gains_[i]; }
  // The delay memory in use and its size (floats).
  const float* memory() const { return buffer_; }
  int memory_size() const { return len_; }

  // Process writes a complete block before reading its taps. Retain that
  // block as well as the history, including for zero/short tap lengths.
  int RequiredSize() const { return static_cast<int>(length_) + kMaxBlockSize; }

  // Runs the delay in buffer[0..size) instead of the memory from Init(); the
  // contents are undefined until ClearBuffers(). Size must be at least
  // RequiredSize(); a tap length raised afterwards is limited to the buffer.
  void UseMemory(float* buffer, int size) {
    buffer_ = buffer;
    len_ = size;
    // The index runs backwards: from the end, the blocks of a buffer sized
    // in whole blocks never cross it (see Staging::Plan). Not audible.
    index_ = size - 1;
    Update();
  }

  void UseHomeMemory() { UseMemory(home_buffer_, home_size_); }

  bool placed() const { return buffer_ != home_buffer_; }

  static constexpr int kHistory = 2 * kMaxBlockSize;
#if CLOUDSEED_STAGED_MEMORY
  // Serves the taps from per-tap windows and a block buffer instead of the
  // ring (see staged_io.h); nullptr returns to the ring. Taps closer than
  // kHistory samples read the input history kept here instead of a window.
  void SetStaged(const StagedTaps* staged) {
    if (staged != nullptr && !is_staged_) utils::Zero(history_, kHistory);
    is_staged_ = staged != nullptr;
    if (staged != nullptr) staged_ = *staged;
  }
  bool staged() const { return is_staged_; }
#endif

  // Ring index the next block's first sample is written to (the index runs
  // backwards), for the staging manager.
  int write_index() const { return index_; }

  void Process(const float* input, int count) {
#if CLOUDSEED_STAGED_MEMORY
    if (is_staged_) {
      ProcessStaged(input, count);
      return;
    }
#endif
    ProcessRing(input, count);
  }

  // In a staging build the ring path only serves a multitap whose tap
  // windows did not fit the staging memory (the biggest programs) and is
  // compiled for size like the other ring paths, see config.h.
  CLOUDSEED_RING_PATH void ProcessRing(const float* input, int count) {
    float* const buffer = buffer_;
    const int len = len_;
    float* const out = output_;
    const int* const tap_pos = tap_positions_;
    const float* const tap_gain = tap_gains_;
    const int taps = active_count_;

    // A live length increase can put a tap near the end of an already placed
    // ring. Preserve sample ordering when a full write would erase history.
    // Normal preset placement reserves a whole block and takes the fast path.
    if (count > len || (taps > 0 && tap_pos[taps - 1] > len - count)) {
      for (int i = 0; i < count; i++) {
        buffer[index_] = input[i];
        float sum = 0.f;
        for (int j = 0; j < taps; j++) {
          int r = index_ + tap_pos[j];
          if (r >= len) r -= len;
          sum += buffer[r] * tap_gain[j];
        }
        out[i] = sum;
        if (--index_ < 0) index_ += len;
      }
      return;
    }

    // The write index counts down: sample i lands at start - i (mod len).
    const int start = index_;
    {
      int idx = start;
      int done = 0;
      while (done < count) {
        int n = count - done;
        if (n > idx + 1) n = idx + 1;
        float* const w = buffer + idx;
        const float* const in = input + done;
        for (int k = 0; k < n; k++) w[-k] = in[k];
        done += n;
        idx -= n;
        if (idx < 0) idx += len;
      }
      index_ = idx;
    }

    for (int i = 0; i < count; i++) out[i] = 0.f;

    // Four taps per pass, each added in tap order to the same sum, so the
    // additions happen in the plugin's order; the taps read backwards, one
    // cache line at a time with the next one prefetched. The tap positions
    // never exceed the buffer, so a single subtraction is the plugin's modulo.
    int j = 0;
    for (; j + 3 < taps; j += 4) {
      const float g0 = tap_gain[j], g1 = tap_gain[j + 1];
      const float g2 = tap_gain[j + 2], g3 = tap_gain[j + 3];
      int r0 = start + tap_pos[j], r1 = start + tap_pos[j + 1];
      int r2 = start + tap_pos[j + 2], r3 = start + tap_pos[j + 3];
      if (r0 >= len) r0 -= len;
      if (r1 >= len) r1 -= len;
      if (r2 >= len) r2 -= len;
      if (r3 >= len) r3 -= len;
      int done = 0;
      while (done < count) {
        // Samples until the end of the block or until one of the four
        // reads wraps.
        int n = count - done;
        if (n > r0 + 1) n = r0 + 1;
        if (n > r1 + 1) n = r1 + 1;
        if (n > r2 + 1) n = r2 + 1;
        if (n > r3 + 1) n = r3 + 1;
        const float* const s0 = buffer + r0;
        const float* const s1 = buffer + r1;
        const float* const s2 = buffer + r2;
        const float* const s3 = buffer + r3;
        float* const o = out + done;
        for (int c = 0; c < n; c += 8) {
          utils::Prefetch(buffer, r0 - c - 8, len);
          utils::Prefetch(buffer, r1 - c - 8, len);
          utils::Prefetch(buffer, r2 - c - 8, len);
          utils::Prefetch(buffer, r3 - c - 8, len);
          const int m = n - c < 8 ? n : c + 8;
          for (int k = c; k < m; k++) {
            float sum = o[k];
            sum += s0[-k] * g0;
            sum += s1[-k] * g1;
            sum += s2[-k] * g2;
            sum += s3[-k] * g3;
            o[k] = sum;
          }
        }
        done += n;
        r0 -= n;
        r1 -= n;
        r2 -= n;
        r3 -= n;
        if (r0 < 0) r0 += len;
        if (r1 < 0) r1 += len;
        if (r2 < 0) r2 += len;
        if (r3 < 0) r3 += len;
      }
    }
    for (; j < taps; j++) {
      const float gain = tap_gain[j];
      int r = start + tap_pos[j];
      if (r >= len) r -= len;
      int done = 0;
      while (done < count) {
        int n = count - done;
        if (n > r + 1) n = r + 1;
        const float* const src = buffer + r;
        float* const o = out + done;
        for (int c = 0; c < n; c += 8) {
          utils::Prefetch(buffer, r - c - 8, len);
          const int m = n - c < 8 ? n : c + 8;
          for (int k = c; k < m; k++) o[k] += src[-k] * gain;
        }
        done += n;
        r -= n;
        if (r < 0) r += len;
      }
    }
  }

  void ClearBuffers() {
    utils::Zero(buffer_, len_);
    utils::Zero(output_, kMaxBlockSize);
#if CLOUDSEED_STAGED_MEMORY
    utils::Zero(history_, kHistory);
#endif
  }

 private:
#if CLOUDSEED_STAGED_MEMORY
  // The block goes to the block buffer (ring order: block[i] is the sample
  // written to ring[start - (kMaxBlockSize - 1) + i]); every tap with a
  // window reads it backwards, taps nearer than kHistory samples read this
  // block's input and the history of the two previous blocks. The sums keep
  // the tap order of the ring path.
  void ProcessStaged(const float* input, int count) {
    const StagedTaps& s = staged_;
    float* const out = output_;
    const int taps = active_count_;
    const int* const tap_pos = tap_positions_;
    const float* const tap_gain = tap_gains_;
    const int len = len_;

    for (int k = 0; k < count; k++) s.block[kMaxBlockSize - 1 - k] = input[k];
    for (int i = 0; i < count; i++) out[i] = 0.f;

    // The taps come in order of their position: those without a window
    // (the near ones) first, served from this block's input and the
    // history.
    int j = 0;
    for (; j < taps && s.windows[j] == nullptr; j++) {
      const float gain = tap_gain[j];
      // Sample k reads what was written pos samples earlier: this block's
      // input for k >= pos, else the history (oldest first).
      const int pos = tap_pos[j];
      int k = 0;
      for (; k < count && k < pos; k++)
        out[k] += history_[kHistory + k - pos] * gain;
      for (; k < count; k++) out[k] += input[k - pos] * gain;
    }
    // Four windowed taps per pass, added in tap order to the same sum (the
    // ring path's order); two samples per iteration keep the FPU busy
    // while a sum's four dependent multiply-adds settle.
    for (; j + 3 < taps; j += 4) {
      const float g0 = tap_gain[j], g1 = tap_gain[j + 1];
      const float g2 = tap_gain[j + 2], g3 = tap_gain[j + 3];
      const float* const s0 = s.windows[j] + (kMaxBlockSize - 1);
      const float* const s1 = s.windows[j + 1] + (kMaxBlockSize - 1);
      const float* const s2 = s.windows[j + 2] + (kMaxBlockSize - 1);
      const float* const s3 = s.windows[j + 3] + (kMaxBlockSize - 1);
      int k = 0;
      for (; k + 1 < count; k += 2) {
        float sum0 = out[k];
        float sum1 = out[k + 1];
        sum0 += s0[-k] * g0;
        sum1 += s0[-k - 1] * g0;
        sum0 += s1[-k] * g1;
        sum1 += s1[-k - 1] * g1;
        sum0 += s2[-k] * g2;
        sum1 += s2[-k - 1] * g2;
        sum0 += s3[-k] * g3;
        sum1 += s3[-k - 1] * g3;
        out[k] = sum0;
        out[k + 1] = sum1;
      }
      for (; k < count; k++) {
        float sum = out[k];
        sum += s0[-k] * g0;
        sum += s1[-k] * g1;
        sum += s2[-k] * g2;
        sum += s3[-k] * g3;
        out[k] = sum;
      }
    }
    for (; j < taps; j++) {
      const float gain = tap_gain[j];
      const float* const src = s.windows[j] + (kMaxBlockSize - 1);
      for (int k = 0; k < count; k++) out[k] += src[-k] * gain;
    }

    // Shift the history by one block.
    for (int i = 0; i < kHistory - count; i++)
      history_[i] = history_[i + count];
    for (int k = 0; k < count; k++) history_[kHistory - count + k] = input[k];

    index_ -= count;
    if (index_ < 0) index_ += len;
  }
#endif

 private:
  void Update() {
    int s = 0;
    auto rand = [&]() { return seed_values_[s++]; };

    if (count_ < 1) count_ = 1;
    if (count_ > kMaxTaps) count_ = kMaxTaps;
    if (length_ < count_) length_ = count_;

    // Used to adjust the volume of the overall output as it grows when more
    // taps are added. The plugin divides two integers here, so the factor is
    // 1.0 for fewer than kMaxTaps taps and 0.5 for kMaxTaps; kept as is.
    const double tap_count_factor = 1.0 / (1 + sqrt(count_ / kMaxTaps));

    double tap_data[kMaxTaps];
    double sum_lengths = 0.0;
    for (int i = 0; i < count_; i++) {
      const double val = 0.1 + rand();
      tap_data[i] = val;
      sum_lengths += val;
    }

    const double scale_length = length_ / sum_lengths;
    tap_positions_[0] = 0;
    for (int i = 1; i < count_; i++) {
      tap_positions_[i] =
          tap_positions_[i - 1] + static_cast<int>(tap_data[i] * scale_length);
    }

    const double last_tap_pos = tap_positions_[count_ - 1];
    for (int i = 0; i < count_; i++) {
      // when decay set to 0, there is no decay, when set to 1, the gain at the
      // last sample is 0.01 = -40dB
      const double g = exp(-decay_ * 2 * tap_positions_[i] /
                           (double)(last_tap_pos + 1) * M_LN10);
      const double tap = (2 * rand() - 1) * tap_count_factor;
      tap_gains_[i] = static_cast<float>(tap * g * gain_);
    }

    // Set the tap vs. clean mix
    tap_gains_[0] = static_cast<float>(1 - gain_);

    // Memory from UseMemory() is sized for the loaded preset (RequiredSize);
    // a tap length raised afterwards is limited to it.
    for (int i = 0; i < count_; i++) {
      if (tap_positions_[i] > len_ - 1) tap_positions_[i] = len_ - 1;
    }

    // Preserve summation order, but omit exact zero gains from the hot loop.
    // Noise in the Hallway asks for 50 taps with gain=0: only the dry tap
    // contributes. Keep writing history so subsequent parameter changes work.
    active_count_ = 0;
    for (int i = 0; i < count_; i++) {
      if (tap_gains_[i] == 0.f) continue;
      tap_positions_[active_count_] = tap_positions_[i];
      tap_gains_[active_count_++] = tap_gains_[i];
    }
  }

  void UpdateSeeds() {
    sha_random::Generate(seed_, kMaxTaps * 2, cross_seed_, seed_values_);
    Update();
  }

  float* buffer_ = nullptr;
  int len_ = 0;
  float* home_buffer_ = nullptr;
  int home_size_ = 0;
  int index_ = 0;
#if CLOUDSEED_STAGED_MEMORY
  StagedTaps staged_ = {};
  bool is_staged_ = false;
  float history_[kHistory];
#endif
  float output_[kMaxBlockSize];
  float tap_gains_[kMaxTaps];
  int tap_positions_[kMaxTaps];
  double seed_values_[kMaxTaps * 2];
  int seed_ = 0;
  double cross_seed_ = 0.0;
  int count_ = 1;
  int active_count_ = 1;
  double length_ = 1;
  double gain_ = 1.0;
  double decay_ = 0.0;
};

}  // namespace cloudseed
