#pragma once

namespace cloudseed {
namespace trig {

// sin(x) and cos(x) for |x| <= 2^19 * pi / 2 (the filters call them with
// 0 <= x <= pi), computed on the Daisy Seed exactly as newlib's libm
// computes them, without linking libm's sin() and cos(): those bring the
// reduction for huge arguments (__kernel_rem_pio2 and its tables, 2.5 KB of
// flash) that no argument here needs. See fdlibm_trig.cpp. On other
// platforms these are the libm functions.
double Sin(double x);
double Cos(double x);

}  // namespace trig
}  // namespace cloudseed
