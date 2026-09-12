#pragma once

#include <math.h>

#include "config.h"
#include "fast_sin.h"
#include "memory_pool.h"
#include "staged_io.h"
#include "utils.h"

namespace cloudseed {

// Delay line with a sinusoidally modulated, linearly interpolated read head
// (CloudSeed's ModulatedDelay). The plugin jumps to a new read position when
// the delay length changes; this port crossfades from the old to the new
// position over kFadeSamples to support delay-time automation through the
// DSP API. The firmware panel does not expose delay length.
//
// Like ModulatedAllpass, the steady-state loop keeps its state in locals and
// runs over wrap-free segments between two LFO updates; the crossfade keeps
// the plugin's sample-by-sample form, it is rare.
class ModulatedDelay {
 public:
  static constexpr int kFadeSamples = 1024;

  bool Init(MemoryPool& pool, int buffer_size, int sample_delay,
            float mod_phase) {
    home_buffer_ = pool.Allocate(buffer_size);
    home_size_ = buffer_size;
    buffer_ = home_buffer_;
    buffer_size_ = buffer_size;
    write_index_ = 0;
    mod_phase_ = mod_phase;
    mod_amount = 0.f;
    mod_rate = 0.f;
    sample_delay_ = target_delay_ = old_sample_delay_ = sample_delay;
    fade_remaining_ = 0;
    samples_processed_ = 0;
    utils::Zero(output_, kMaxBlockSize);
    Update();
    return buffer_ != nullptr;
  }

  // Delay in samples; takes effect at the next LFO update, crossfading from
  // the previous length. Requests during a crossfade are picked up when it
  // has finished.
  void SetDelay(int samples) {
    if (samples < 0) samples = 0;
    if (samples > buffer_size_ - 2) samples = buffer_size_ - 2;
    target_delay_ = samples;
  }

  int delay() const { return target_delay_; }
  // The delay memory in use and its size (floats).
  const float* memory() const { return buffer_; }
  int memory_size() const { return buffer_size_; }

  const float* output() const { return output_; }

  // Memory for placement followed by ClearBuffers(): only the target delay
  // survives that operation. Including old heads wastes RAM on every switch
  // from a longer preset (and on the initial 10000-sample placeholder).
  int RequiredSize() const {
    return target_delay_ + static_cast<int>(mod_amount) + 4;
  }

  // Runs the delay in buffer[0..size) instead of the memory from Init(); the
  // contents are undefined until ClearBuffers(). Size must be at least
  // RequiredSize(); a delay raised afterwards is limited to the buffer.
  void UseMemory(float* buffer, int size) {
    buffer_ = buffer;
    buffer_size_ = size;
    write_index_ = 0;
    if (target_delay_ > size - 2) target_delay_ = size - 2;
    if (sample_delay_ > size - 2) sample_delay_ = size - 2;
    if (old_sample_delay_ > size - 2) old_sample_delay_ = size - 2;
    ComputeHeads(FastSin::Get(mod_phase_));  // the LFO stays where it is
  }

  void UseHomeMemory() { UseMemory(home_buffer_, home_size_); }

  bool placed() const { return buffer_ != home_buffer_; }

#if CLOUDSEED_STAGED_MEMORY
  // Serves the delay from a window and a block buffer instead of its ring
  // (see staged_io.h); nullptr returns to the ring. A staged delay applies a
  // new length at the next LFO update without the crossfade.
  void SetStaged(const StagedHead* staged) {
    is_staged_ = staged != nullptr;
    if (staged != nullptr) staged_ = *staged;
  }
  // The lead of the next window (see ModulatedAllpass::SetStagedLead).
  void SetStagedLead(int lead) { staged_.lead = lead; }
  bool staged() const { return is_staged_; }
#endif

  // Ring index of the next sample written and the current integer delay of
  // the read head, for the staging manager.
  int write_index() const { return write_index_; }
  int current_delay() const { return delay_a_; }
  bool crossfading() const { return fade_remaining_ > 0; }

  void Process(const float* input, int count) {
#if CLOUDSEED_STAGED_MEMORY
    if (is_staged_) {
      ProcessStaged(input, count);
      return;
    }
#endif
    ProcessRing(input, count);
  }

  // Whether the ring path is the hot path (a build without staging): only
  // then does it get its two-sample interleaving and its wrap-free fast
  // path (see ModulatedAllpass::kRingHot).
  static constexpr bool kRingHot = !CLOUDSEED_STAGED_MEMORY;

  CLOUDSEED_RING_PATH void ProcessRing(const float* input, int count) {
    float* const buffer = buffer_;
    const int size = buffer_size_;
    float* const out = output_;

    // As in ModulatedAllpass: when neither the write head nor the read head
    // can cross the end of the buffer within this block, the runs need no
    // wrap checks.
    int furthest =
        sample_delay_ > target_delay_ ? sample_delay_ : target_delay_;
    if (old_sample_delay_ > furthest) furthest = old_sample_delay_;
    furthest += static_cast<int>(mod_amount) + 2;
    const bool no_wrap =
        kRingHot && write_index_ + count <= size && write_index_ >= furthest;

    int done = 0;
    while (done < count) {
      if (samples_processed_ >= kModulationUpdateRate) Update();
      // Samples until the next LFO update or the end of the block.
      int n = kModulationUpdateRate - samples_processed_;
      if (n > count - done) n = count - done;

      if (no_wrap && fade_remaining_ == 0) {
        const float gain_a = gain_a_;
        const float gain_b = gain_b_;
        const float* const in = input + done;
        float* const o = out + done;
        float* const w = buffer + write_index_;
        const float* const ra = buffer + read_a_;
        const float* const rb = buffer + read_b_;
        utils::Prefetch(buffer, read_a_ + 8, size);
        utils::Prefetch<1>(buffer, write_index_ + 8, size);
        int k = 0;
        if (kRingHot && write_index_ - read_a_ >= 2) {
          for (; k + 1 < n; k += 2) {
            const float a0 = ra[k];
            const float b0 = rb[k];
            const float a1 = ra[k + 1];
            const float b1 = rb[k + 1];
            w[k] = in[k];
            w[k + 1] = in[k + 1];
            o[k] = a0 * gain_a + b0 * gain_b;
            o[k + 1] = a1 * gain_a + b1 * gain_b;
          }
        }
        for (; k < n; k++) {
          w[k] = in[k];
          o[k] = ra[k] * gain_a + rb[k] * gain_b;
        }
        done += n;
        samples_processed_ += n;
        write_index_ += n;
        read_a_ += n;
        read_b_ += n;
        continue;
      }

      if (fade_remaining_ > 0) {
        // Crossfade to a new delay length, as the plugin does it.
        if (n > fade_remaining_) n = fade_remaining_;
        for (int i = done; i < done + n; i++) {
          buffer[write_index_] = input[i];
          float sample = buffer[read_a_] * gain_a_ + buffer[read_b_] * gain_b_;
          const float old = buffer[old_read_a_] * old_gain_a_ +
                            buffer[old_read_b_] * old_gain_b_;
          const float t =
              static_cast<float>(fade_remaining_) * (1.f / kFadeSamples);
          sample = old * t + sample * (1.f - t);
          fade_remaining_--;
          if (++old_read_a_ >= size) old_read_a_ = 0;
          if (++old_read_b_ >= size) old_read_b_ = 0;
          out[i] = sample;
          if (++write_index_ >= size) write_index_ = 0;
          if (++read_a_ >= size) read_a_ = 0;
          if (++read_b_ >= size) read_b_ = 0;
        }
        done += n;
        samples_processed_ += n;
        continue;
      }

      const float gain_a = gain_a_;
      const float gain_b = gain_b_;
      int write = write_index_;
      int read_a = read_a_;
      int read_b = read_b_;
      samples_processed_ += n;
      // A delay below two samples would read what the previous sample wrote.
      const bool interleave = read_a != write && (write - read_a) != 1 &&
                              (write - read_a) != 1 - size;
      while (n > 0) {
        int m = n;
        if (m > size - write) m = size - write;
        if (m > size - read_a) m = size - read_a;
        if (m > size - read_b) m = size - read_b;
        const float* const in = input + done;
        float* const o = out + done;
        float* const w = buffer + write;
        const float* const ra = buffer + read_a;
        const float* const rb = buffer + read_b;
        // The next cache line of every stream, for the next run (the LFO
        // moves the read head by a sample at most per update).
        utils::Prefetch(buffer, read_a + 8, size);
        utils::Prefetch(buffer, read_b + 8, size);
        utils::Prefetch<1>(buffer, write + 8, size);
        int k = 0;
        if (kRingHot && interleave) {
          // Two independent samples per iteration hide the FPU latency.
          for (; k + 1 < m; k += 2) {
            const float a0 = ra[k];
            const float b0 = rb[k];
            const float a1 = ra[k + 1];
            const float b1 = rb[k + 1];
            w[k] = in[k];
            w[k + 1] = in[k + 1];
            o[k] = a0 * gain_a + b0 * gain_b;
            o[k + 1] = a1 * gain_a + b1 * gain_b;
          }
        }
        for (; k < m; k++) {
          w[k] = in[k];
          o[k] = ra[k] * gain_a + rb[k] * gain_b;
        }
        done += m;
        n -= m;
        write += m;
        read_a += m;
        read_b += m;
        if (write >= size) write = 0;
        if (read_a >= size) read_a = 0;
        if (read_b >= size) read_b = 0;
      }
      write_index_ = write;
      read_a_ = read_a;
      read_b_ = read_b;
    }
    // The final run can end exactly at size. A crossfade in the next call
    // dereferences these indices before its per-sample wrap checks.
    if (write_index_ == size) write_index_ = 0;
    if (read_a_ == size) read_a_ = 0;
    if (read_b_ == size) read_b_ = 0;
  }

#if CLOUDSEED_STAGED_MEMORY
  // One run of a staged block: the window predates the block (the delay
  // exceeds a block, see Staging::Plan), so four samples run as
  // independent chains (see ModulatedAllpass::StagedRun). The interpolated
  // read of sample k takes ra[k] and the sample before it.
  static inline void StagedRun(const float* __restrict in, float* __restrict o,
                               const float* __restrict ra, float* __restrict w,
                               int n, float gain_a, float gain_b) {
    int k = 0;
    for (; k + 3 < n; k += 4) {
      const float a0 = ra[k];
      const float a1 = ra[k + 1];
      const float a2 = ra[k + 2];
      const float a3 = ra[k + 3];
      const float b0 = ra[k - 1];
      w[k] = in[k];
      w[k + 1] = in[k + 1];
      w[k + 2] = in[k + 2];
      w[k + 3] = in[k + 3];
      o[k] = a0 * gain_a + b0 * gain_b;
      o[k + 1] = a1 * gain_a + a0 * gain_b;
      o[k + 2] = a2 * gain_a + a1 * gain_b;
      o[k + 3] = a3 * gain_a + a2 * gain_b;
    }
    for (; k < n; k++) {
      w[k] = in[k];
      o[k] = ra[k] * gain_a + ra[k - 1] * gain_b;
    }
  }

  // One run of kModulationUpdateRate samples, written out (see
  // ModulatedAllpass::StagedRunFull).
  static inline void StagedRunFull(const float* __restrict in,
                                   float* __restrict o,
                                   const float* __restrict ra,
                                   float* __restrict w, float gain_a,
                                   float gain_b) {
#pragma GCC unroll 8
    for (int k = 0; k < kModulationUpdateRate; k++) {
      w[k] = in[k];
      o[k] = ra[k] * gain_a + ra[k - 1] * gain_b;
    }
  }

  // The runs of a block from its plan (see ModulatedAllpass::StagedBlock).
  static void StagedBlock(const float* __restrict in, float* __restrict o,
                          const float* __restrict window, int lead,
                          float* __restrict w,
                          const StagedLfoPlan& plan) {
    int done = 0;
    for (int r = 0; r < plan.runs; r++) {
      const int n = plan.lengths[r];
      const float* const ra = window + (lead - plan.delays[r] + done);
      if (n == kModulationUpdateRate) {
        StagedRunFull(in + done, o + done, ra, w + done, plan.gains_a[r],
                      plan.gains_b[r]);
      } else {
        StagedRun(in + done, o + done, ra, w + done, n, plan.gains_a[r],
                  plan.gains_b[r]);
      }
      done += n;
    }
  }

  // Reads from the window and writes to the block buffer of staged_: no
  // wrap checks, and the runs between LFO updates are plain runs. The
  // block's LFO updates come first, all of them (PlanStagedRuns, see
  // ModulatedAllpass::ProcessStaged), then the runs. No crossfade while
  // staged: the window only covers one read head, so a new length takes
  // effect at the block's first update. count is at most kMaxBlockSize.
  __attribute__((noinline)) void ProcessStaged(const float* input, int count) {
    const StagedHead& s = staged_;
    const int size = buffer_size_;
    const unsigned int processed =
        static_cast<unsigned int>(samples_processed_);
    const bool updates =
        processed >= kModulationUpdateRate ||
        count > kModulationUpdateRate - static_cast<int>(processed);
    if (updates && target_delay_ != sample_delay_)
      sample_delay_ = old_sample_delay_ = target_delay_;
    StagedLfoPlan plan;
    PlanStagedRuns(count, processed, mod_phase_, mod_rate,
                   static_cast<float>(sample_delay_), mod_amount,
                   static_cast<float>(size - 2), false, &plan);
    if (!plan.first_updated) {
      plan.delays[0] = delay_a_;
      plan.gains_a[0] = gain_a_;
      plan.gains_b[0] = gain_b_;
    }
    mod_phase_ = plan.phase;
    samples_processed_ = static_cast<int>(plan.processed);
    StagedBlock(input, output_, s.window, s.lead, s.block, plan);
    fade_remaining_ = 0;
    write_index_ += count;
    if (write_index_ >= size) write_index_ -= size;
    // Reuse the last run's head instead of recomputing the same sine,
    // modulation and interpolation. Only the ring indices need advancing.
    const int last = plan.runs - 1;
    delay_a_ = plan.delays[last];
    gain_a_ = plan.gains_a[last];
    gain_b_ = plan.gains_b[last];
    read_a_ = write_index_ - delay_a_;
    if (read_a_ < 0) read_a_ += size;
    read_b_ = read_a_ - 1;
    if (read_b_ < 0) read_b_ += size;
  }
#endif

  // Clears the delay memory. Also applies a pending delay length at once,
  // without a crossfade.
  void ClearBuffers() {
    utils::Zero(buffer_, buffer_size_);
    utils::Zero(output_, kMaxBlockSize);
    sample_delay_ = old_sample_delay_ = target_delay_;
    fade_remaining_ = 0;
    Update();
  }

  float mod_amount;  // samples
  float mod_rate;    // cycles per sample

 private:
  // The LFO phase after one update (see ModulatedAllpass::AdvancePhase).
  static inline float AdvancePhase(float phase, float rate) {
    phase += rate * kModulationUpdateRate;
    while (phase > 1.f) phase -= 1.f;
    return phase;
  }

  // The read position for an LFO value: its integer delay and the gains of
  // the interpolation with the next sample; max_delay is the buffer's
  // limit (memory from UseMemory() is sized for the loaded preset,
  // RequiredSize; a modulation depth raised afterwards is limited to it).
  static inline void Modulate(float mod, float delay, float amount,
                              float max_delay, int* delay_a, float* gain_a,
                              float* gain_b) {
    float total_delay = delay + amount * mod;
    if (total_delay > max_delay) total_delay = max_delay;
    const int a = static_cast<int>(total_delay);
    const float partial = total_delay - static_cast<float>(a);
    *delay_a = a;
    *gain_a = 1.f - partial;
    *gain_b = partial;
  }

  void Update() {
    mod_phase_ = AdvancePhase(mod_phase_, mod_rate);
    const float mod = FastSin::Get(mod_phase_);

    if (fade_remaining_ == 0 && target_delay_ != sample_delay_) {
      old_sample_delay_ = sample_delay_;
      sample_delay_ = target_delay_;
      fade_remaining_ = kFadeSamples;
    }

    ComputeHeads(mod);
    samples_processed_ = 0;
  }

  void ComputeHeads(float mod) {
    delay_a_ =
        ComputeHead(sample_delay_, mod, &read_a_, &read_b_, &gain_a_, &gain_b_);
    if (fade_remaining_ > 0) {
      ComputeHead(old_sample_delay_, mod, &old_read_a_, &old_read_b_,
                  &old_gain_a_, &old_gain_b_);
    }
  }

  // Returns the integer delay of the head.
  int ComputeHead(int delay, float mod, int* read_a, int* read_b, float* gain_a,
                  float* gain_b) const {
    int delay_a;
    Modulate(mod, static_cast<float>(delay), mod_amount,
             static_cast<float>(buffer_size_ - 2), &delay_a, gain_a, gain_b);
    const int delay_b = delay_a + 1;
    int a = write_index_ - delay_a;
    int b = write_index_ - delay_b;
    if (a < 0) a += buffer_size_;
    if (b < 0) b += buffer_size_;
    *read_a = a;
    *read_b = b;
    return delay_a;
  }

  float* buffer_ = nullptr;
  int buffer_size_ = 0;
  float* home_buffer_ = nullptr;
  int home_size_ = 0;
  int write_index_ = 0;
  int read_a_ = 0, read_b_ = 0;
  float gain_a_ = 1.f, gain_b_ = 0.f;
  int old_read_a_ = 0, old_read_b_ = 0;
  float old_gain_a_ = 1.f, old_gain_b_ = 0.f;
  int sample_delay_ = 0;
  int target_delay_ = 0;
  int old_sample_delay_ = 0;
  int fade_remaining_ = 0;
  int samples_processed_ = 0;
  float mod_phase_ = 0.f;
  int delay_a_ = 0;  // integer delay of the current read head
#if CLOUDSEED_STAGED_MEMORY
  StagedHead staged_ = {};
  bool is_staged_ = false;
#endif
  float output_[kMaxBlockSize];
};

}  // namespace cloudseed
