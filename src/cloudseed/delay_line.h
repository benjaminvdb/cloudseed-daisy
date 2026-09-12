#pragma once

#include "allpass_diffuser.h"
#include "biquad.h"
#include "config.h"
#include "memory_pool.h"
#include "modulated_delay.h"
#include "one_pole.h"
#include "utils.h"

namespace cloudseed {

// One late-reverb delay line (CloudSeed's DelayLine): a modulated delay with
// feedback, an allpass diffuser either before or after the delay
// (late_stage_tap), and a low shelf, a high shelf and a first-order low-pass
// filter in the feedback path.
class DelayLine {
 public:
  bool Init(MemoryPool& pool, double sample_rate, int delay_buffer_size,
            int diffuser_buffer_size) {
    bool ok = true;
    ok = delay_.Init(pool, delay_buffer_size, 10000, utils::NextModPhase()) &&
         ok;
    ok = diffuser_.Init(pool, sample_rate, diffuser_buffer_size) && ok;
    low_shelf_.Init(Biquad::FilterType::LowShelf, sample_rate);
    high_shelf_.Init(Biquad::FilterType::HighShelf, sample_rate);
    low_pass_.Init(sample_rate);

    diffuser_enabled = false;
    low_shelf_enabled = false;
    high_shelf_enabled = false;
    cutoff_enabled = false;
    late_stage_tap = false;
    feedback_gain_ = 0.f;
    current_feedback_ = 0.f;
    feedback_ = filter_output_;
    frozen_ = false;
    utils::Zero(mixed_, kMaxBlockSize);
    utils::Zero(filter_output_, kMaxBlockSize);
    stage_ = mixed_;

    low_shelf_.slope = 1.0;
    low_shelf_.SetGainDb(-20);
    low_shelf_.frequency = 20;

    high_shelf_.slope = 1.0;
    high_shelf_.SetGainDb(-20);
    high_shelf_.frequency = 19000;

    low_pass_.SetCutoffHz(1000);
    low_shelf_.Update();
    high_shelf_.Update();

    SetDiffuserSeed(1, 0.0);
    return ok;
  }

  void SetDiffuserSeed(int seed, double cross_seed) {
    diffuser_.SetSeed(seed);
    diffuser_.SetCrossSeed(cross_seed);
  }

  void SetDelay(int delay_samples) { delay_.SetDelay(delay_samples); }
  void SetFeedback(double feedback) {
    feedback_gain_ = static_cast<float>(feedback);
  }

  // Frozen, the line uses unity feedback and bypasses its damping filters.
  // Delay interpolation and modulation can still change the held energy.
  void SetFrozen(bool value) {
    if (value == frozen_) return;
    frozen_ = value;
    if (!frozen_) {
      low_shelf_.ClearBuffers();
      high_shelf_.ClearBuffers();
      low_pass_.output = 0.f;
    }
  }
  bool frozen() const { return frozen_; }
  void SetDiffuserDelay(int delay_samples) {
    diffuser_.SetDelay(delay_samples);
  }
  void SetDiffuserFeedback(double feedback) { diffuser_.SetFeedback(feedback); }
  void SetDiffuserStages(int stages) { diffuser_.set_stages(stages); }

  void SetLowShelfGain(double gain) {
    low_shelf_.SetGain(gain);
    low_shelf_.Update();
  }

  void SetLowShelfFrequency(double frequency) {
    low_shelf_.frequency = frequency;
    low_shelf_.Update();
  }

  void SetHighShelfGain(double gain) {
    high_shelf_.SetGain(gain);
    high_shelf_.Update();
  }

  void SetHighShelfFrequency(double frequency) {
    high_shelf_.frequency = frequency;
    high_shelf_.Update();
  }

  void SetCutoffFrequency(double frequency) {
    low_pass_.SetCutoffHz(frequency);
  }

  void SetCutoffCoefficients(Lp1::Coefficients coefficients) {
    low_pass_.SetCoefficients(coefficients);
  }

  void SetLineModAmount(double amount) {
    delay_.mod_amount = static_cast<float>(amount);
  }

  void SetLineModRate(double rate) {
    delay_.mod_rate = static_cast<float>(rate);
  }

  void SetDiffuserModAmount(double amount) {
    diffuser_.SetModulationEnabled(amount > 0.0);
    diffuser_.SetModAmount(amount);
  }

  void SetDiffuserModRate(double rate) { diffuser_.SetModRate(rate); }

  void SetInterpolationEnabled(bool value) {
    diffuser_.SetInterpolationEnabled(value);
  }

  // The delay memory of the line and its diffuser, for
  // ReverbController::PlaceBuffers().
  ModulatedDelay& delay() { return delay_; }
  AllpassDiffuser& diffuser() { return diffuser_; }
  const ModulatedDelay& delay() const { return delay_; }
  const AllpassDiffuser& diffuser() const { return diffuser_; }

  const float* output() const {
    if (late_stage_tap) {
      if (diffuser_enabled)
        return diffuser_.output();
      else
        return mixed_;
    } else {
      return delay_.output();
    }
  }

  // The line's block: ProcessStage() mixes the feedback in and runs the
  // delay and the diffuser, ProcessFilters() runs the damping filters on
  // the result into the feedback for the next block. The filters of
  // different lines are independent: ReverbChannel runs the stages of all
  // lines, then the filters two lines at a time (ProcessFilterPair).
  void Process(const float* input, int count) {
    ProcessStage(input, count);
    ProcessFilters(count);
  }

  void ProcessStage(const float* input, int count) {
    if (count <= 0) return;
    CLOUDSEED_PROFILE_SECTION(kProfileLineMix);
    // Ramp live decay changes across the block. ClearBuffers initializes the
    // current gain to the preset gain, so a static preset is unchanged.
    const float target = frozen_ ? 1.f : feedback_gain_;
    const float* const previous = feedback_;
    if (current_feedback_ == target) {
      for (int i = 0; i < count; i++)
        mixed_[i] = input[i] + previous[i] * target;
    } else {
      const float step = (target - current_feedback_) / count;
      for (int i = 0; i < count; i++) {
        current_feedback_ += step;
        mixed_[i] = input[i] + previous[i] * current_feedback_;
      }
    }
    current_feedback_ = target;

    const float* stage;
    if (late_stage_tap) {
      if (diffuser_enabled) {
        CLOUDSEED_PROFILE_SECTION(kProfileLineDiffuser);
        diffuser_.Process(mixed_, count);
        CLOUDSEED_PROFILE_SECTION(kProfileLineDelay);
        delay_.Process(diffuser_.output(), count);
      } else {
        CLOUDSEED_PROFILE_SECTION(kProfileLineDelay);
        delay_.Process(mixed_, count);
      }
      stage = delay_.output();
    } else {
      CLOUDSEED_PROFILE_SECTION(kProfileLineDelay);
      delay_.Process(mixed_, count);
      if (diffuser_enabled) {
        CLOUDSEED_PROFILE_SECTION(kProfileLineDiffuser);
        diffuser_.Process(delay_.output(), count);
        stage = diffuser_.output();
      } else {
        stage = delay_.output();
      }
    }

    stage_ = stage;
  }

  // The damping filters run from the stage output straight into the
  // feedback buffer for the next block (the plugin copies through a
  // temporary buffer); without filters the next block reads the stage
  // output itself, which keeps its values until then. The feedback of
  // this block was mixed in by ProcessStage().
  __attribute__((noinline)) void ProcessFilters(int count) {
    if (count <= 0) return;
    float* const feedback_out = filter_output_;
    const float* src = stage_;
    if (!frozen_) {
      if (low_shelf_enabled) {
        low_shelf_.Process(src, feedback_out, count);
        src = feedback_out;
      }
      if (high_shelf_enabled) {
        high_shelf_.Process(src, feedback_out, count);
        src = feedback_out;
      }
      if (cutoff_enabled) {
        low_pass_.Process(src, feedback_out, count);
        src = feedback_out;
      }
    }
    feedback_ = src;
  }

  // ProcessFilters() of two lines with the same filter switches, each
  // filter of one line interleaved with the same filter of the other (see
  // Biquad::ProcessPair); the lines of a preset share the switches.
  __attribute__((noinline)) static void ProcessFilterPair(DelayLine& a,
                                                          DelayLine& b,
                                                          int count) {
    if (count <= 0) return;
    if (a.frozen_ != b.frozen_ || a.low_shelf_enabled != b.low_shelf_enabled ||
        a.high_shelf_enabled != b.high_shelf_enabled ||
        a.cutoff_enabled != b.cutoff_enabled) {
      a.ProcessFilters(count);
      b.ProcessFilters(count);
      return;
    }
    float* const out_a = a.filter_output_;
    float* const out_b = b.filter_output_;
    const float* src_a = a.stage_;
    const float* src_b = b.stage_;
    if (!a.frozen_) {
      if (a.low_shelf_enabled) {
        Biquad::ProcessPair(a.low_shelf_, b.low_shelf_, src_a, src_b, out_a,
                            out_b, count);
        src_a = out_a;
        src_b = out_b;
      }
      if (a.high_shelf_enabled) {
        Biquad::ProcessPair(a.high_shelf_, b.high_shelf_, src_a, src_b, out_a,
                            out_b, count);
        src_a = out_a;
        src_b = out_b;
      }
      if (a.cutoff_enabled) {
        Lp1::ProcessPair(a.low_pass_, b.low_pass_, src_a, src_b, out_a, out_b,
                         count);
        src_a = out_a;
        src_b = out_b;
      }
    }
    a.feedback_ = src_a;
    b.feedback_ = src_b;
  }

  void ClearDiffuserBuffer() { diffuser_.ClearBuffers(); }

  void ClearBuffers() {
    delay_.ClearBuffers();
    diffuser_.ClearBuffers();
    low_shelf_.ClearBuffers();
    high_shelf_.ClearBuffers();
    low_pass_.output = 0.f;
    utils::Zero(mixed_, kMaxBlockSize);
    utils::Zero(filter_output_, kMaxBlockSize);
    feedback_ = filter_output_;
    stage_ = mixed_;
    current_feedback_ = frozen_ ? 1.f : feedback_gain_;
  }

  bool diffuser_enabled = false;
  bool low_shelf_enabled = false;
  bool high_shelf_enabled = false;
  bool cutoff_enabled = false;
  bool late_stage_tap = false;

 private:
  ModulatedDelay delay_;
  AllpassDiffuser diffuser_;
  Biquad low_shelf_;
  Biquad high_shelf_;
  Lp1 low_pass_;
  float mixed_[kMaxBlockSize];
  float filter_output_[kMaxBlockSize];
  const float* stage_ = nullptr;     // this block's output before the filters
  const float* feedback_ = nullptr;  // the previous block's feedback signal
  float feedback_gain_ = 0.f;
  float current_feedback_ = 0.f;
  bool frozen_ = false;
};

}  // namespace cloudseed
