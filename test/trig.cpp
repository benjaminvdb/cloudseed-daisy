/*
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 */

// Checks dsp/fdlibm_trig.cpp against fdlibm's own sin(), cos() and
// __ieee754_rem_pio2() (newlib, libm/math/s_sin.c, s_cos.c, e_rem_pio2.c,
// k_sin.c, k_cos.c; Sun Microsystems 1993, transcribed below without the
// huge-argument path). Built twice: with the port's fused operations
// replaced by plain ones it must match the source bit for bit; with them
// (as on the Daisy Seed) the results may differ from the unfused source by
// the last bit of the reduced argument, which the kernels turn into at most
// one ulp of the result.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cloudseed/fdlibm_trig.h"

namespace {

int32_t High(double x) {
  uint64_t b;
  std::memcpy(&b, &x, 8);
  return static_cast<int32_t>(b >> 32);
}
uint32_t Low(double x) {
  uint64_t b;
  std::memcpy(&b, &x, 8);
  return static_cast<uint32_t>(b);
}
double FromWords(int32_t high, uint32_t low) {
  const uint64_t b =
      (static_cast<uint64_t>(static_cast<uint32_t>(high)) << 32) | low;
  double x;
  std::memcpy(&x, &b, 8);
  return x;
}

// k_sin.c
const double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
             S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
             S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
// k_cos.c
const double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
             C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
             C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;

}  // namespace

extern "C" double __kernel_sin(double x, double y, int iy) {
  int32_t ix = High(x) & 0x7fffffff;
  if (ix < 0x3e400000) {
    if (static_cast<int>(x) == 0) return x;
  }
  const double z = x * x;
  const double v = z * x;
  const double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
  if (iy == 0) return x + v * (S1 + z * r);
  return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

extern "C" double __kernel_cos(double x, double y) {
  int32_t ix = High(x) & 0x7fffffff;
  if (ix < 0x3e400000) {
    if (static_cast<int>(x) == 0) return 1.0;
  }
  const double z = x * x;
  const double r =
      z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
  if (ix < 0x3FD33333) return 1.0 - (0.5 * z - (z * r - x * y));
  double qx;
  if (ix > 0x3fe90000) {
    qx = 0.28125;
  } else {
    qx = FromWords(ix - 0x00200000, 0);
  }
  const double hz = 0.5 * z - qx;
  const double a = 1.0 - qx;
  return a - (hz - (z * r - x * y));
}

namespace {

// e_rem_pio2.c, medium arguments only.
const int32_t npio2_hw[] = {
    0x3FF921FB, 0x400921FB, 0x4012D97C, 0x401921FB, 0x401F6A7A, 0x4022D97C,
    0x4025FDBB, 0x402921FB, 0x402C463A, 0x402F6A7A, 0x4031475C, 0x4032D97C,
    0x40346B9C, 0x4035FDBB, 0x40378FDB, 0x403921FB, 0x403AB41B, 0x403C463A,
    0x403DD85A, 0x403F6A7A, 0x40407E4C, 0x4041475C, 0x4042106C, 0x4042D97C,
    0x4043A28C, 0x40446B9C, 0x404534AC, 0x4045FDBB, 0x4046C6CB, 0x40478FDB,
    0x404858EB, 0x404921FB,
};
const double half = 0.5, invpio2 = 6.36619772367581382433e-01,
             pio2_1 = 1.57079632673412561417e+00,
             pio2_1t = 6.07710050650619224932e-11,
             pio2_2 = 6.07710050630396597660e-11,
             pio2_2t = 2.02226624879595063154e-21,
             pio2_3 = 2.02226624871116645580e-21,
             pio2_3t = 8.47842766036889956997e-32;

int32_t ieee754_rem_pio2(double x, double* y) {
  double z = 0.0, w, t, r, fn;
  int32_t i, j, n, ix, hx;
  hx = High(x);
  ix = hx & 0x7fffffff;
  if (ix <= 0x3fe921fb) {
    y[0] = x;
    y[1] = 0;
    return 0;
  }
  if (ix < 0x4002d97c) {
    if (hx > 0) {
      z = x - pio2_1;
      if (ix != 0x3ff921fb) {
        y[0] = z - pio2_1t;
        y[1] = (z - y[0]) - pio2_1t;
      } else {
        z -= pio2_2;
        y[0] = z - pio2_2t;
        y[1] = (z - y[0]) - pio2_2t;
      }
      return 1;
    } else {
      z = x + pio2_1;
      if (ix != 0x3ff921fb) {
        y[0] = z + pio2_1t;
        y[1] = (z - y[0]) + pio2_1t;
      } else {
        z += pio2_2;
        y[0] = z + pio2_2t;
        y[1] = (z - y[0]) + pio2_2t;
      }
      return -1;
    }
  }
  if (ix <= 0x413921fb) {
    t = std::fabs(x);
    n = static_cast<int32_t>(t * invpio2 + half);
    fn = static_cast<double>(n);
    r = t - fn * pio2_1;
    w = fn * pio2_1t;
    if (n < 32 && ix != npio2_hw[n - 1]) {
      y[0] = r - w;
    } else {
      uint32_t high;
      j = ix >> 20;
      y[0] = r - w;
      high = static_cast<uint32_t>(High(y[0]));
      i = j - ((high >> 20) & 0x7ff);
      if (i > 16) {
        t = r;
        w = fn * pio2_2;
        r = t - w;
        w = fn * pio2_2t - ((t - r) - w);
        y[0] = r - w;
        high = static_cast<uint32_t>(High(y[0]));
        i = j - ((high >> 20) & 0x7ff);
        if (i > 49) {
          t = r;
          w = fn * pio2_3;
          r = t - w;
          w = fn * pio2_3t - ((t - r) - w);
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
  std::fprintf(stderr, "argument outside the medium range\n");
  std::abort();
}

double fdlibm_sin(double x) {
  double y[2];
  int32_t ix = High(x) & 0x7fffffff;
  if (ix <= 0x3fe921fb) return __kernel_sin(x, 0.0, 0);
  if (ix >= 0x7ff00000) return x - x;
  const int32_t n = ieee754_rem_pio2(x, y);
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

double fdlibm_cos(double x) {
  double y[2];
  int32_t ix = High(x) & 0x7fffffff;
  if (ix <= 0x3fe921fb) return __kernel_cos(x, 0.0);
  if (ix >= 0x7ff00000) return x - x;
  const int32_t n = ieee754_rem_pio2(x, y);
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

uint64_t Bits(double x) {
  uint64_t b;
  std::memcpy(&b, &x, 8);
  return b;
}

uint64_t UlpDistance(double a, double b) {
  const uint64_t x = Bits(a), y = Bits(b);
  if (a == b) return 0;
  if ((x >> 63) != (y >> 63)) return UINT64_MAX;
  return x > y ? x - y : y - x;
}

}  // namespace

int main(int argc, char** argv) {
  const bool fused = argc > 1 && std::strcmp(argv[1], "fused") == 0;
  uint64_t exact = 0, off_by_one = 0, worse = 0, total = 0;
  auto check = [&](double x) {
    const double values[2] = {cloudseed::trig::Sin(x), cloudseed::trig::Cos(x)};
    const double reference[2] = {fdlibm_sin(x), fdlibm_cos(x)};
    for (int k = 0; k < 2; k++) {
      total++;
      const uint64_t d = UlpDistance(values[k], reference[k]);
      if (d == 0) {
        exact++;
      } else if (d == 1) {
        off_by_one++;
      } else {
        worse++;
        if (worse < 5)
          std::fprintf(stderr, "x=%.17g %s port=%.17g fdlibm=%.17g\n", x,
                       k ? "cos" : "sin", values[k], reference[k]);
      }
    }
  };
  // The filters' arguments: 2 pi f / fs for every frequency the presets and
  // pots can produce, densely, plus the boundaries near pi/2 and pi.
  for (int i = 0; i <= 2000000; i++) check(M_PI * i / 2000000.0);
  for (int i = -2000; i <= 2000; i++) {
    check(std::nextafter(M_PI / 2, i < 0 ? 0.0 : 4.0) + 0.0 * i);
    check(FromWords(High(M_PI / 2), static_cast<uint32_t>(Low(M_PI / 2) + i)));
    check(FromWords(High(M_PI), static_cast<uint32_t>(Low(M_PI) + i)));
  }
  // Random arguments over the whole medium range, both signs.
  uint64_t state = 0x9E3779B97F4A7C15ull;
  for (int i = 0; i < 2000000; i++) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    const double u = static_cast<double>(state >> 11) / 9007199254740992.0;
    check((u - 0.5) * 2.0 * 823549.6);  // 2^19 * pi / 2 = 823549.66
  }
  std::printf(
      "%s: %llu values, %llu identical, %llu one ulp apart, %llu worse\n",
      fused ? "fused" : "unfused", (unsigned long long)total,
      (unsigned long long)exact, (unsigned long long)off_by_one,
      (unsigned long long)worse);
  if (worse) return EXIT_FAILURE;
  if (!fused && off_by_one) {
    std::fprintf(stderr,
                 "FAIL: the unfused port must match the source bit for bit\n");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
