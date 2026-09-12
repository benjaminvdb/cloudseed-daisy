#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "fast_sin.h"
#include "utils.h"

namespace cloudseed {

// Delay memory served from tightly coupled memory (see staging.h). While a
// ring buffer is staged, its owner never touches the ring itself: for every
// Process() call it reads a window of the ring that holds every position the
// block can read, and writes the block's samples to a block buffer, which the
// staging manager copies back to the ring afterwards.
struct StagedHead {
  // window[i] holds ring[start - lead + i] for 0 <= i < length, where start
  // is the ring index of the block's first write. A read at delay d of
  // sample k is window[lead - d + k], with lead - d >= 1 and
  // lead - d + count <= length by construction of the window.
  const float* window;
  int lead;
  int length;
  // block[k] receives the sample written to ring[start + k].
  float* block;
};

// The multitap delay's ring runs backwards (sample k of a block goes to
// ring[start - k]); its taps read positions ahead of the write index.
struct StagedTaps {
  // One window of kMaxBlockSize floats per active tap, windows[j][i] holding
  // ring[start + position_j - (kMaxBlockSize - 1) + i], or nullptr for a tap
  // that reads what the block being written wrote (position < kMaxBlockSize,
  // see Staging::Plan): those are served from the input history instead,
  // which holds the last two blocks.
  const float* const* windows;
  // block[i] receives the sample written to ring[start - (kMaxBlockSize - 1) +
  // i].
  float* block;
};

// The reverb reports the groups of staged heads it has finished with in a
// block (ReverbController::SetStagedProgress), so that the staging
// (staging.h) can stream their copies while the rest of the block runs:
// per channel the early section (the pre-delay, the multitap and the early
// diffuser) is group 0 and line l is group 1 + l, the right channel's
// groups offset by kStagedGroups.
constexpr int kStagedGroups = 1 + kMaxLines;
struct StagedProgress {
  void (*fn)(void* context, int group);
  void* context;
  void Report(int group) const {
    if (fn) fn(context, group);
  }
};

// The LFO of a staged head over one block (ModulatedAllpass, ModulatedDelay):
// the runs the block splits into at its LFO updates, and the read position
// of each, the integer delay and the two interpolation gains.
struct StagedLfoPlan {
  // A first run that may be shorter (the update counter carries over from
  // the previous block), then runs of kModulationUpdateRate samples, each
  // starting at an update.
  static constexpr int kMaxRuns = kMaxBlockSize / kModulationUpdateRate + 2;
  // The updates are computed three at a time (see PlanStagedRuns); the
  // arrays hold the surplus of the last group.
  static constexpr int kChains = (kMaxRuns + 2) / 3 * 3;
  int runs;
  int lengths[kMaxRuns];
  int delays[kChains];
  float gains_a[kChains], gains_b[kChains];
  bool first_updated;      // whether run 0 starts at an update
  float phase;             // the LFO phase after the block
  unsigned int processed;  // samples since the last update, after the block
};

// Plans the runs of a block of count samples (at most kMaxBlockSize), from
// the update counter and phase as they were before the block, the LFO
// rate, and the delay (as a float), depth and buffer limit the updates use;
// clamp_zero makes a modulated delay at or below zero one sample
// (ModulatedAllpass::Modulate; ModulatedDelay never modulates below zero).
// The head's Update() would compute the same values per run; a run without
// an update (only the first run can be one) keeps the head's current read
// position, which the caller fills in.
//
// An update is a chain of about twenty dependent operations. The updates of
// a block are independent once their phases are known (one addition each),
// so they are computed three at a time in straight-line code, each step for
// the three before the next, which the compiler's scheduler interleaves;
// the surplus chains of the last group are computed and ignored. There is
// no branch per chain and no float compare: the phase wrap and the two
// clamps compare the values' bits in the integer pipeline (FloatBits), so
// the selects need no flag transfer; the zero clamp of the allpass is a
// floor of +0 (bits >= 1) against a floor of everything for the delay. The
// integer part of the delay comes from truncf (one instruction, vrintz),
// which is (float)(int) bit for bit. The values per update are those of
// Update(). One copy in the image: every staged head calls it once per
// block.
__attribute__((noinline)) inline void PlanStagedRuns(
    int count, unsigned int processed, float phase, float rate, float delay,
    float amount, float max_delay, bool clamp_zero, StagedLfoPlan* plan) {
  constexpr int kChains = StagedLfoPlan::kChains;
  // The phases at the updates: one dependent addition per update. The wrap
  // is Update()'s `while (phase > 1.f)`; the phase is never negative, and a
  // negative one would compare below one either way.
  constexpr int32_t kOneBits = 0x3f800000;
  float phases[kChains];
  const float step = rate * kModulationUpdateRate;
  int runs = 0;
  int done = 0;
  bool first_updated = false;
  if (count > 0 && processed >= static_cast<unsigned int>(kModulationUpdateRate) &&
      count % kModulationUpdateRate == 0) {
    // The steady state on the module (blocks of kMaxBlockSize): every run
    // starts at an update and is a full run.
    runs = count / kModulationUpdateRate;
    first_updated = true;
    for (int r = 0; r < runs; r++) {
      phase += step;
      while (FloatBits(phase) > kOneBits) phase -= 1.f;
      phases[r] = phase;
      plan->lengths[r] = kModulationUpdateRate;
    }
    processed = static_cast<unsigned int>(kModulationUpdateRate);
    done = count;
  }
  while (done < count) {
    if (processed >= static_cast<unsigned int>(kModulationUpdateRate)) {
      phase += step;
      while (FloatBits(phase) > kOneBits) phase -= 1.f;
      processed = 0;
      if (runs == 0) first_updated = true;
    }
    int n = kModulationUpdateRate - static_cast<int>(processed);
    const int left = count - done;
    if (n > left) n = left;
    phases[runs] = phase;
    plan->lengths[runs] = n;
    runs++;
    processed += static_cast<unsigned int>(n);
    done += n;
  }
  const int groups = (runs + 2) / 3 * 3;
  for (int r = runs; r < groups; r++) phases[r] = phase;
  // `t > 0 ? t : 1` for the allpass (bits >= 1), no clamp for the delay;
  // `t > max ? max : t` for both, max_delay being positive and t at or
  // above +0 after the first clamp or, for the delay, negative only when
  // Update() would compute the same negative delay.
  const int32_t min_bits = clamp_zero ? 1 : INT32_MIN;
  const int32_t max_bits = FloatBits(max_delay);
  for (int r = 0; r < groups; r += 3) {
    float t0 = FastSin::Get(phases[r]);
    float t1 = FastSin::Get(phases[r + 1]);
    float t2 = FastSin::Get(phases[r + 2]);
    t0 = delay + amount * t0;
    t1 = delay + amount * t1;
    t2 = delay + amount * t2;
    t0 = FloatBits(t0) >= min_bits ? t0 : 1.f;
    t1 = FloatBits(t1) >= min_bits ? t1 : 1.f;
    t2 = FloatBits(t2) >= min_bits ? t2 : 1.f;
    t0 = FloatBits(t0) > max_bits ? max_delay : t0;
    t1 = FloatBits(t1) > max_bits ? max_delay : t1;
    t2 = FloatBits(t2) > max_bits ? max_delay : t2;
    const float f0 = truncf(t0);
    const float f1 = truncf(t1);
    const float f2 = truncf(t2);
    const float p0 = t0 - f0;
    const float p1 = t1 - f1;
    const float p2 = t2 - f2;
    plan->delays[r] = static_cast<int>(f0);
    plan->delays[r + 1] = static_cast<int>(f1);
    plan->delays[r + 2] = static_cast<int>(f2);
    plan->gains_a[r] = 1.f - p0;
    plan->gains_a[r + 1] = 1.f - p1;
    plan->gains_a[r + 2] = 1.f - p2;
    plan->gains_b[r] = p0;
    plan->gains_b[r + 1] = p1;
    plan->gains_b[r + 2] = p2;
  }
  plan->runs = runs;
  plan->first_updated = first_updated;
  plan->phase = phase;
  plan->processed = processed;
}

}  // namespace cloudseed
