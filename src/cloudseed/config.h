#pragma once

#include <stddef.h>

namespace cloudseed {

// Most late-reverb delay lines per channel. The plugin has 12; every line
// costs a modulated delay, up to 8 allpass stages and three filters per
// sample, so this is the main CPU (and memory) knob of the port. Presets that
// ask for more lines are clamped to this number; the firmware sets its own
// count per program below it.
#ifndef CLOUDSEED_MAX_LINES
#define CLOUDSEED_MAX_LINES 12
#endif
constexpr int kMaxLines = CLOUDSEED_MAX_LINES;

// The plugin's delay-line count; the per-line random seeds are generated for
// this many lines so that the first kMaxLines lines match the plugin.
constexpr int kPluginLineCount = 12;
static_assert(kMaxLines >= 1 && kMaxLines <= kPluginLineCount,
              "CLOUDSEED_MAX_LINES must be between 1 and 12");

constexpr int kMaxStages = 8;  // allpass stages per diffuser
constexpr int kMaxTaps = 50;   // taps of the early-reflection multitap delay

// Largest number of samples per Process() call.
constexpr int kMaxBlockSize = 48;

// Samples between two updates of the delay-modulation LFOs.
constexpr int kModulationUpdateRate = 8;

// Whether the delay memory can be served from staging memory (staged_io.h,
// staging.h). A build without staging compiles the staged paths out and
// keeps the ring paths optimized for speed; with staging, the ring paths
// only serve the few rings too short for a window and are compiled for size.
#ifndef CLOUDSEED_STAGED_MEMORY
#define CLOUDSEED_STAGED_MEMORY 1
#endif
#if CLOUDSEED_STAGED_MEMORY
#define CLOUDSEED_RING_PATH __attribute__((noinline, cold))
#else
#define CLOUDSEED_RING_PATH __attribute__((noinline))
#endif

// Section attribute of the DSP's lookup tables (FastSin); the Daisy firmware
// puts them in the DTCM, which is not cached, with the reverb's state.
#ifndef CLOUDSEED_TABLE_SECTION
#define CLOUDSEED_TABLE_SECTION
#endif

// Parts of the processing that a profiling build times separately. The DSP
// marks the start of each part with CLOUDSEED_PROFILE_SECTION(); a build
// with CLOUDSEED_PROFILE defines ProfileSection() to attribute the time
// since the previous mark to the previous part. Without it the marks
// compile to nothing.
enum ProfileSectionId {
  kProfileOther = 0,  // the audio callback outside the reverb
  kProfileInput,      // input filters and gate
  kProfilePreDelay,
  kProfileMultitap,  // early reflections
  kProfileEarlyDiffuser,
  kProfileLineMix,    // feedback mix of a late line
  kProfileLineDelay,  // modulated delay of a late line
  kProfileLineDiffuser,
  kProfileLineFilters,
  kProfileOutput,       // sum of the lines and the output mix
  kProfileControls,     // firmware: pots, CVs, program selection
  kProfileParameters,   // firmware: parameter updates from the pots
  kProfileMix,          // firmware: soft clip and dry/wet crossfade
  kProfileStaging,      // staging: building the transport's list
  kProfileStagingWait,  // staging: waiting for the transport
  kProfileSectionCount
};
#if defined(CLOUDSEED_PROFILE) && CLOUDSEED_PROFILE
// Returns the section that was current.
int ProfileSection(int section);
#define CLOUDSEED_PROFILE_SECTION(section) ::cloudseed::ProfileSection(section)
#else
#define CLOUDSEED_PROFILE_SECTION(section) ((void)0)
#endif

}  // namespace cloudseed
