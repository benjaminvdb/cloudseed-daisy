#pragma once

#include <math.h>

namespace cloudseed {

// sin(2 * pi * phase) from a table, used by the delay-modulation LFOs. The
// plugin's table has 32768 entries and truncates the phase; this one has 4096
// entries and interpolates, which is more accurate and 8 times smaller.
class FastSin {
 public:
  static void Init();

  // phase in 0..1
  static inline float Get(float phase) {
    const float p = phase * kSize;
    // The integer part as a float in one instruction on the FPv5 (vrintz);
    // the fraction no longer waits for the index's trip through a core
    // register and back (TECHNICAL.md, "DSP kernels"). For phase in 0..1 the
    // index is never clamped and f is bit-identical to p - (float)i.
    const float fi = truncf(p);
    int i = static_cast<int>(fi);
    if (i < 0) i = 0;
    if (i > kSize) i = kSize;
    const float f = p - fi;
    return table_[i] + f * (table_[i + 1] - table_[i]);
  }

 private:
  static constexpr int kSize = 4096;
  static float table_[kSize + 2];
};

}  // namespace cloudseed
