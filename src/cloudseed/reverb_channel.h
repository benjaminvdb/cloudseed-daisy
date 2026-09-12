#pragma once

#include <stdint.h>

#include <stddef.h>

#include "allpass_diffuser.h"
#include "config.h"
#include "delay_line.h"
#include "memory_pool.h"
#include "modulated_delay.h"
#include "multitap_diffuser.h"
#include "one_pole.h"
#include "parameter.h"

namespace cloudseed {

enum class ChannelSide { Left, Right };

// A delay buffer that ReverbController::PlaceBuffers() may move to internal
// RAM: which object, how much memory it needs for the loaded preset, and the
// benefit of moving it (memory streams per float of buffer).
struct Placement {
  enum class Kind { kAllpass, kDelay, kMultitap };
  Kind kind;
  void* object;
  int size;      // floats, the object's RequiredSize()
  float weight;  // streams per float
  bool placed;   // runs in a fast pool
  uint8_t group; // the staging's progress group (staged_io.h)
};

// What Process() does with the current parameters, for diagnostics: the
// stages that run, the switches, and the delay-memory streams per sample
// (reads and writes that each advance one sample per sample), split into
// all of them and those still served from the memory of Init().
struct Workload {
  int lines;
  int early_stages;  // 0 when the early diffuser is off
  int late_stages;   // per line, 0 when the late diffusers are off
  int taps;          // active early-reflection taps
  int predelay;      // samples
  bool early_mod, line_mod, late_mod, interpolation;
  bool high_pass, low_pass, low_shelf, high_shelf, cutoff, late_tap;
  int streams;
  int slow_streams;
};

// One channel of the reverb (CloudSeed's ReverbChannel): input filters,
// pre-delay, the early reflections (multitap delay and allpass diffuser) and
// up to kMaxLines late-reverb delay lines in parallel, mixed to the output.
//
// Parameters arrive already scaled to their real ranges (milliseconds, Hz,
// seconds, counts) from ReverbController.
class ReverbChannel {
 public:
  static constexpr int PreDelayBufferSize(int sample_rate) {
    return sample_rate;  // 1 second
  }
  static constexpr int MultitapBufferSize(int sample_rate) {
    return sample_rate;  // 1 second
  }
  static constexpr int DiffuserBufferSize(int sample_rate) {
    return sample_rate * 150 / 1000;  // 150 ms: 100 ms delay plus modulation
  }
  static constexpr int LineBufferSize(int sample_rate) {
    return sample_rate * 2;  // 2 seconds: delay, random spread and modulation
  }

  // Floats of pool memory that Init() takes for the given sample rate.
  static constexpr size_t RequiredPoolFloats(int sample_rate) {
    return static_cast<size_t>(PreDelayBufferSize(sample_rate)) +
           static_cast<size_t>(MultitapBufferSize(sample_rate)) +
           static_cast<size_t>(kMaxStages) * DiffuserBufferSize(sample_rate) +
           static_cast<size_t>(kMaxLines) *
               (static_cast<size_t>(LineBufferSize(sample_rate)) +
                static_cast<size_t>(kMaxStages) *
                    DiffuserBufferSize(sample_rate));
  }

  bool Init(int sample_rate, ChannelSide side, MemoryPool& pool);

  void SetParameter(Parameter param, double value);

  void Process(const float* input, int count);

  void ClearBuffers();

  // Freezes the late reverb: the input is muted and the delay lines use
  // unity feedback without damping filters (see DelayLine::SetFrozen).
  void SetFrozen(bool frozen);
  bool frozen() const { return frozen_; }

  const float* output() const { return out_; }
  const float* line_output() const { return line_out_; }

  int line_count() const { return line_count_; }
#if CLOUDSEED_STAGED_MEMORY
  // Whom Process() reports its finished groups to (staged_io.h).
  void SetStagedProgress(StagedProgress progress) { staged_progress_ = progress; }
#endif

  // Delay buffers of this channel that Process() touches with the current
  // parameters, at most kMaxPlacements; see ReverbController::PlaceBuffers().
  static constexpr int kMaxPlacements = 2 + kMaxStages + kMaxLines * (1 + kMaxStages);
  int CollectPlacements(Placement* out, int max);

  // Runs every delay buffer in the memory from Init() again.
  void UseHomeMemory();

  Workload GetWorkload() const;

 private:
  void SetPresetParameter(Parameter param, double value);
  void UpdateLines();
  void UpdateFeedback();
  void UpdateDelayLineSeeds();
  void UpdatePostDiffusion();
  double Ms2Samples(double value) const {
    return value / 1000.0 * sample_rate_;
  }

  double parameters_[kParameterCount];
  double sample_rate_ = 48000.0;
  ChannelSide side_ = ChannelSide::Left;
#if CLOUDSEED_STAGED_MEMORY
  StagedProgress staged_progress_ = {nullptr, nullptr};
#endif

  ModulatedDelay pre_delay_;
  MultitapDiffuser multitap_;
  AllpassDiffuser diffuser_;
  DelayLine lines_[kMaxLines];
  Hp1 high_pass_;
  Lp1 low_pass_;

  float temp_[kMaxBlockSize];
  float line_out_[kMaxBlockSize];
  float out_[kMaxBlockSize];

  // Random values of the delay lines, generated for the plugin's 12 lines so
  // that the first kMaxLines lines match the plugin; refreshed when the delay
  // seed or the cross seed changes.
  double delay_line_seeds_[kPluginLineCount * 3];
  double line_delay_samples_[kMaxLines] = {};
  int delay_line_seed_ = 0;
  int post_diffusion_seed_ = 0;
  double cross_seed_ = 0.0;

  int line_count_ = kMaxLines;
  bool frozen_ = false;
  bool high_pass_enabled_ = false;
  bool low_pass_enabled_ = false;
  bool diffuser_enabled_ = false;
  float dry_out_ = 0.f;
  float predelay_out_ = 0.f;
  float early_out_ = 0.f;
  float line_out_gain_ = 0.f;
  float per_line_gain_ = 1.f;
};

}  // namespace cloudseed
