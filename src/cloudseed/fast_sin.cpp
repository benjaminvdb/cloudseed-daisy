#include "fast_sin.h"

#include <math.h>

#include "config.h"
#include "fdlibm_trig.h"

namespace cloudseed {

float CLOUDSEED_TABLE_SECTION FastSin::table_[FastSin::kSize + 2];

// The table from the port's sin() (fdlibm_trig.h): the same values on the
// host and on the module, which computed them with newlib's sinf() before
// (a different function from the host's), and no sinf() in the image.
void FastSin::Init() {
  for (int i = 0; i < kSize + 2; i++) {
    table_[i] = static_cast<float>(
        trig::Sin(2.0 * M_PI * static_cast<double>(i) / kSize));
  }
}

}  // namespace cloudseed
