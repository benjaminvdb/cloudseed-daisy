# Changelog

Notable changes to this library, newest first. The format follows [Keep a
Changelog](https://keepachangelog.com/en/1.1.0/), and the library follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html) over the interface
described in [README.md](README.md#-versioning). While the version is below
1.0.0 the interface is not frozen: a 0.x minor release may change it, and a
change that will need one is called out here.

## [Unreleased]

Nothing yet.

## [0.1.0] - 2026-09-13

The first public release, complete but not yet frozen. Everything below works
and is tested; what it is *called* may still move before 1.0.0, once the
firmware has been through other people's projects and boards.

### Added

- **The reverb kernel** (`cloudseed::ReverbController`), a single-precision port
  of [Cloud Seed](https://github.com/ValdemarOrn/CloudSeed) kept expression for
  expression against the plugin's own kernel, with no dependency on libDaisy.
  Output sits 82 to 147 dB below the reference with modulation off.
- **Ten programs** (`cloudseed::presets`): Cloud Seed's nine factory presets and
  Cloud Seed 2's Dark Plate, each at its full size, up to twelve late delay
  lines per channel.
- **The Daisy engine** (`cloudseed_daisy::Engine`): program loading off the
  audio interrupt, per-block CPU measurement, overload recovery that reloads a
  program one delay line lighter and fades the wet signal back in, transport
  fault recovery, freeze, and smooth parameter changes.
- **Delay-memory placement and staging**: per-program placement across the
  STM32H750's AXI SRAM, D2 SRAM and SDRAM, with the MDMA streaming each block's
  windows through the ITCM and DTCM. Staged and direct paths agree bit for bit.
- **The Seed's system settings** (`cloudseed_daisy/seed_system.h`): the 480 MHz
  boost check, the SDRAM refresh correction, SDRAM timings, and the SRAM cache
  policies. None of them is specific to this reverb.
- **`cloudseed.mk`**, a three-line build integration for a libDaisy project,
  with twelve build options and no DaisySP dependency.
- **A profiling build** (`CLOUDSEED_PROFILE=1`) reporting per-section load over
  any sink, with the image's CRC-32.
- **`src/cloudseed_daisy/version.h`**, the library's version as preprocessor
  macros, with `CLOUDSEED_DAISY_VERSION_AT_LEAST`.
- **The host test suites** ([`test/`](test/)) and the CI that runs them: the DSP
  and staging under ASan and UBSan, the engine against libDaisy's
  `CpuLoadMeter`, the MDMA transport at register level, `sin`/`cos` against
  fdlibm over 8,024,008 values, fidelity against the plugin, the build
  integration's expanded compiler recipes, the README's own quick start built as
  a submodule consumer, and the firmware's flash and RAM against a budget.

### Known limitations

- **The interface is not frozen.** 1.0.0 follows once the first users have had a
  go at it; until then a 0.x minor release may rename or change a call.
- The performance figures in the README come from reference image `623fe82a`, an
  earlier firmware. They have not been re-measured on the current code, and Dark
  Plate has not been measured at all. See
  [`docs/release-acceptance.md`](docs/release-acceptance.md).
- The observed peaks are measurements, not a worst-case timing bound.
- One `Engine` per application: its DSP, pools and transport are shared static
  storage.

[Unreleased]:
https://github.com/benjaminvdb/cloudseed-daisy/compare/v0.1.0...HEAD [0.1.0]:
https://github.com/benjaminvdb/cloudseed-daisy/releases/tag/v0.1.0
