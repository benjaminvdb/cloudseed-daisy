#pragma once

#include <math.h>

namespace cloudseed {

// CloudSeed maps its 0..1 parameters to their ranges through lookup tables
// (AudioLib::ValueTables) with 4001 entries: "ResponseNOct" doubles N times
// over the range, "ResponseNDec" multiplies by 10 N times, both normalized so
// that 0 -> 0 and 1 -> 1. The tables are pure functions of their index, so
// they are computed here instead of stored; the index quantization of the
// tables is kept so that the results match the plugin. b^x is computed as
// exp(x ln b): pow() would link 4 KB more of libm into the firmware, and the
// fidelity test shows no difference in the resulting delays and gains.
inline double Response(double value, double b, double ln_b) {
  int idx = static_cast<int>(value * 4000.999);
  if (idx < 0) idx = 0;
  if (idx > 4000) idx = 4000;
  const double x = idx / 4000.0;
  return (exp(x * ln_b) - 1.0) / (b - 1.0);
}

inline double ResponseOct(double value, int octaves) {
  return Response(value, static_cast<double>(1 << octaves), octaves * M_LN2);
}

inline double ResponseDec(double value, int decades) {
  double b = 1.0;
  for (int i = 0; i < decades; i++) b *= 10.0;
  return Response(value, b, decades * M_LN10);
}

}  // namespace cloudseed
