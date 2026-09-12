#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace cloudseed {

// The bits of a float as a signed integer. For values that are not NaN,
// x > 0 is FloatBits(x) >= 1, and for x and y both at or above +0 (or both
// at or below -0), x > y is FloatBits(x) > FloatBits(y); with the sign bit
// masked off, the bits order like the magnitude. The comparisons built on
// this run in the integer pipeline, whose flags a VSEL uses directly, where
// a float compare needs a flag transfer (VMRS) that waits for it
// (TECHNICAL.md, "DSP kernels").
static inline int32_t FloatBits(float x) {
  int32_t bits;
  memcpy(&bits, &x, sizeof bits);
  return bits;
}

// Clamps to 0..1 by the bits of the value: values at or above +0 order like
// their bits, and everything below +0 (including -0) becomes 0.
static inline float ClampUnit(float x) {
  x = FloatBits(x) < 0 ? 0.f : x;
  return FloatBits(x) > 0x3f800000 ? 1.f : x;
}

namespace utils {

inline void Zero(float* buffer, int len) {
  for (int i = 0; i < len; i++) buffer[i] = 0.f;
}

// A plain loop: newlib-nano's memcpy, which GCC would substitute for the
// loop, copies these short block buffers byte by byte on the Cortex-M7.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-tree-loop-distribute-patterns")))
#endif
inline void Copy(const float* source, float* dest, int len) {
  for (int i = 0; i < len; i++) dest[i] = source[i];
}

inline void Gain(float* buffer, float gain, int len) {
  for (int i = 0; i < len; i++) buffer[i] *= gain;
}

// A prefetch hint may target unmapped memory, but forming its C++ pointer
// must still stay within the array. Skip hints crossing a ring boundary.
template <int write = 0>
inline void Prefetch(const float* buffer, int index, int size) {
  if (static_cast<unsigned int>(index) < static_cast<unsigned int>(size))
    __builtin_prefetch(buffer + index, write);
}

// 10^(db/20) as exp(); pow() would link 4 KB more of libm into the firmware.
inline double Db2Gain(double db) { return exp(db * (M_LN10 / 20.0)); }

// 10^x for 0 <= x <= 1 (the seeded delay and gain scalings).
inline double Pow10(double x) { return exp(x * M_LN10); }

// Start phase of a modulation LFO. The plugin draws it from rand(); this port
// uses a fixed pseudo-random sequence so that every build behaves the same.
// ReverbController::Init() restarts the sequence, so every reverb instance
// gets the same phases.
inline uint32_t& ModPhaseState() {
  static uint32_t state = 0x2545F491u;
  return state;
}

inline void ResetModPhaseSequence() { ModPhaseState() = 0x2545F491u; }

inline float NextModPhase() {
  uint32_t& state = ModPhaseState();
  state = state * 1664525u + 1013904223u;
  return 0.01f + 0.98f * static_cast<float>(state >> 8) / 16777216.f;
}

}  // namespace utils
}  // namespace cloudseed
