#pragma once

#include <stddef.h>

#include "config.h"
#include "memory_pool.h"
#include "parameter.h"
#include "reverb_channel.h"

namespace cloudseed {

// The stereo reverb (CloudSeed's ReverbController): two ReverbChannels with
// an optional mix of the two inputs. Parameters are set as normalized 0..1
// values and scaled to their ranges here, exactly like the plugin does.
//
// Usage:
//   float pool[ReverbController::RequiredPoolFloats(48000)];  // in SDRAM
//   MemoryPool memory;
//   memory.Init(pool, sizeof(pool) / sizeof(pool[0]));
//   ReverbController reverb;
//   reverb.Init(48000, memory);
//   reverb.LoadPreset(presets::kMediumSpace.values);
//   reverb.ClearBuffers();
//   reverb.Process(in_l, in_r, out_l, out_r, 48);
class ReverbController {
 public:
  static constexpr size_t RequiredPoolFloats(int sample_rate) {
    return 2 * ReverbChannel::RequiredPoolFloats(sample_rate);
  }

  // Initializes the reverb for the given sample rate, taking its delay
  // memory from the pool. Call FastSin::Init() once before. Returns false
  // when the pool is too small.
  bool Init(int sample_rate, MemoryPool& pool);

  int sample_rate() const { return sample_rate_; }

  // Sets a parameter from its normalized 0..1 value.
  void SetParameter(Parameter param, double value);
  double GetParameter(Parameter param) const;

  // The value a parameter is scaled to before it reaches the DSP (Hz, ms,
  // seconds, counts, gains or 0/1 for switches).
  double GetScaledParameter(Parameter param) const;

  // Sets all kParameterCount parameters from a preset, then places the
  // delay buffers (see PlaceBuffers). Call ClearBuffers() afterwards.
  void LoadPreset(const double* values);
  void LoadPreset(const float* values);

  // Internal-RAM pools for the delay memory of the loaded preset. The memory
  // from Init() (the SDRAM on the Daisy Seed) holds every buffer at its
  // maximum size; LoadPreset() then moves the buffers that the preset
  // actually processes, sized for the preset, into these pools in the order
  // of their benefit (memory accesses per sample and byte) until the pools
  // are full. The rest stays in the Init() memory. Up to kMaxFastPools pools;
  // hand them over before the first LoadPreset(). A parameter change that
  // lengthens a placed buffer's delay afterwards is limited to the placed
  // size: call PlaceBuffers() and ClearBuffers() again to resize.
  static constexpr int kMaxFastPools = 2;
  void AddFastPool(float* base, size_t floats);

  // Places the buffers for the current parameters; LoadPreset() calls it.
  // The delay memory is undefined afterwards until ClearBuffers().
  void PlaceBuffers();

  // What the loaded preset makes one channel do (both are alike).
  Workload GetWorkload() const { return channel_l_.GetWorkload(); }

  // Diagnostics of the last placement: floats used of fast pool i, its size,
  // and the buffers that did not fit.
  size_t fast_pool_used(int i) const { return fast_pools_[i].used(); }
  size_t fast_pool_size(int i) const { return fast_pools_[i].size(); }
  int fast_pool_count() const { return fast_pool_count_; }
  const MemoryPool& fast_pool(int i) const { return fast_pools_[i]; }
  int unplaced_buffers() const { return unplaced_; }

  void ClearBuffers();

  // Freezes or releases the late reverb of both channels: frozen, new input
  // is excluded and damping is bypassed. Modulation can still change energy.
  void SetFrozen(bool frozen);
  bool frozen() const { return channel_l_.frozen(); }

  // Processes count (1..kMaxBlockSize) samples; in and out may not overlap.
  // Keep count constant between calls: like the legacy plugin, every late
  // line feeds back the previous block. Changing block size changes the sound.
  void Process(const float* in_l, const float* in_r, float* out_l, float* out_r,
               int count);

  // Delay lines in use, after clamping the preset's count to kMaxLines.
  int line_count() const { return channel_l_.line_count(); }
#if CLOUDSEED_STAGED_MEMORY
  void SetStagedProgress(StagedProgress progress) {
    channel_l_.SetStagedProgress(progress);
    channel_r_.SetStagedProgress(progress);
  }
#endif

  // The two channels, for diagnostics and delay-memory staging.
  ReverbChannel& channel(int i) { return i == 0 ? channel_l_ : channel_r_; }
  const ReverbChannel& channel(int i) const {
    return i == 0 ? channel_l_ : channel_r_;
  }

 private:
  double P(Parameter param) const;

  int sample_rate_ = 48000;
  float input_mix_ = 0.f;  // InputMix * 0.5, cached for Process()
  ReverbChannel channel_l_;
  ReverbChannel channel_r_;
  float left_in_[kMaxBlockSize];
  float right_in_[kMaxBlockSize];
  double parameters_[kParameterCount];
  MemoryPool fast_pools_[kMaxFastPools];
  int fast_pool_count_ = 0;
  int unplaced_ = 0;
};

}  // namespace cloudseed
