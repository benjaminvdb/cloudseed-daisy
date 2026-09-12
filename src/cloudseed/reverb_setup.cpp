#include "reverb_channel.h"

#include <math.h>

#include "utils.h"

// Firmware startup, preset loading and diagnostics run outside the audio
// callback. This file uses -Os so the 128 KiB image retains room for profiling.
// Processing, freeze, and live decay/tone updates stay in reverb_channel.cpp
// with -O2. All parameters remain available through the same DSP API.
namespace cloudseed {

bool ReverbChannel::Init(int sample_rate, ChannelSide side, MemoryPool& pool) {
  sample_rate_ = sample_rate;
  side_ = side;

  bool ok = true;
  ok = pre_delay_.Init(pool, PreDelayBufferSize(sample_rate), 100,
                       utils::NextModPhase()) &&
       ok;
  ok = multitap_.Init(pool, MultitapBufferSize(sample_rate)) && ok;
  ok = diffuser_.Init(pool, sample_rate, DiffuserBufferSize(sample_rate)) && ok;
  for (int i = 0; i < kMaxLines; i++) {
    ok = lines_[i].Init(pool, sample_rate, LineBufferSize(sample_rate),
                        DiffuserBufferSize(sample_rate)) &&
         ok;
  }
  high_pass_.Init(sample_rate);
  low_pass_.Init(sample_rate);

  for (int i = 0; i < kParameterCount; i++) parameters_[i] = 0.0;

  cross_seed_ = 0.0;
  delay_line_seed_ = 0;
  post_diffusion_seed_ = 0;
  line_count_ = kMaxLines;
  per_line_gain_ = static_cast<float>(1.0 / sqrt(line_count_));
  frozen_ = false;
  high_pass_enabled_ = false;
  low_pass_enabled_ = false;
  diffuser_enabled_ = false;
  dry_out_ = predelay_out_ = early_out_ = line_out_gain_ = 0.f;

  diffuser_.SetInterpolationEnabled(true);
  high_pass_.SetCutoffHz(20);
  low_pass_.SetCutoffHz(20000);

  utils::Zero(temp_, kMaxBlockSize);
  utils::Zero(line_out_, kMaxBlockSize);
  utils::Zero(out_, kMaxBlockSize);

  UpdateDelayLineSeeds();
  return ok;
}

void ReverbChannel::SetPresetParameter(Parameter param, double value) {
  switch (param) {
    case Parameter::PreDelay:
      pre_delay_.SetDelay(static_cast<int>(Ms2Samples(value)));
      break;
    case Parameter::HighPass:
      high_pass_.SetCutoffHz(value);
      break;
    case Parameter::LowPass:
      low_pass_.SetCutoffHz(value);
      break;

    case Parameter::TapCount:
      multitap_.SetTapCount(static_cast<int>(value));
      break;
    case Parameter::TapLength:
      multitap_.SetTapLength(static_cast<int>(Ms2Samples(value)));
      break;
    case Parameter::TapGain:
      multitap_.SetTapGain(value);
      break;
    case Parameter::TapDecay:
      multitap_.SetTapDecay(value);
      break;

    case Parameter::DiffusionEnabled: {
      const bool new_value = value >= 0.5;
      if (new_value != diffuser_enabled_) diffuser_.ClearBuffers();
      diffuser_enabled_ = new_value;
      break;
    }
    case Parameter::DiffusionStages:
      diffuser_.set_stages(static_cast<int>(value));
      break;
    case Parameter::DiffusionDelay:
      diffuser_.SetDelay(static_cast<int>(Ms2Samples(value)));
      break;
    case Parameter::DiffusionFeedback:
      diffuser_.SetFeedback(value);
      break;

    case Parameter::LineCount: {
      int count = static_cast<int>(value);
      if (count < 1) count = 1;
      if (count > kMaxLines) count = kMaxLines;
      line_count_ = count;
      per_line_gain_ = static_cast<float>(1.0 / sqrt(count));
      break;
    }
    case Parameter::LineDelay:
      UpdateLines();
      break;

    case Parameter::LateDiffusionEnabled:
      for (int i = 0; i < kMaxLines; i++) {
        const bool new_value = value >= 0.5;
        if (new_value != lines_[i].diffuser_enabled)
          lines_[i].ClearDiffuserBuffer();
        lines_[i].diffuser_enabled = new_value;
      }
      break;
    case Parameter::LateDiffusionStages:
      for (int i = 0; i < kMaxLines; i++)
        lines_[i].SetDiffuserStages(static_cast<int>(value));
      break;
    case Parameter::LateDiffusionDelay:
      for (int i = 0; i < kMaxLines; i++)
        lines_[i].SetDiffuserDelay(static_cast<int>(Ms2Samples(value)));
      break;
    case Parameter::LateDiffusionFeedback:
      for (int i = 0; i < kMaxLines; i++) lines_[i].SetDiffuserFeedback(value);
      break;

    case Parameter::PostLowShelfGain:
      for (int i = 0; i < kMaxLines; i++) lines_[i].SetLowShelfGain(value);
      break;
    case Parameter::PostLowShelfFrequency:
      for (int i = 0; i < kMaxLines; i++) lines_[i].SetLowShelfFrequency(value);
      break;
    case Parameter::PostHighShelfGain:
      for (int i = 0; i < kMaxLines; i++) lines_[i].SetHighShelfGain(value);
      break;
    case Parameter::PostHighShelfFrequency:
      for (int i = 0; i < kMaxLines; i++)
        lines_[i].SetHighShelfFrequency(value);
      break;

    case Parameter::EarlyDiffusionModAmount:
      diffuser_.SetModulationEnabled(value > 0.0);
      diffuser_.SetModAmount(Ms2Samples(value));
      break;
    case Parameter::EarlyDiffusionModRate:
      diffuser_.SetModRate(value);
      break;
    case Parameter::LineModAmount:
      UpdateLines();
      break;
    case Parameter::LineModRate:
      UpdateLines();
      break;
    case Parameter::LateDiffusionModAmount:
      UpdateLines();
      break;
    case Parameter::LateDiffusionModRate:
      UpdateLines();
      break;

    case Parameter::TapSeed:
      multitap_.SetSeed(static_cast<int>(value));
      break;
    case Parameter::DiffusionSeed:
      diffuser_.SetSeed(static_cast<int>(value));
      break;
    case Parameter::DelaySeed:
      delay_line_seed_ = static_cast<int>(value);
      UpdateDelayLineSeeds();
      UpdateLines();
      break;
    case Parameter::PostDiffusionSeed:
      post_diffusion_seed_ = static_cast<int>(value);
      UpdatePostDiffusion();
      break;

    case Parameter::CrossSeed:
      // As in the plugin, only the late reverb of the right channel uses the
      // mixed series; the early reflections of both channels get the same.
      cross_seed_ = side_ == ChannelSide::Right ? value : 0.0;
      multitap_.SetCrossSeed(value);
      diffuser_.SetCrossSeed(value);
      UpdateDelayLineSeeds();
      UpdateLines();
      UpdatePostDiffusion();
      break;

    case Parameter::DryOut:
      dry_out_ = static_cast<float>(value);
      break;
    case Parameter::PredelayOut:
      predelay_out_ = static_cast<float>(value);
      break;
    case Parameter::EarlyOut:
      early_out_ = static_cast<float>(value);
      break;
    case Parameter::MainOut:
      line_out_gain_ = static_cast<float>(value);
      break;

    case Parameter::HiPassEnabled:
      high_pass_enabled_ = value >= 0.5;
      break;
    case Parameter::LowPassEnabled:
      low_pass_enabled_ = value >= 0.5;
      break;
    case Parameter::LowShelfEnabled:
      for (int i = 0; i < kMaxLines; i++)
        lines_[i].low_shelf_enabled = value >= 0.5;
      break;
    case Parameter::HighShelfEnabled:
      for (int i = 0; i < kMaxLines; i++)
        lines_[i].high_shelf_enabled = value >= 0.5;
      break;
    case Parameter::CutoffEnabled:
      for (int i = 0; i < kMaxLines; i++)
        lines_[i].cutoff_enabled = value >= 0.5;
      break;
    case Parameter::LateStageTap:
      for (int i = 0; i < kMaxLines; i++)
        lines_[i].late_stage_tap = value >= 0.5;
      break;

    case Parameter::Interpolation:
      for (int i = 0; i < kMaxLines; i++)
        lines_[i].SetInterpolationEnabled(value >= 0.5);
      break;

    default:
      break;
  }
}

int ReverbChannel::CollectPlacements(Placement* out, int max) {
  int n = 0;
  // In processing order, each with its progress group (staged_io.h).
  int group = side_ == ChannelSide::Left ? 0 : kStagedGroups;
  auto add = [&](Placement::Kind kind, void* object, int size, float streams,
                 bool placed) {
    if (n < max)
      out[n++] = {kind, object, size, streams / static_cast<float>(size),
                  placed, static_cast<uint8_t>(group)};
  };
  // Streams: a write and one or two adjacent reads per delay, a write and a
  // read per allpass stage, a write and a read per active tap.
  add(Placement::Kind::kDelay, &pre_delay_, pre_delay_.RequiredSize(), 3.f,
      pre_delay_.placed());
  add(Placement::Kind::kMultitap, &multitap_, multitap_.RequiredSize(),
      1.f + static_cast<float>(multitap_.active_count()), multitap_.placed());
  if (diffuser_enabled_) {
    for (int i = 0; i < diffuser_.stages(); i++) {
      ModulatedAllpass& stage = diffuser_.stage(i);
      add(Placement::Kind::kAllpass, &stage, stage.RequiredSize(), 2.f,
          stage.placed());
    }
  }
  for (int l = 0; l < line_count_; l++) {
    group++;
    ModulatedDelay& delay = lines_[l].delay();
    add(Placement::Kind::kDelay, &delay, delay.RequiredSize(), 3.f,
        delay.placed());
    if (!lines_[l].diffuser_enabled) continue;
    AllpassDiffuser& diffuser = lines_[l].diffuser();
    for (int i = 0; i < diffuser.stages(); i++) {
      ModulatedAllpass& stage = diffuser.stage(i);
      add(Placement::Kind::kAllpass, &stage, stage.RequiredSize(), 2.f,
          stage.placed());
    }
  }
  return n;
}

Workload ReverbChannel::GetWorkload() const {
  Workload w = {};
  w.lines = line_count_;
  w.early_stages = diffuser_enabled_ ? diffuser_.stages() : 0;
  w.late_stages = lines_[0].diffuser_enabled ? lines_[0].diffuser().stages() : 0;
  w.taps = multitap_.active_count();
  w.predelay = pre_delay_.delay();
  w.early_mod = diffuser_.modulation_enabled();
  w.line_mod = lines_[0].delay().mod_amount > 0.f;
  w.late_mod = lines_[0].diffuser().modulation_enabled();
  w.interpolation = lines_[0].diffuser().stage(0).interpolation_enabled;
  w.high_pass = high_pass_enabled_;
  w.low_pass = low_pass_enabled_;
  w.low_shelf = lines_[0].low_shelf_enabled;
  w.high_shelf = lines_[0].high_shelf_enabled;
  w.cutoff = lines_[0].cutoff_enabled;
  w.late_tap = lines_[0].late_stage_tap;
  // CollectPlacements() only reads; it is non-const because the placements
  // it returns are used to move buffers.
  Placement list[kMaxPlacements];
  const int n =
      const_cast<ReverbChannel*>(this)->CollectPlacements(list, kMaxPlacements);
  for (int i = 0; i < n; i++) {
    const int streams =
        static_cast<int>(list[i].weight * static_cast<float>(list[i].size) + 0.5f);
    w.streams += streams;
    if (!list[i].placed) w.slow_streams += streams;
  }
  return w;
}

void ReverbChannel::UseHomeMemory() {
  pre_delay_.UseHomeMemory();
  multitap_.UseHomeMemory();
  for (int i = 0; i < kMaxStages; i++) diffuser_.stage(i).UseHomeMemory();
  for (int l = 0; l < kMaxLines; l++) {
    lines_[l].delay().UseHomeMemory();
    for (int i = 0; i < kMaxStages; i++)
      lines_[l].diffuser().stage(i).UseHomeMemory();
  }
}

void ReverbChannel::ClearBuffers() {
  utils::Zero(temp_, kMaxBlockSize);
  utils::Zero(line_out_, kMaxBlockSize);
  utils::Zero(out_, kMaxBlockSize);

  low_pass_.output = 0.f;
  high_pass_.ClearBuffers();

  pre_delay_.ClearBuffers();
  multitap_.ClearBuffers();
  diffuser_.ClearBuffers();
  for (int i = 0; i < kMaxLines; i++) lines_[i].ClearBuffers();
}

void ReverbChannel::UpdateDelayLineSeeds() {
  sha_random::Generate(delay_line_seed_, kPluginLineCount * 3, cross_seed_,
                       delay_line_seeds_);
}

void ReverbChannel::UpdateLines() {
  const int line_delay_samples = static_cast<int>(
      Ms2Samples(parameters_[static_cast<int>(Parameter::LineDelay)]));
  const double line_mod_amount =
      Ms2Samples(parameters_[static_cast<int>(Parameter::LineModAmount)]);
  const double line_mod_rate =
      parameters_[static_cast<int>(Parameter::LineModRate)];

  const double late_diffusion_mod_amount = Ms2Samples(
      parameters_[static_cast<int>(Parameter::LateDiffusionModAmount)]);
  const double late_diffusion_mod_rate =
      parameters_[static_cast<int>(Parameter::LateDiffusionModRate)];

  const int count = kPluginLineCount;
  for (int i = 0; i < kMaxLines; i++) {
    const double mod_amount =
        line_mod_amount * (0.7 + 0.3 * delay_line_seeds_[i + count]);
    const double mod_rate = line_mod_rate *
                            (0.7 + 0.3 * delay_line_seeds_[i + 2 * count]) /
                            sample_rate_;

    double delay_samples =
        (0.5 + 1.0 * delay_line_seeds_[i]) * line_delay_samples;
    // when the delay is set really short, and the modulation is very high,
    // the mod could actually take the delay time negative; prevent that with
    // 2 extra samples as margin of safety
    if (delay_samples < mod_amount + 2) delay_samples = mod_amount + 2;

    line_delay_samples_[i] = delay_samples;
    lines_[i].SetDelay(static_cast<int>(delay_samples));
    lines_[i].SetLineModAmount(mod_amount);
    lines_[i].SetLineModRate(mod_rate);
    lines_[i].SetDiffuserModAmount(late_diffusion_mod_amount);
    lines_[i].SetDiffuserModRate(late_diffusion_mod_rate);
  }
  UpdateFeedback();
}

void ReverbChannel::UpdatePostDiffusion() {
  for (int i = 0; i < kMaxLines; i++) {
    lines_[i].SetDiffuserSeed(
        static_cast<int>(static_cast<long long>(post_diffusion_seed_) *
                         (i + 1)),
        cross_seed_);
  }
}

}  // namespace cloudseed
