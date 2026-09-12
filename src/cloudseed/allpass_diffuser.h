#pragma once

#include <math.h>

#include "config.h"
#include "memory_pool.h"
#include "modulated_allpass.h"
#include "sha_random.h"
#include "utils.h"

namespace cloudseed {

// Up to kMaxStages modulated allpass filters in series (CloudSeed's
// AllpassDiffuser). The delay, modulation depth and rate of every stage are
// scaled by seeded random values.
class AllpassDiffuser {
 public:
  bool Init(MemoryPool& pool, double sample_rate, int buffer_size) {
    sample_rate_ = sample_rate;
    utils::Zero(buffers_[0], kMaxBlockSize);
    utils::Zero(buffers_[1], kMaxBlockSize);
    bool ok = true;
    for (int i = 0; i < kMaxStages; i++) {
      ok =
          filters_[i].Init(pool, buffer_size, 100, utils::NextModPhase()) && ok;
    }
    cross_seed_ = 0.0;
    seed_ = 23456;
    delay_ = 0;
    mod_amount_ = 0.0;
    mod_rate_ = 0.0;
    UpdateSeeds();
    stages_ = 1;
    SetModRate(mod_rate_);
    return ok;
  }

  void SetSeed(int seed) {
    seed_ = seed;
    UpdateSeeds();
  }

  void SetCrossSeed(double cross_seed) {
    cross_seed_ = cross_seed;
    UpdateSeeds();
  }

  bool modulation_enabled() const { return filters_[0].modulation_enabled; }

  void SetModulationEnabled(bool value) {
    for (int i = 0; i < kMaxStages; i++) filters_[i].modulation_enabled = value;
  }

  void SetInterpolationEnabled(bool enabled) {
    for (int i = 0; i < kMaxStages; i++)
      filters_[i].interpolation_enabled = enabled;
  }

  // The last stage's output of the last Process() call.
  const float* output() const { return buffers_[(stages_ - 1) & 1]; }

  void SetDelay(int delay_samples) {
    delay_ = delay_samples;
    Update();
  }

  void SetFeedback(double feedback) {
    for (int i = 0; i < kMaxStages; i++)
      filters_[i].feedback = static_cast<float>(feedback);
  }

  void SetModAmount(double amount) {
    mod_amount_ = amount;
    for (int i = 0; i < kMaxStages; i++) {
      filters_[i].mod_amount = static_cast<float>(
          amount * (0.85 + 0.3 * seed_values_[kMaxStages + i]));
    }
  }

  void SetModRate(double rate) {
    mod_rate_ = rate;
    for (int i = 0; i < kMaxStages; i++) {
      filters_[i].mod_rate = static_cast<float>(
          rate * (0.85 + 0.3 * seed_values_[kMaxStages * 2 + i]) /
          sample_rate_);
    }
  }

  void set_stages(int stages) {
    if (stages < 1) stages = 1;
    if (stages > kMaxStages) stages = kMaxStages;
    stages_ = stages;
  }
  int stages() const { return stages_; }

  // Stage i (0 <= i < kMaxStages), for ReverbController::PlaceBuffers().
  ModulatedAllpass& stage(int i) { return filters_[i]; }
  const ModulatedAllpass& stage(int i) const { return filters_[i]; }

  // The stages alternate between two block buffers; only the last stage's
  // output is read afterwards.
  void Process(const float* input, int count) {
    filters_[0].Process(input, buffers_[0], count);
    for (int i = 1; i < stages_; i++) {
      filters_[i].Process(buffers_[(i - 1) & 1], buffers_[i & 1], count);
    }
  }

  void ClearBuffers() {
    for (int i = 0; i < kMaxStages; i++) filters_[i].ClearBuffers();
    utils::Zero(buffers_[0], kMaxBlockSize);
    utils::Zero(buffers_[1], kMaxBlockSize);
  }

 private:
  void Update() {
    for (int i = 0; i < kMaxStages; i++) {
      const double r = seed_values_[i];
      const double d = utils::Pow10(r) * 0.1;  // 0.1 ... 1.0
      int delay = static_cast<int>(delay_ * d);
      const int max_delay = filters_[i].buffer_size() - 2;
      if (delay > max_delay) delay = max_delay;
      filters_[i].sample_delay = delay;
    }
  }

  void UpdateSeeds() {
    sha_random::Generate(seed_, kMaxStages * 3, cross_seed_, seed_values_);
    Update();
    // Seeds scale all three properties. Presets load seeds after modulation;
    // updating only delays leaves depth/rate dependent on the prior preset.
    SetModAmount(mod_amount_);
    SetModRate(mod_rate_);
  }

  ModulatedAllpass filters_[kMaxStages];
  float buffers_[2][kMaxBlockSize];
  double sample_rate_ = 48000.0;
  int delay_ = 0;
  double mod_rate_ = 0.0;
  double mod_amount_ = 0.0;
  double seed_values_[kMaxStages * 3];
  int seed_ = 23456;
  double cross_seed_ = 0.0;
  int stages_ = 1;
};

}  // namespace cloudseed
