#pragma once

#include <math.h>

#include "config.h"
#include "fast_sin.h"
#include "memory_pool.h"
#include "staged_io.h"
#include "utils.h"

namespace cloudseed {

// Schroeder allpass filter whose delay is modulated by a sine LFO, with
// optional linear interpolation of the modulated read position (CloudSeed's
// ModulatedAllpass).
//
// The inner loops are shaped for the Cortex-M7, measured with the profiling
// build: they copy their state into locals and run over segments of the
// circular buffer that need no wrap-around checks (a store into the buffer
// would otherwise make GCC reload every float member after each sample);
// they prefetch the next cache line of every stream (the delay memory is far
// too large for the cache, so every eighth sample used to stall on a line
// fill); and they compute two samples per iteration, whose four dependent
// floating-point operations otherwise stall on the FPU's three-cycle
// latency. The arithmetic per sample is the plugin's, so the output does not
// change.
class ModulatedAllpass {
 public:
  bool Init(MemoryPool& pool, int buffer_size, int delay, float mod_phase) {
    home_buffer_ = pool.Allocate(buffer_size);
    home_size_ = buffer_size;
    buffer_ = home_buffer_;
    buffer_size_ = buffer_size;
    index_ = buffer_size - 1;
    samples_processed_ = 0;
    mod_phase_ = mod_phase;
    sample_delay = delay;
    feedback = 0.f;
    mod_amount = 0.f;
    mod_rate = 0.f;
    interpolation_enabled = true;
    modulation_enabled = false;
    Update();
    return buffer_ != nullptr;
  }

  int buffer_size() const { return buffer_size_; }
  // The delay memory in use.
  const float* memory() const { return buffer_; }

  // Delay memory (floats) the stage needs for its delay and modulation depth.
  int RequiredSize() const {
    return sample_delay + static_cast<int>(mod_amount) + 4;
  }

  // Runs the stage in buffer[0..size) instead of the memory from Init(); the
  // contents are undefined until ClearBuffers(). Size must be at least
  // RequiredSize(); a delay raised afterwards is limited to the buffer.
  void UseMemory(float* buffer, int size) {
    buffer_ = buffer;
    buffer_size_ = size;
    // From the start of the buffer: the blocks of a buffer sized in whole
    // blocks then never cross its end (one write-back copy per block for
    // the staging, see Staging::Plan). The position is not audible.
    index_ = 0;
    UpdateDelay();
  }

  void UseHomeMemory() { UseMemory(home_buffer_, home_size_); }

  // Whether the stage runs in memory from UseMemory() (a fast pool).
  bool placed() const { return buffer_ != home_buffer_; }

  void ClearBuffers() {
    utils::Zero(buffer_, buffer_size_);
    // Preset changes may replace the delay before the next LFO tick. Rebuild
    // its cached heads without advancing or resetting the modulation phase.
    UpdateDelay();
  }

  // Writes count samples to output, which may not overlap input.
  void Process(const float* input, float* output, int count) {
#if CLOUDSEED_STAGED_MEMORY
    if (is_staged_) {
      ProcessStaged(input, output, count);
      return;
    }
#endif
    if (modulation_enabled)
      ProcessWithMod(input, output, count);
    else
      ProcessNoMod(input, output, count);
  }

#if CLOUDSEED_STAGED_MEMORY
  // Serves the stage from a window and a block buffer instead of its ring
  // (see staged_io.h); nullptr returns to the ring.
  void SetStaged(const StagedHead* staged) {
    is_staged_ = staged != nullptr;
    if (staged != nullptr) staged_ = *staged;
  }
  // The lead of the next window (the staging manager rebuilds the window
  // for every block around the delay of that moment).
  void SetStagedLead(int lead) { staged_.lead = lead; }
  bool staged() const { return is_staged_; }
#endif

  // Ring index of the next sample written, and the current integer delay of
  // the read head (the LFO moves it by at most mod_amount around
  // sample_delay), for the staging manager.
  int write_index() const { return index_; }
  int current_delay() const { return delay_a_; }

  int sample_delay;  // samples
  float feedback;    // the allpass coefficient g
  float mod_amount;  // samples
  float mod_rate;    // cycles per sample
  bool interpolation_enabled;
  bool modulation_enabled;

 private:
  // Samples per prefetch: one cache line of floats.
  static constexpr int kLine = 8;

  // Whether the ring path is the hot path (a build without staging): only
  // then does it get its two-sample interleaving and its wrap-free fast
  // path; in a staging build it serves the few rings too short for a
  // window and is compiled for size.
  static constexpr bool kRingHot = !CLOUDSEED_STAGED_MEMORY;

  // One segment of n samples reading at r (one position) and writing at w.
  // With a delay of at least two samples the second sample's read never hits
  // the first sample's write, so the two chains can be interleaved.
  static inline void Segment(const float* in, float* o, const float* r,
                             float* w, int n, float g, bool interleave) {
    int k = 0;
    if (kRingHot && interleave) {
      for (; k + 1 < n; k += 2) {
        const float buf0 = r[k];
        const float buf1 = r[k + 1];
        const float in0 = in[k] + buf0 * g;
        const float in1 = in[k + 1] + buf1 * g;
        w[k] = in0;
        w[k + 1] = in1;
        o[k] = buf0 - in0 * g;
        o[k + 1] = buf1 - in1 * g;
      }
    }
    for (; k < n; k++) {
      const float buf_out = r[k];
      const float in_val = in[k] + buf_out * g;
      w[k] = in_val;
      o[k] = buf_out - in_val * g;
    }
  }

  // The same with an interpolated read from ra and rb (adjacent positions).
  static inline void SegmentInterpolated(const float* in, float* o,
                                         const float* ra, const float* rb,
                                         float* w, int n, float g, float gain_a,
                                         float gain_b, bool interleave) {
    int k = 0;
    if (kRingHot && interleave) {
      for (; k + 1 < n; k += 2) {
        const float a0 = ra[k];
        const float b0 = rb[k];
        const float a1 = ra[k + 1];
        const float b1 = rb[k + 1];
        const float buf0 = a0 * gain_a + b0 * gain_b;
        const float buf1 = a1 * gain_a + b1 * gain_b;
        const float in0 = in[k] + buf0 * g;
        const float in1 = in[k + 1] + buf1 * g;
        w[k] = in0;
        w[k + 1] = in1;
        o[k] = buf0 - in0 * g;
        o[k + 1] = buf1 - in1 * g;
      }
    }
    for (; k < n; k++) {
      const float buf_out = ra[k] * gain_a + rb[k] * gain_b;
      const float in_val = in[k] + buf_out * g;
      w[k] = in_val;
      o[k] = buf_out - in_val * g;
    }
  }

#if CLOUDSEED_STAGED_MEMORY
  // The staged runs: the window holds what the ring held before the block
  // (a staged delay is longer than a block, see Staging::Plan), so no
  // sample depends on another of the same block and four samples run as
  // independent chains. The Cortex-M7 issues in order and a fused
  // multiply-add's result feeds the next one as a multiplicand 5 cycles
  // later (the allpass output takes the new ring sample that way): two
  // chains left the FPU waiting, four keep it busy while the loads and
  // stores (one per cycle) set the pace. The arithmetic per sample is that
  // of Segment(), so the output is the ring path's.
  static inline void StagedRun(const float* __restrict in, float* __restrict o,
                               const float* __restrict r, float* __restrict w,
                               int n, float g) {
    int k = 0;
    for (; k + 3 < n; k += 4) {
      const float b0 = r[k];
      const float b1 = r[k + 1];
      const float b2 = r[k + 2];
      const float b3 = r[k + 3];
      const float i0 = in[k] + b0 * g;
      const float i1 = in[k + 1] + b1 * g;
      const float i2 = in[k + 2] + b2 * g;
      const float i3 = in[k + 3] + b3 * g;
      w[k] = i0;
      w[k + 1] = i1;
      w[k + 2] = i2;
      w[k + 3] = i3;
      o[k] = b0 - i0 * g;
      o[k + 1] = b1 - i1 * g;
      o[k + 2] = b2 - i2 * g;
      o[k + 3] = b3 - i3 * g;
    }
    for (; k < n; k++) {
      const float buf_out = r[k];
      const float in_val = in[k] + buf_out * g;
      w[k] = in_val;
      o[k] = buf_out - in_val * g;
    }
  }

  static inline void StagedRunInterpolated(const float* __restrict in,
                                           float* __restrict o,
                                           const float* __restrict ra,
                                           float* __restrict w, int n, float g,
                                           float gain_a, float gain_b) {
    // rb (the sample before ra) is ra - 1: one stream, read twice.
    int k = 0;
    for (; k + 3 < n; k += 4) {
      const float b0 = ra[k] * gain_a + ra[k - 1] * gain_b;
      const float b1 = ra[k + 1] * gain_a + ra[k] * gain_b;
      const float b2 = ra[k + 2] * gain_a + ra[k + 1] * gain_b;
      const float b3 = ra[k + 3] * gain_a + ra[k + 2] * gain_b;
      const float i0 = in[k] + b0 * g;
      const float i1 = in[k + 1] + b1 * g;
      const float i2 = in[k + 2] + b2 * g;
      const float i3 = in[k + 3] + b3 * g;
      w[k] = i0;
      w[k + 1] = i1;
      w[k + 2] = i2;
      w[k + 3] = i3;
      o[k] = b0 - i0 * g;
      o[k + 1] = b1 - i1 * g;
      o[k + 2] = b2 - i2 * g;
      o[k + 3] = b3 - i3 * g;
    }
    for (; k < n; k++) {
      const float buf_out = ra[k] * gain_a + ra[k - 1] * gain_b;
      const float in_val = in[k] + buf_out * g;
      w[k] = in_val;
      o[k] = buf_out - in_val * g;
    }
  }

  // One run of kModulationUpdateRate samples, written out: no loop-carried
  // state, so the samples are independent chains and the offsets fold into
  // the loads and stores. Longer runs do not occur; shorter ones (a block
  // shorter than the update rate, or the run before the block's first
  // update) take StagedRun().
  static inline void StagedRunFull(const float* __restrict in,
                                   float* __restrict o,
                                   const float* __restrict r,
                                   float* __restrict w, float g) {
#pragma GCC unroll 8
    for (int k = 0; k < kModulationUpdateRate; k++) {
      const float b = r[k];
      const float i = in[k] + b * g;
      w[k] = i;
      o[k] = b - i * g;
    }
  }

  static inline void StagedRunFullInterpolated(const float* __restrict in,
                                               float* __restrict o,
                                               const float* __restrict ra,
                                               float* __restrict w, float g,
                                               float gain_a, float gain_b) {
#pragma GCC unroll 8
    for (int k = 0; k < kModulationUpdateRate; k++) {
      const float b = ra[k] * gain_a + ra[k - 1] * gain_b;
      const float i = in[k] + b * g;
      w[k] = i;
      o[k] = b - i * g;
    }
  }

  // The runs of a block from its plan, reading at window[lead - delay + k].
  // Subtract the delay before forming a pointer: lead can exceed the entire
  // window allocation. One function per kernel keeps the
  // pointers and the coefficient in registers across the runs; the plan's
  // arrays are read once per run.
  static void StagedBlock(const float* __restrict in, float* __restrict o,
                          const float* __restrict window, int lead,
                          float* __restrict w,
                          const StagedLfoPlan& plan, float g) {
    int done = 0;
    for (int r = 0; r < plan.runs; r++) {
      const int n = plan.lengths[r];
      const float* const ra = window + (lead - plan.delays[r] + done);
      if (n == kModulationUpdateRate)
        StagedRunFull(in + done, o + done, ra, w + done, g);
      else
        StagedRun(in + done, o + done, ra, w + done, n, g);
      done += n;
    }
  }

  static void StagedBlockInterpolated(const float* __restrict in,
                                      float* __restrict o,
                                      const float* __restrict window, int lead,
                                      float* __restrict w,
                                      const StagedLfoPlan& plan, float g) {
    int done = 0;
    for (int r = 0; r < plan.runs; r++) {
      const int n = plan.lengths[r];
      const float* const ra = window + (lead - plan.delays[r] + done);
      if (n == kModulationUpdateRate) {
        StagedRunFullInterpolated(in + done, o + done, ra, w + done, g,
                                  plan.gains_a[r], plan.gains_b[r]);
      } else {
        StagedRunInterpolated(in + done, o + done, ra, w + done, n, g,
                              plan.gains_a[r], plan.gains_b[r]);
      }
      done += n;
    }
  }

  // Reads from the window and writes to the block buffer of staged_: no ring
  // wrap can occur, and the runs between two LFO updates are plain runs.
  // The block's LFO updates come first, all of them (PlanStagedRuns), then
  // the runs (StagedBlock). One copy of each kernel in the image: they are
  // called per stage and block, and inlining them at every call site costs
  // kilobytes of flash. count is at most kMaxBlockSize (the block buffer's
  // size, see staged_io.h).
  __attribute__((noinline)) void ProcessStaged(const float* input,
                                               float* const out, int count) {
    const StagedHead& s = staged_;
    const float g = feedback;
    const int size = buffer_size_;
    const bool modulated = modulation_enabled;
    StagedLfoPlan plan;
    if (modulated) {
      PlanStagedRuns(count, samples_processed_, mod_phase_, mod_rate,
                     static_cast<float>(sample_delay), Amount(),
                     static_cast<float>(size - 2), true, &plan);
      if (!plan.first_updated) {
        plan.delays[0] = delay_a_;
        plan.gains_a[0] = gain_a_;
        plan.gains_b[0] = gain_b_;
      }
      mod_phase_ = plan.phase;
      samples_processed_ = plan.processed;
      delay_a_ = plan.delays[plan.runs - 1];
      delay_b_ = delay_a_ + 1;
      gain_a_ = plan.gains_a[plan.runs - 1];
      gain_b_ = plan.gains_b[plan.runs - 1];
    } else {
      // Without modulation one run covers the block.
      plan.runs = 1;
      plan.lengths[0] = count;
      plan.delays[0] = sample_delay;
      samples_processed_ += static_cast<unsigned int>(count);
    }
    if (modulated && interpolation_enabled)
      StagedBlockInterpolated(input, out, s.window, s.lead, s.block, plan, g);
    else
      StagedBlock(input, out, s.window, s.lead, s.block, plan, g);
    index_ += count;
    if (index_ >= size) index_ -= size;
  }
#endif

  CLOUDSEED_RING_PATH void ProcessNoMod(const float* input, float* const out,
                                        int count) {
    float* const buffer = buffer_;
    const int size = buffer_size_;
    const float g = feedback;
    const bool interleave = sample_delay >= 2;
    int write = index_;
    int read = write - sample_delay;
    if (read < 0) read += size;

    int done = 0;
    while (done < count) {
      // Samples until the end of the block or until either index wraps,
      // processed one cache line at a time with the next line on its way.
      int n = count - done;
      if (n > size - write) n = size - write;
      if (n > size - read) n = size - read;
      const float* const r = buffer + read;
      float* const w = buffer + write;
      for (int k = 0; k < n; k += kLine) {
        utils::Prefetch(buffer, read + k + kLine, size);
        utils::Prefetch<1>(buffer, write + k + kLine, size);
        const int m = n - k < kLine ? n - k : kLine;
        Segment(input + done + k, out + done + k, r + k, w + k, m, g,
                interleave);
      }
      done += n;
      write += n;
      read += n;
      if (write >= size) write = 0;
      if (read >= size) read = 0;
    }
    index_ = write;
    samples_processed_ += static_cast<unsigned int>(count);
  }

  CLOUDSEED_RING_PATH void ProcessWithMod(const float* input, float* const out,
                                          int count) {
    float* const buffer = buffer_;
    const int size = buffer_size_;
    const float g = feedback;
    int write = index_;

    // The read head stays at most sample_delay + mod_amount + 1 samples
    // behind the write head, whatever the LFO does in this block. When
    // neither head can cross the end of the buffer within the block, the
    // usual case (a wrap happens once per traversal of the buffer), the runs
    // between two LFO updates need no wrap checks at all.
    const int furthest_read = sample_delay + static_cast<int>(mod_amount) + 2;
    const bool no_wrap =
        kRingHot && write + count <= size && write >= furthest_read;

    int done = 0;
    while (done < count) {
      if (samples_processed_ >= kModulationUpdateRate) Update();
      // Samples until the next LFO update or the end of the block.
      int n = kModulationUpdateRate - static_cast<int>(samples_processed_);
      if (n > count - done) n = count - done;
      samples_processed_ += static_cast<unsigned int>(n);
      const bool interleave = delay_a_ >= 2;

      if (no_wrap) {
        const float* const ra = buffer + (write - delay_a_);
        float* const w = buffer + write;
        utils::Prefetch(buffer, write - delay_a_ + kLine, size);
        utils::Prefetch<1>(buffer, write + kLine, size);
        if (interpolation_enabled) {
          // delay_b_ is delay_a_ + 1: the sample before ra.
          SegmentInterpolated(input + done, out + done, ra, ra - 1, w, n, g,
                              gain_a_, gain_b_, interleave);
        } else {
          Segment(input + done, out + done, ra, w, n, g, interleave);
        }
        done += n;
        write += n;
        continue;
      }

      int read_a = write - delay_a_;
      if (read_a < 0) read_a += size;
      if (interpolation_enabled) {
        int read_b = write - delay_b_;
        if (read_b < 0) read_b += size;
        const float gain_a = gain_a_;
        const float gain_b = gain_b_;
        while (n > 0) {
          int m = n;
          if (m > size - write) m = size - write;
          if (m > size - read_a) m = size - read_a;
          if (m > size - read_b) m = size - read_b;
          const float* const ra = buffer + read_a;
          const float* const rb = buffer + read_b;
          float* const w = buffer + write;
          // The next update moves the read head by a sample at most: the
          // following line of each stream is what the next run needs.
          utils::Prefetch(buffer, read_a + kLine, size);
          utils::Prefetch(buffer, read_b + kLine, size);
          utils::Prefetch<1>(buffer, write + kLine, size);
          SegmentInterpolated(input + done, out + done, ra, rb, w, m, g, gain_a,
                              gain_b, interleave);
          done += m;
          n -= m;
          write += m;
          read_a += m;
          read_b += m;
          if (write >= size) write = 0;
          if (read_a >= size) read_a = 0;
          if (read_b >= size) read_b = 0;
        }
      } else {
        while (n > 0) {
          int m = n;
          if (m > size - write) m = size - write;
          if (m > size - read_a) m = size - read_a;
          const float* const ra = buffer + read_a;
          float* const w = buffer + write;
          utils::Prefetch(buffer, read_a + kLine, size);
          utils::Prefetch<1>(buffer, write + kLine, size);
          Segment(input + done, out + done, ra, w, m, g, interleave);
          done += m;
          n -= m;
          write += m;
          read_a += m;
          if (write >= size) write = 0;
          if (read_a >= size) read_a = 0;
        }
      }
    }
    index_ = write;
  }

  // The LFO phase after one update. fmodf(phase, 1) as a subtraction: the
  // difference is exact (the phase is below 2^24, so it is a multiple of
  // the result's ulp), the same value without the library call.
  static inline float AdvancePhase(float phase, float rate) {
    phase += rate * kModulationUpdateRate;
    while (phase > 1.f) phase -= 1.f;
    return phase;
  }

  // The modulation depth, never modulating to a negative delay.
  float Amount() const {
    return mod_amount >= static_cast<float>(sample_delay)
               ? static_cast<float>(sample_delay - 1)
               : mod_amount;
  }

  // The read position for an LFO value: its integer delay and the gains of
  // the interpolation between it and the next sample. delay is the sample
  // delay as a float, max_delay the buffer's limit (memory from
  // UseMemory() is sized for the loaded preset, RequiredSize; a delay
  // raised afterwards is limited to it).
  static inline void Modulate(float mod, float delay, float amount,
                              float max_delay, int* delay_a, float* gain_a,
                              float* gain_b) {
    float total_delay = delay + amount * mod;
    if (total_delay <= 0.f) total_delay = 1.f;
    if (total_delay > max_delay) total_delay = max_delay;
    const int a = static_cast<int>(total_delay);
    const float partial = total_delay - static_cast<float>(a);
    *delay_a = a;
    *gain_a = 1.f - partial;
    *gain_b = partial;
  }

  void Update() {
    mod_phase_ = AdvancePhase(mod_phase_, mod_rate);
    UpdateDelay();
    samples_processed_ = 0;
  }

  void UpdateDelay() {
    Modulate(FastSin::Get(mod_phase_), static_cast<float>(sample_delay),
             Amount(), static_cast<float>(buffer_size_ - 2), &delay_a_,
             &gain_a_, &gain_b_);
    delay_b_ = delay_a_ + 1;
  }

  float* buffer_ = nullptr;
  int buffer_size_ = 0;
  float* home_buffer_ = nullptr;
  int home_size_ = 0;
  int index_ = 0;
  unsigned int samples_processed_ = 0;
  float mod_phase_ = 0.f;
  int delay_a_ = 0;
  int delay_b_ = 1;
  float gain_a_ = 1.f;
  float gain_b_ = 0.f;
#if CLOUDSEED_STAGED_MEMORY
  StagedHead staged_ = {};
  bool is_staged_ = false;
#endif
};

}  // namespace cloudseed
