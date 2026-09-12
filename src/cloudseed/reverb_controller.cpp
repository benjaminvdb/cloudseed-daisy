#include "reverb_controller.h"

#include <math.h>

#include "response_curves.h"
#include "utils.h"

namespace cloudseed {

bool ReverbController::Init(int sample_rate, MemoryPool& pool) {
  utils::ResetModPhaseSequence();
  sample_rate_ = sample_rate;
  for (int i = 0; i < kParameterCount; i++) parameters_[i] = 0.0;
  bool ok = channel_l_.Init(sample_rate, ChannelSide::Left, pool);
  ok = channel_r_.Init(sample_rate, ChannelSide::Right, pool) && ok;
  return ok && !pool.overflow();
}

double ReverbController::P(Parameter param) const {
  const int idx = static_cast<int>(param);
  return idx >= 0 && idx < kParameterCount ? parameters_[idx] : 0.0;
}

double ReverbController::GetParameter(Parameter param) const {
  return P(param);
}

double ReverbController::GetScaledParameter(Parameter param) const {
  switch (param) {
    // Input
    case Parameter::InputMix:
      return P(Parameter::InputMix);
    case Parameter::PreDelay:
      return static_cast<int>(P(Parameter::PreDelay) * 1000);

    case Parameter::HighPass:
      return 20 + ResponseOct(P(Parameter::HighPass), 4) * 980;
    case Parameter::LowPass:
      return 400 + ResponseOct(P(Parameter::LowPass), 4) * 19600;

    // Early
    case Parameter::TapCount:
      return 1 + static_cast<int>(P(Parameter::TapCount) * (kMaxTaps - 1));
    case Parameter::TapLength:
      return static_cast<int>(P(Parameter::TapLength) * 500);
    case Parameter::TapGain:
      return ResponseDec(P(Parameter::TapGain), 2);
    case Parameter::TapDecay:
      return P(Parameter::TapDecay);

    case Parameter::DiffusionEnabled:
      return P(Parameter::DiffusionEnabled) < 0.5 ? 0.0 : 1.0;
    case Parameter::DiffusionStages:
      return 1 + static_cast<int>(P(Parameter::DiffusionStages) *
                                  (kMaxStages - 0.001));
    case Parameter::DiffusionDelay:
      return static_cast<int>(10 + P(Parameter::DiffusionDelay) * 90);
    case Parameter::DiffusionFeedback:
      return P(Parameter::DiffusionFeedback);

    // Late
    case Parameter::LineCount:
      return 1 + static_cast<int>(P(Parameter::LineCount) * 11.999);
    case Parameter::LineDelay:
      return static_cast<int>(20.0 +
                              ResponseDec(P(Parameter::LineDelay), 2) * 980);
    case Parameter::LineDecay:
      return 0.05 + ResponseDec(P(Parameter::LineDecay), 3) * 59.95;

    case Parameter::LateDiffusionEnabled:
      return P(Parameter::LateDiffusionEnabled) < 0.5 ? 0.0 : 1.0;
    case Parameter::LateDiffusionStages:
      return 1 + static_cast<int>(P(Parameter::LateDiffusionStages) *
                                  (kMaxStages - 0.001));
    case Parameter::LateDiffusionDelay:
      return static_cast<int>(10 + P(Parameter::LateDiffusionDelay) * 90);
    case Parameter::LateDiffusionFeedback:
      return P(Parameter::LateDiffusionFeedback);

    // Frequency Response
    case Parameter::PostLowShelfGain:
      return ResponseDec(P(Parameter::PostLowShelfGain), 2);
    case Parameter::PostLowShelfFrequency:
      return 20 + ResponseOct(P(Parameter::PostLowShelfFrequency), 4) * 980;
    case Parameter::PostHighShelfGain:
      return ResponseDec(P(Parameter::PostHighShelfGain), 2);
    case Parameter::PostHighShelfFrequency:
      return 400 + ResponseOct(P(Parameter::PostHighShelfFrequency), 4) * 19600;
    case Parameter::PostCutoffFrequency:
      return 400 + ResponseOct(P(Parameter::PostCutoffFrequency), 4) * 19600;

    // Modulation
    case Parameter::EarlyDiffusionModAmount:
      return P(Parameter::EarlyDiffusionModAmount) * 2.5;
    case Parameter::EarlyDiffusionModRate:
      return ResponseDec(P(Parameter::EarlyDiffusionModRate), 2) * 5;
    case Parameter::LineModAmount:
      return P(Parameter::LineModAmount) * 2.5;
    case Parameter::LineModRate:
      return ResponseDec(P(Parameter::LineModRate), 2) * 5;
    case Parameter::LateDiffusionModAmount:
      return P(Parameter::LateDiffusionModAmount) * 2.5;
    case Parameter::LateDiffusionModRate:
      return ResponseDec(P(Parameter::LateDiffusionModRate), 2) * 5;

    // Seeds
    case Parameter::TapSeed:
      return static_cast<int>(floor(P(Parameter::TapSeed) * 1000000 + 0.001));
    case Parameter::DiffusionSeed:
      return static_cast<int>(
          floor(P(Parameter::DiffusionSeed) * 1000000 + 0.001));
    case Parameter::DelaySeed:
      return static_cast<int>(floor(P(Parameter::DelaySeed) * 1000000 + 0.001));
    case Parameter::PostDiffusionSeed:
      return static_cast<int>(
          floor(P(Parameter::PostDiffusionSeed) * 1000000 + 0.001));

    // Output
    case Parameter::CrossSeed:
      return P(Parameter::CrossSeed);

    case Parameter::DryOut:
      return ResponseDec(P(Parameter::DryOut), 2);
    case Parameter::PredelayOut:
      return ResponseDec(P(Parameter::PredelayOut), 2);
    case Parameter::EarlyOut:
      return ResponseDec(P(Parameter::EarlyOut), 2);
    case Parameter::MainOut:
      return ResponseDec(P(Parameter::MainOut), 2);

    // Switches
    case Parameter::HiPassEnabled:
      return P(Parameter::HiPassEnabled) < 0.5 ? 0.0 : 1.0;
    case Parameter::LowPassEnabled:
      return P(Parameter::LowPassEnabled) < 0.5 ? 0.0 : 1.0;
    case Parameter::LowShelfEnabled:
      return P(Parameter::LowShelfEnabled) < 0.5 ? 0.0 : 1.0;
    case Parameter::HighShelfEnabled:
      return P(Parameter::HighShelfEnabled) < 0.5 ? 0.0 : 1.0;
    case Parameter::CutoffEnabled:
      return P(Parameter::CutoffEnabled) < 0.5 ? 0.0 : 1.0;
    case Parameter::LateStageTap:
      return P(Parameter::LateStageTap) < 0.5 ? 0.0 : 1.0;

    // Effects
    case Parameter::Interpolation:
      return P(Parameter::Interpolation) < 0.5 ? 0.0 : 1.0;

    default:
      return 0.0;
  }
}

void ReverbController::SetParameter(Parameter param, double value) {
  const int idx = static_cast<int>(param);
  if (idx < 0 || idx >= kParameterCount) return;
  parameters_[idx] = value;
  const double scaled = GetScaledParameter(param);
  if (param == Parameter::InputMix)
    input_mix_ = static_cast<float>(scaled * 0.5);
  channel_l_.SetParameter(param, scaled);
  channel_r_.SetParameter(param, scaled);
}

void ReverbController::LoadPreset(const double* values) {
  // Placed buffers are sized for the previous preset and would limit the
  // delays of this one while they are set: start from the Init() memory.
  channel_l_.UseHomeMemory();
  channel_r_.UseHomeMemory();
  for (int i = 0; i < kParameterCount; i++) {
    SetParameter(static_cast<Parameter>(i), values[i]);
  }
  PlaceBuffers();
}

void ReverbController::LoadPreset(const float* values) {
  channel_l_.UseHomeMemory();
  channel_r_.UseHomeMemory();
  for (int i = 0; i < kParameterCount; i++) {
    SetParameter(static_cast<Parameter>(i), static_cast<double>(values[i]));
  }
  PlaceBuffers();
}

void ReverbController::AddFastPool(float* base, size_t floats) {
  if (fast_pool_count_ >= kMaxFastPools || base == nullptr || floats == 0)
    return;
  fast_pools_[fast_pool_count_++].Init(base, floats);
}

void ReverbController::PlaceBuffers() {
  // Start from the Init() memory: a buffer placed for the previous preset
  // but unused by this one must not keep memory that is handed out again.
  channel_l_.UseHomeMemory();
  channel_r_.UseHomeMemory();
  unplaced_ = 0;
  if (fast_pool_count_ == 0) return;
  for (int i = 0; i < fast_pool_count_; i++) fast_pools_[i].Reset();

  constexpr int kMax = 2 * ReverbChannel::kMaxPlacements;
  Placement list[kMax];
  int n = channel_l_.CollectPlacements(list, kMax);
  n += channel_r_.CollectPlacements(list + n, kMax - n);

  // Most beneficial first; a stable insertion sort keeps the order of equal
  // weights deterministic.
  for (int i = 1; i < n; i++) {
    const Placement p = list[i];
    int j = i;
    while (j > 0 && list[j - 1].weight < p.weight) {
      list[j] = list[j - 1];
      j--;
    }
    list[j] = p;
  }

  for (int i = 0; i < n; i++) {
    float* memory = nullptr;
    size_t allocated = 0;
    // Whole blocks: the blocks a placed buffer's owner writes from its start
    // then never cross its end, which spares the staging a split copy per
    // block (Staging::Plan). The Init() buffers are whole blocks as well.
    const size_t wanted =
        (static_cast<size_t>(list[i].size) + kMaxBlockSize - 1) /
        kMaxBlockSize * kMaxBlockSize;
    for (int p = 0; p < fast_pool_count_ && memory == nullptr; p++) {
      if (fast_pools_[p].Fits(wanted))
        memory = fast_pools_[p].AllocateAligned(wanted, &allocated);
    }
    if (memory == nullptr) {
      unplaced_++;
      continue;
    }
    const int size = static_cast<int>(allocated);
    switch (list[i].kind) {
      case Placement::Kind::kAllpass:
        static_cast<ModulatedAllpass*>(list[i].object)->UseMemory(memory, size);
        break;
      case Placement::Kind::kDelay:
        static_cast<ModulatedDelay*>(list[i].object)->UseMemory(memory, size);
        break;
      case Placement::Kind::kMultitap:
        static_cast<MultitapDiffuser*>(list[i].object)->UseMemory(memory, size);
        break;
    }
  }
}

void ReverbController::ClearBuffers() {
  channel_l_.ClearBuffers();
  channel_r_.ClearBuffers();
}

void ReverbController::SetFrozen(bool frozen) {
  channel_l_.SetFrozen(frozen);
  channel_r_.SetFrozen(frozen);
}

void ReverbController::Process(const float* in_l, const float* in_r,
                               float* out_l, float* out_r, int count) {
  if (count > kMaxBlockSize) count = kMaxBlockSize;

  const float cm = input_mix_;
  const float cmi = 1.f - cm;

  for (int i = 0; i < count; i++) {
    left_in_[i] = in_l[i] * cmi + in_r[i] * cm;
    right_in_[i] = in_r[i] * cmi + in_l[i] * cm;
  }

  channel_l_.Process(left_in_, count);
  channel_r_.Process(right_in_, count);
  const float* left_out = channel_l_.output();
  const float* right_out = channel_r_.output();

  for (int i = 0; i < count; i++) {
    out_l[i] = left_out[i];
    out_r[i] = right_out[i];
  }
}

}  // namespace cloudseed
