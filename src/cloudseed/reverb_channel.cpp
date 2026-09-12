#include "reverb_channel.h"

#include <math.h>

#include "utils.h"

namespace cloudseed {

void ReverbChannel::SetParameter(Parameter param, double value) {
  parameters_[static_cast<int>(param)] = value;
  // Only decay and tone change in the firmware callback. Keep those paths
  // optimized for speed; the large preset-only switch is optimized for size.
  if (param == Parameter::LineDecay) {
    UpdateFeedback();
  } else if (param == Parameter::PostCutoffFrequency) {
    const auto coefficients = Lp1::Calculate(value, sample_rate_);
    for (int i = 0; i < kMaxLines; i++)
      lines_[i].SetCutoffCoefficients(coefficients);
  } else {
    SetPresetParameter(param, value);
  }
}

void ReverbChannel::Process(const float* input, int count) {
  if (count > kMaxBlockSize) count = kMaxBlockSize;
  CLOUDSEED_PROFILE_SECTION(kProfileInput);

  const float* predelay_output = pre_delay_.output();
  const float* low_pass_input = high_pass_enabled_ ? temp_ : input;

  if (frozen_) {
    // Do not let notes played during freeze accumulate in the input filters:
    // their state would otherwise enter the reverb on release.
    utils::Zero(temp_, count);
  } else {
    if (high_pass_enabled_) high_pass_.Process(input, temp_, count);
    if (low_pass_enabled_) low_pass_.Process(low_pass_input, temp_, count);
    if (!low_pass_enabled_ && !high_pass_enabled_)
      utils::Copy(input, temp_, count);

    // Preserve the legacy input gate (-90 dBFS); this is distinct from
    // flushing subnormal values inside the feedback network on the MCU.
    // The square is never below +0, so its bits order like its value
    // (FloatBits): an integer compare and a select, no flag transfer.
    for (int i = 0; i < count; i++) {
      const float n = temp_[i];
      const float square = n * n;
      temp_[i] = FloatBits(square) < FloatBits(0.000000001f) ? 0.f : n;
    }
  }

  // Frozen, nothing new enters the reverb; what is still on its way through
  // the pre-delay and the early reflections joins the held tail.
  CLOUDSEED_PROFILE_SECTION(kProfilePreDelay);
  pre_delay_.Process(temp_, count);
  CLOUDSEED_PROFILE_SECTION(kProfileMultitap);
  multitap_.Process(pre_delay_.output(), count);

  const float* early_out_stage =
      diffuser_enabled_ ? diffuser_.output() : multitap_.output();

  CLOUDSEED_PROFILE_SECTION(kProfileEarlyDiffuser);
  if (diffuser_enabled_) diffuser_.Process(multitap_.output(), count);

#if CLOUDSEED_STAGED_MEMORY
  // The staging streams the copies of the heads that are done (staged_io.h).
  const int group = side_ == ChannelSide::Left ? 0 : kStagedGroups;
  staged_progress_.Report(group);
#endif
  for (int i = 0; i < line_count_; i++) {
    lines_[i].ProcessStage(early_out_stage, count);
#if CLOUDSEED_STAGED_MEMORY
    staged_progress_.Report(group + 1 + i);
#endif
  }

  // The damping filters of two lines at a time (see DelayLine).
  CLOUDSEED_PROFILE_SECTION(kProfileLineFilters);
  int i = 0;
  for (; i + 1 < line_count_; i += 2)
    DelayLine::ProcessFilterPair(lines_[i], lines_[i + 1], count);
  if (i < line_count_) lines_[i].ProcessFilters(count);

  CLOUDSEED_PROFILE_SECTION(kProfileOutput);
  for (int i = 0; i < line_count_; i++) {
    const float* buf = lines_[i].output();
    if (i == 0) {
      for (int j = 0; j < count; j++) line_out_[j] = buf[j];
    } else {
      for (int j = 0; j < count; j++) line_out_[j] += buf[j];
    }
  }

  utils::Gain(line_out_, per_line_gain_, count);

  for (int i = 0; i < count; i++) {
    out_[i] = dry_out_ * input[i] +                 //
              predelay_out_ * predelay_output[i] +  //
              early_out_ * early_out_stage[i] +     //
              line_out_gain_ * line_out_[i];
  }
}

void ReverbChannel::SetFrozen(bool frozen) {
  if (frozen && !frozen_) {
    low_pass_.output = 0.f;
    high_pass_.ClearBuffers();
  }
  frozen_ = frozen;
  for (int i = 0; i < kMaxLines; i++) lines_[i].SetFrozen(frozen);
}

void ReverbChannel::UpdateFeedback() {
  const double decay_millis =
      parameters_[static_cast<int>(Parameter::LineDecay)] * 1000;
  const double decay_samples = Ms2Samples(decay_millis);
  for (int i = 0; i < kMaxLines; i++) {
    // Presets set LineDelay before LineDecay; avoid division by zero during
    // loading. Retain the plugin's unrounded delay in the gain calculation.
    const double gain =
        decay_samples > 0.0
            ? utils::Db2Gain(line_delay_samples_[i] / decay_samples * (-60))
            : 0.0;
    lines_[i].SetFeedback(gain);
  }
}

}  // namespace cloudseed
