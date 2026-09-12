/*
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 */

// newlib's sin() and cos() for arguments up to 2^19 * pi / 2, bit for bit.
//
// newlib's libm computes sin(x) and cos(x) (fdlibm's s_sin.c and s_cos.c,
// Sun Microsystems 1993) by reducing x to y0 + y1 = x - n * pi / 2 with
// __ieee754_rem_pio2() (e_rem_pio2.c) and evaluating __kernel_sin() and
// __kernel_cos() (k_sin.c, k_cos.c). The reduction has a path for medium
// arguments (|x| <= 2^19 * pi / 2) and one for huge arguments that calls
// __kernel_rem_pio2() with a 476-digit table of 2 / pi, which the linker
// then pulls in. This file is the medium path of the reduction, and it
// calls libm's own kernels; the huge path is left out.
//
// The arithmetic follows the libm object of the toolchain (GCC 16.2,
// arm-none-eabi-newlib, libm.a member libm_a-e_rem_pio2.o) rather than the
// source: the library was compiled with fused multiply-adds, and which
// operations it fused decides the last bit of the reduced argument. The
// fused operations are written as fma() below and this file is compiled
// with -ffp-contract=off (see the Makefile), so that nothing else is fused:
//
//   n  = (int)(t * invpio2 + 0.5)                  vfma
//   r  = t - fn * pio2_1                            vfms
//   w  = fn * pio2_1t; y0 = r - w                   vmul, vsub
//   2nd round: r' = t - fn * pio2_2                 vfms
//              u  = (t - r') - fn * pio2_2          vsub, vfms
//              w' = fn * pio2_2t - u                vfnms
//   3rd round: the same with pio2_3 and pio2_3t
//   y1 = (r - y0) - w                               vsub, vsub
//
// Only the STM32 build uses this: elsewhere the platform's sin() and cos().
#include "fdlibm_trig.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#if defined(__ARM_EABI__) || defined(CLOUDSEED_TRIG_FDLIBM)

extern "C" double __kernel_sin(double x, double y, int iy);
extern "C" double __kernel_cos(double x, double y);

namespace cloudseed {
namespace trig {
namespace {

// High word of a double.
inline int32_t HighWord(double x) {
  uint64_t bits;
  memcpy(&bits, &x, sizeof(bits));
  return static_cast<int32_t>(bits >> 32);
}

// e_rem_pio2.c: high words of n * pi / 2 for n = 1..32 (arguments there
// need the longer reduction).
const int32_t kNpio2Hw[32] = {
    0x3FF921FB, 0x400921FB, 0x4012D97C, 0x401921FB, 0x401F6A7A, 0x4022D97C,
    0x4025FDBB, 0x402921FB, 0x402C463A, 0x402F6A7A, 0x4031475C, 0x4032D97C,
    0x40346B9C, 0x4035FDBB, 0x40378FDB, 0x403921FB, 0x403AB41B, 0x403C463A,
    0x403DD85A, 0x403F6A7A, 0x40407E4C, 0x4041475C, 0x4042106C, 0x4042D97C,
    0x4043A28C, 0x40446B9C, 0x404534AC, 0x4045FDBB, 0x4046C6CB, 0x40478FDB,
    0x404858EB, 0x404921FB,
};

constexpr double kInvPio2 = 6.36619772367581382433e-01;  // 53 bits of 2/pi
constexpr double kPio2_1 = 1.57079632673412561417e+00;  // first 33 bits of pi/2
constexpr double kPio2_1t = 6.07710050650619224932e-11;  // pi/2 - pio2_1
constexpr double kPio2_2 = 6.07710050630396597660e-11;   // second 33 bits
constexpr double kPio2_2t =
    2.02226624879595063154e-21;  // pi/2 - (pio2_1 + pio2_2)
constexpr double kPio2_3 = 2.02226624871116645580e-21;  // third 33 bits
constexpr double kPio2_3t =
    8.47842766036889956997e-32;  // pi/2 - (pio2_1 + pio2_2 + pio2_3)

#ifdef CLOUDSEED_TRIG_UNFUSED
// For the host check against the fdlibm source, where nothing is fused.
inline double Fma(double a, double b, double c) { return a * b + c; }
#else
inline double Fma(double a, double b, double c) { return fma(a, b, c); }
#endif

// x rem pi/2 in y[0] + y[1]; returns n. Valid for |x| <= 2^19 * pi / 2.
int RemPio2(double x, double* y) {
  const int32_t hx = HighWord(x);
  const int32_t ix = hx & 0x7fffffff;
  if (ix <= 0x3fe921fb) {  // |x| ~<= pi/4, no reduction
    y[0] = x;
    y[1] = 0;
    return 0;
  }
  if (ix < 0x4002d97c) {  // |x| < 3pi/4, n = +-1
    if (hx > 0) {
      double z = x - kPio2_1;
      if (ix != 0x3ff921fb) {  // 33+53 bit pi is good enough
        y[0] = z - kPio2_1t;
        y[1] = (z - y[0]) - kPio2_1t;
      } else {  // near pi/2, use 33+33+53 bit pi
        z -= kPio2_2;
        y[0] = z - kPio2_2t;
        y[1] = (z - y[0]) - kPio2_2t;
      }
      return 1;
    }
    double z = x + kPio2_1;
    if (ix != 0x3ff921fb) {
      y[0] = z + kPio2_1t;
      y[1] = (z - y[0]) + kPio2_1t;
    } else {
      z += kPio2_2;
      y[0] = z + kPio2_2t;
      y[1] = (z - y[0]) + kPio2_2t;
    }
    return -1;
  }
  // |x| <= 2^19 * pi / 2, medium size.
  double t = fabs(x);
  const int32_t n = static_cast<int32_t>(Fma(t, kInvPio2, 0.5));
  const double fn = static_cast<double>(n);
  double r = Fma(-fn, kPio2_1, t);
  double w = fn * kPio2_1t;  // first round good to 85 bits
  y[0] = r - w;
  if (!(n < 32 && ix != kNpio2Hw[n - 1])) {
    const int32_t j = ix >> 20;
    int32_t i = j - ((HighWord(y[0]) >> 20) & 0x7ff);
    if (i > 16) {  // second round, good to 118 bits
      t = r;
      r = Fma(-fn, kPio2_2, t);
      double u = t - r;
      u = Fma(-fn, kPio2_2, u);
      w = Fma(fn, kPio2_2t, -u);
      y[0] = r - w;
      i = j - ((HighWord(y[0]) >> 20) & 0x7ff);
      if (i > 49) {  // third round, 151 bits: covers all cases
        t = r;
        r = Fma(-fn, kPio2_3, t);
        u = t - r;
        u = Fma(-fn, kPio2_3, u);
        w = Fma(fn, kPio2_3t, -u);
        y[0] = r - w;
      }
    }
  }
  y[1] = (r - y[0]) - w;
  if (hx < 0) {
    y[0] = -y[0];
    y[1] = -y[1];
    return -n;
  }
  return n;
}

}  // namespace

double Sin(double x) {
  const int32_t ix = HighWord(x) & 0x7fffffff;
  if (ix <= 0x3fe921fb) return __kernel_sin(x, 0.0, 0);  // |x| ~< pi/4
  if (ix >= 0x7ff00000) return x - x;                    // inf or NaN
  double y[2];
  const int n = RemPio2(x, y);
  switch (n & 3) {
    case 0:
      return __kernel_sin(y[0], y[1], 1);
    case 1:
      return __kernel_cos(y[0], y[1]);
    case 2:
      return -__kernel_sin(y[0], y[1], 1);
    default:
      return -__kernel_cos(y[0], y[1]);
  }
}

double Cos(double x) {
  const int32_t ix = HighWord(x) & 0x7fffffff;
  if (ix <= 0x3fe921fb) return __kernel_cos(x, 0.0);
  if (ix >= 0x7ff00000) return x - x;
  double y[2];
  const int n = RemPio2(x, y);
  switch (n & 3) {
    case 0:
      return __kernel_cos(y[0], y[1]);
    case 1:
      return -__kernel_sin(y[0], y[1], 1);
    case 2:
      return -__kernel_cos(y[0], y[1]);
    default:
      return __kernel_sin(y[0], y[1], 1);
  }
}

}  // namespace trig
}  // namespace cloudseed

#else

namespace cloudseed {
namespace trig {
double Sin(double x) { return sin(x); }
double Cos(double x) { return cos(x); }
}  // namespace trig
}  // namespace cloudseed

#endif
