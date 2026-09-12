#pragma once

namespace cloudseed {

// CloudSeed's parameters, in the plugin's order. Every parameter is set as a
// normalized 0..1 value; ReverbController scales it to its real range.
enum class Parameter {
  // Input
  InputMix = 0,
  PreDelay,
  HighPass,
  LowPass,

  // Early reflections
  TapCount,
  TapLength,
  TapGain,
  TapDecay,
  DiffusionEnabled,
  DiffusionStages,
  DiffusionDelay,
  DiffusionFeedback,

  // Late reverberation
  LineCount,
  LineDelay,
  LineDecay,
  LateDiffusionEnabled,
  LateDiffusionStages,
  LateDiffusionDelay,
  LateDiffusionFeedback,

  // Frequency response of the feedback path
  PostLowShelfGain,
  PostLowShelfFrequency,
  PostHighShelfGain,
  PostHighShelfFrequency,
  PostCutoffFrequency,

  // Modulation
  EarlyDiffusionModAmount,
  EarlyDiffusionModRate,
  LineModAmount,
  LineModRate,
  LateDiffusionModAmount,
  LateDiffusionModRate,

  // Seeds
  TapSeed,
  DiffusionSeed,
  DelaySeed,
  PostDiffusionSeed,
  CrossSeed,

  // Output mixer
  DryOut,
  PredelayOut,
  EarlyOut,
  MainOut,

  // Switches
  HiPassEnabled,
  LowPassEnabled,
  LowShelfEnabled,
  HighShelfEnabled,
  CutoffEnabled,
  LateStageTap,
  Interpolation,

  Count
};

constexpr int kParameterCount = static_cast<int>(Parameter::Count);

}  // namespace cloudseed
