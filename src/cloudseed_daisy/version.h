// cloudseed-daisy: the library's version.
//
// Preprocessor only: no include, no libDaisy, nothing to link. A firmware
// that uses the kernel alone can include this header too.
//
// The version applies to the documented interface — the engine
// (cloudseed_daisy/engine.h), the reverb (cloudseed/reverb_controller.h),
// the programs (cloudseed/presets.h), the system settings
// (cloudseed_daisy/seed_system.h) and the build options cloudseed.mk
// accepts. See README.md, "Versioning", for what a major, minor and patch
// release may change.
//
// This is a 0.x version: the interface is not frozen yet, and a 0.x minor
// release may change it. 1.0.0 is what settles it.
//
// This file is the version: the release workflow refuses a tag that
// disagrees with it.

#ifndef CLOUDSEED_DAISY_VERSION_H
#define CLOUDSEED_DAISY_VERSION_H

#define CLOUDSEED_DAISY_VERSION_MAJOR 0
#define CLOUDSEED_DAISY_VERSION_MINOR 1
#define CLOUDSEED_DAISY_VERSION_PATCH 0

#define CLOUDSEED_DAISY_VERSION_STRING "0.1.0"

// Encoded as major * 1000000 + minor * 1000 + patch, so that minor and patch
// have room to three digits and the whole compares as one integer.
#define CLOUDSEED_DAISY_VERSION_ENCODE(major, minor, patch) \
  ((major) * 1000000 + (minor) * 1000 + (patch))

#define CLOUDSEED_DAISY_VERSION                                \
  CLOUDSEED_DAISY_VERSION_ENCODE(CLOUDSEED_DAISY_VERSION_MAJOR, \
                                 CLOUDSEED_DAISY_VERSION_MINOR, \
                                 CLOUDSEED_DAISY_VERSION_PATCH)

// For a firmware that must build against more than one release:
//
//   #if CLOUDSEED_DAISY_VERSION_AT_LEAST(1, 1, 0)
//     engine.SomethingAddedIn_1_1();
//   #endif
#define CLOUDSEED_DAISY_VERSION_AT_LEAST(major, minor, patch) \
  (CLOUDSEED_DAISY_VERSION >= CLOUDSEED_DAISY_VERSION_ENCODE(major, minor, patch))

#endif  // CLOUDSEED_DAISY_VERSION_H
