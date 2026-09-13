# ☁️ cloudseed-daisy 🌼

cloudseed-daisy runs [Cloud Seed](https://github.com/ValdemarOrn/CloudSeed),
Valdemar Erlingsson's algorithmic reverb, on the [Electro-Smith Daisy
Seed](https://daisy.audio/products/seed3).

[![Tests](https://github.com/benjaminvdb/cloudseed-daisy/actions/workflows/test.yml/badge.svg)](https://github.com/benjaminvdb/cloudseed-daisy/actions/workflows/test.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform: Daisy Seed](https://img.shields.io/badge/platform-Daisy%20Seed%20%C2%B7%20STM32H750-8a2be2.svg)](https://daisy.audio/products/seed3)
[![Standard: C++14](https://img.shields.io/badge/C%2B%2B-14-00599c.svg)](#requirements)

Cloud Seed is an MIT-licensed VST plugin built "for emulating huge, endless
spaces and modulated echoes". This library ports its C++ kernel to single
precision and wraps it in an engine for the Seed's STM32H750, which runs the
plugin's programs at their full size: all twelve delay lines of the largest one,
with per-block CPU measurement and recovery when a block runs over.

Ten programs ship with it. Nine are Cloud Seed's own factory presets; the tenth,
Dark Plate, comes from the MIT-licensed
[core](https://github.com/GhostNoteAudio/CloudSeedCore) of its successor, [Ghost
Note Audio](https://ghostnoteaudio.uk/)'s Cloud Seed 2.

A project picks it up by including `cloudseed.mk` where it used to include
libDaisy's core Makefile. DaisySP is not needed.

The performance figures below were measured on a Seed, on a build of this
library that differs from the current source only by its version header and the
notices. [`docs/release-acceptance.md`](docs/release-acceptance.md) is the
checklist a release is measured with, since CI can build this firmware but
cannot run it.

[TECHNICAL.md](TECHNICAL.md) is the engineering reference: the architecture, the
numerical decisions, the optimization history with its measurements, the
verification, and every defect found and corrected along the way.

## 🎧 Hear it

Every clip plays the same dry phrase, a strummed guitar chord, a snare and a
piano note left ringing, then that phrase through one program. One gain is
applied to all ten, so a program that is louder than another sounds louder here
too, and each runs at its own dry/wet balance.

**Chorus Delay.** Twelve lines used as modulated echoes rather than as a room.

https://github.com/user-attachments/assets/058be392-9ca3-4e4f-8664-725a7e5d4507

<details>
<summary><b>The other nine programs</b></summary>

**Small Room.** Three lines, a one-second tail.

https://github.com/user-attachments/assets/12d6ba9e-c94d-4374-8ac6-474f61f2979b

**Medium Space.** Three lines, a two-second tail, the everyday room.

https://github.com/user-attachments/assets/9cf6fa96-1c63-4339-9804-9893a17e9734

**Noise in the Hallway.** Eight lines, four seconds, close and narrow.

https://github.com/user-attachments/assets/18685d83-567d-4f5c-9182-3d9e9445b6f1

**Hyperplane.** Nine lines and twenty-two seconds, the widest of the nine
factory programs.

https://github.com/user-attachments/assets/781e48a0-3076-4c11-8cc1-859831ceda39

**Rubi-Ka Fields.** Four lines, fourteen seconds.

https://github.com/user-attachments/assets/01ae1c47-2390-4872-a9eb-b4f5a7e8134e

**Through the Looking Glass.** Twelve lines and twenty-six seconds of tail. The
largest program, and the one the CPU budget was built for.

https://github.com/user-attachments/assets/b8815225-e07d-4579-9295-8e0e1b878c90

**The 90s Are Back.** Nine lines, four seconds.

https://github.com/user-attachments/assets/21e2f431-b878-40fd-b97c-7b5d29611bb7

**Dull Echoes.** Twelve lines, a short tail of discrete echoes.

https://github.com/user-attachments/assets/7cc55168-2b4a-4b64-9559-0f159aba8b85

**Dark Plate.** Twelve lines, fourteen seconds, the program from Cloud Seed 2.

https://github.com/user-attachments/assets/e8208ab8-9eb2-41cf-bf61-d0b1e594856c

</details>

[`demo/`](demo/) holds the renderer, the CC0 sources and
[`demo/make_demos.sh`](demo/make_demos.sh), the script that builds every clip.

## 🚀 Quick start

### Requirements

- A [Daisy Seed](https://daisy.audio/products/seed3), any revision, with its 64
  MB SDRAM. The 480 MHz clock needs an STM32H750 of silicon revision V or X,
  which the library detects.
- A built [libDaisy](https://github.com/electro-smith/libDaisy). Developed
  against revision `cc146d5065dd8286078a662e2830bf820c37a612`; the tests read
  its `CpuLoadMeter` and STM32 headers from whatever `LIBDAISY_DIR` names.
- The [Arm GNU
  toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
  and GNU make. Built and tested with GCC 12.2 and 16.2. The host tests need any
  C++14 compiler.
- An audio block size of 48 samples, or a divisor or multiple of it (see
  `Engine::Config`).

### Add the library

Add it next to libDaisy, as a git submodule or a plain checkout:

```sh
git submodule add https://github.com/benjaminvdb/cloudseed-daisy lib/cloudseed-daisy
git -C lib/cloudseed-daisy checkout v0.1.0   # a release, not the branch tip
```

A project's Makefile names its own sources and libDaisy, sets whatever [build
options](#-build-options) it wants, and includes `cloudseed.mk`, which adds the
library's sources, include path and flags before including libDaisy's core
Makefile:

```make
TARGET = myreverb
CPP_SOURCES = main.cpp
LIBDAISY_DIR = lib/libDaisy
OPT = -Os                      # the reverb's kernels are -O2 regardless
include lib/cloudseed-daisy/cloudseed.mk
```

### Write the firmware

The firmware hands the engine its programs and its audio configuration, calls it
from the audio callback, and services it from the main loop. That is the whole
contract. [`examples/seed/main.cpp`](examples/seed/main.cpp) is the complete
version of what follows:

```cpp
#include "cloudseed_daisy/engine.h"
#include "cloudseed_daisy/seed_system.h"
#include "daisy_seed.h"

daisy::DaisySeed seed;
CLOUDSEED_DAISY_DTCM cloudseed_daisy::Engine engine;
CLOUDSEED_DAISY_DTCM float wet_l[48], wet_r[48];

const cloudseed_daisy::Program kPrograms[] = {
    {&cloudseed::presets::kMediumSpace, 3},
    {&cloudseed::presets::kThroughTheLookingGlass, 12},
};

void AudioCallback(daisy::AudioHandle::InputBuffer in,
                   daisy::AudioHandle::OutputBuffer out, size_t size) {
  engine.BeginBlock();
  engine.RequestProgram(0);
  engine.SetParameter(cloudseed::Parameter::LineDecay, 0.6, 0.001f);
  const bool wet = engine.Process(in[0], in[1], wet_l, wet_r, size);
  for (size_t i = 0; i < size; i++) {
    out[0][i] = 0.5f * in[0][i] + (wet ? 0.5f * wet_l[i] : 0.f);
    out[1][i] = 0.5f * in[1][i] + (wet ? 0.5f * wet_r[i] : 0.f);
  }
  engine.EndBlock();
}

int main() {
  seed.Init(cloudseed_daisy::SupportsBoost());   // 480 MHz where possible
  cloudseed_daisy::ConfigureSdramRefresh();      // libDaisy refreshes too slowly
  cloudseed_daisy::SetSdramTiming(false);
  cloudseed_daisy::ConfigureSramNoWriteAllocate();
  seed.SetAudioBlockSize(48);

  cloudseed_daisy::Engine::Config config;
  config.programs = kPrograms;
  config.program_count = 2;
  config.sample_rate = seed.AudioSampleRate();
  config.block_size = seed.AudioBlockSize();
  config.on_program_loaded = [](cloudseed::ReverbController& reverb, void*) {
    reverb.SetParameter(cloudseed::Parameter::DryOut, 0.0);
  };  // the callback above mixes the dry signal itself
  if (!engine.Init(config)) for (;;) {}
  engine.Start(0);
  seed.StartAudio(AudioCallback);
  for (;;) {
    engine.Service();
    seed.SetLed(engine.overloaded());
    daisy::System::Delay(2);
  }
}
```

### Build and flash

```sh
make -C examples/seed LIBDAISY_DIR=/path/to/libDaisy
```

Flash `examples/seed/build/cloudseed_seed.bin` with the [Daisy Web
Programmer](https://electro-smith.github.io/Programmer/) or with `make
program-dfu`.

## 🧠 How it works

The delay memory of a twelve-line program is far larger than the Seed's 16 KB
data cache. On a bare port the memory system, not the arithmetic, sets the cost.
Three mechanisms get the plugin's largest program under the audio deadline, and
an application sees none of them:

1. **Per-program placement.** Every program's delay buffers are sized for that
   program and placed in the internal SRAM (448 KB of AXI SRAM and 232 KB of D2
   SRAM by default). What does not fit stays in the SDRAM.
2. **Staging in the tightly coupled memories.** For every delay ring the program
   processes, the STM32H750's MDMA copies the window the next block will read
   into the ITCM and DTCM, then copies the block's writes back, streamed through
   the audio callback. The kernels run from zero-wait-state memory without wrap
   checks or cache lines.
3. **Recovery.** Every block is measured. A block over the CPU budget stops the
   program; the main loop reloads it one delay line lighter and the wet signal
   fades back in. A transport fault falls back to the CPU paths. The engine
   remembers, per program and until reset, the line count at which a program
   last went over, so revisiting a heavy program does not retry a workload that
   already failed.

The plugin's arithmetic is kept expression for expression through all of this,
so the staged and direct paths agree bit for bit and both sit 82 to 147 dB below
the plugin's own kernel with modulation off. Parameter changes are smoothed,
freeze is supported, and a profiling build reports the load of every section of
the callback over USB serial.

## 🧩 The engine

`cloudseed_daisy::Engine` ([`engine.h`](src/cloudseed_daisy/engine.h)) owns the
reverb and its delay memory, the staging and its transport, the loading of
programs outside the audio interrupt, and the recovery from overload and
transport faults.

The callback and the main loop share the reverb through one lock-free state. The
callback owns it while it runs, main owns it while it loads or recovers, and the
wet signal fades out before a handover and back in afterwards.

A callback calls `BeginBlock()` first and `EndBlock()` last. In between it can
call `RequestProgram()`, `SetParameter()` and `SetFrozen()`, and it calls
`Process()` to render the wet signal. `Process()` returns false while main owns
the reverb, which is the application's cue to pass the dry signal through alone.
The main loop calls `Service()` to do the loading and the recovery, and
`overloaded()` is what an LED should show. `engine.h` documents every call,
which parameters `SetParameter()` accepts live, and what `Init()` rejects.

Two things are easy to get wrong:

- Use one `Engine` per application. Its DSP, pools and transport are shared
  static storage.
- Place the engine and the callback's buffers in the DTCM
  (`CLOUDSEED_DAISY_DTCM`). It is neither cached nor subject to wait states, and
  the delay-memory streams evict everything else from the data cache. libDaisy's
  start-up code does not clear that section, so the engine clears it before any
  object placed there is constructed.

### The Seed's system settings

[`seed_system.h`](src/cloudseed_daisy/seed_system.h) holds the settings the
performance above was measured with, as functions to call after
`DaisySeed::Init()`. None of them is specific to the reverb.

- `SupportsBoost()` reports whether the silicon takes the 480 MHz clock
  (revision V or X; ST's errata ES0392 limits older revisions to 400 MHz), for
  `DaisySeed::Init(boost)`.
- `ConfigureSdramRefresh()` fixes libDaisy's refresh rate for the Seed's
  AS4C16M32MSA, which its driver refreshes 2.6 times too slowly for the
  datasheet (8192 rows per 64 ms at the 100 MHz SDRAM clock). This is a
  retention risk for any firmware that touches the SDRAM, not a performance
  setting.
- `SetSdramTiming(datasheet)` picks the part's row and column timings: the
  datasheet's, in which case test the memory afterwards with
  `Engine::TestDelayMemory()`, or libDaisy's conservative ones with the two
  minima it cuts short corrected.
- `ConfigureSramNoWriteAllocate()` makes the internal SRAM write-back without
  write allocation, so sample streams written once and read long after leave
  through the store buffer instead of fetching their cache lines.
- `ConfigureSdramWriteAllocate()` does the opposite for the SDRAM, for firmwares
  whose SDRAM the CPU alone uses.

## 🔧 Build options

Set these in the project's Makefile before including `cloudseed.mk`, or on the
make command line. A changed option rebuilds every object.

| Option | Default | Effect |
|---|---:|---|
| `CLOUDSEED_MAX_LINES` | 12 | Cap on late lines per channel, 1 to 12. Sizes the reverb object and the delay memory. |
| `CLOUDSEED_SAMPLE_RATE` | 48000 | The sample rate the delay memory is sized for; `Engine::Init` fails above it. |
| `CLOUDSEED_PLACEMENT` | 1 | Per-program delay memory in the internal SRAM. |
| `CLOUDSEED_AXI_POOL_KB` | 448 | The AXI SRAM delay pool (of 512 KB; the default leaves 34 KB to the application). |
| `CLOUDSEED_D2_POOL_KB` | 232 | The D2 SRAM delay pool, behind libDaisy's audio DMA buffers and the MDMA list (of 288 KB; the default leaves 640 B). |
| `CLOUDSEED_STAGING` | 1 | 0 off, 1 the MDMA stages the delay memory in the TCMs, 2 the CPU copies it. |
| `CLOUDSEED_DTCM_STAGING_KB` | 24 | DTCM given to the staging; the rest above the engine's objects is the stack. |
| `CLOUDSEED_AHBS_INITCOUNT` | 1 | Cortex-M7 AHB slave fairness counter, for the TCM contention experiment. |
| `CLOUDSEED_PROFILE` | 0 | Cycle-counted profiling with reports over USB serial. |
| `CLOUDSEED_ENGINE_OPT` | `-O2` | Optimization of the engine object (the callback's side of the library). |
| `CLOUDSEED_STRIP_USB_HOST` | 0 | Keep libDaisy's USB host stack out of the image (11 KB of flash) when the firmware does not use it. |

Every object compiles with `-ffp-contract=off`. The port's arithmetic is unfused
to match the host-built reference bit for bit, and on the Cortex-M7 unfused
turns out to be faster anyway (TECHNICAL.md, "Numerical decisions").

## 💾 Memory and flash

Static figures from clean builds with ARM GCC 16.2.0 at the defaults, for the
bare Seed example and for the reference firmware, which adds its own controls,
mixing and, in the profiling build, logging.

| Resource | Example | Reference default | Reference profiling | Capacity | Fullest |
|---|---:|---:|---:|---:|---:|
| Flash | 109,296 B | 113,420 B | 127,768 B | 131,072 B | 97.5% |
| DTCM | 106,240 B | 106,240 B | 106,744 B | 131,072 B | 81.4% |
| AXI SRAM | 487,416 B | 489,280 B | 501,552 B | 524,288 B | 95.7% |
| D2 SRAM | 293,952 B | 294,272 B | 294,272 B | 294,912 B | 99.8% |
| SDRAM | 15,974,400 B | 15,974,400 B | 15,974,400 B | 67,108,864 B | 23.8% |

"Fullest" is the largest of the three builds, always the profiling one, against
the capacity beside it. The DTCM figure covers the engine's objects and the
staging memory; the stack takes the remaining 25 KB, of which the profiling
build measured 15 KB never used, so the 19% left over is spoken for rather than
spare. The ITCM is entirely staging memory, placed by address, so the linker
reports it as empty. The SDRAM holds the worst-case delay memory of every
program at `CLOUDSEED_MAX_LINES`.

CI measures the example separately with Arm GNU Toolchain 15.3.Rel1: 110,392 B
of flash by default, 112,320 B with `CLOUDSEED_PROFILE=1`, RAM as above except
for 106,744 B of DTCM when profiling.
[`test/size_budget.txt`](test/size_budget.txt) records those ceilings, with 4 KB
of flash headroom and 512 B per internal RAM region.

## 📊 Performance

Measured on reference image `59d22c54` at 480 MHz, 48 kHz and 48-sample blocks,
over the one-second reports in which the program ran unfrozen and its
configuration did not change: 200 of them, no overload, no staging failure and
no MDMA error. The line counts are the programs' own. These are observed peaks,
not a worst-case timing bound. TECHNICAL.md, "Measured performance", has the
breakdown per section and the number of reports behind each row.

| Program | Lines | Mean load | Peak block |
|---|---:|---:|---:|
| Small Room | 3 | 19.6% | 21.0% |
| Medium Space | 3 | 23.2% | 24.6% |
| Noise in the Hallway | 8 | 19.2% | 21.3% |
| Hyperplane | 9 | 55.0% | 58.2% |
| Rubi-Ka Fields | 4 | 27.2% | 28.5% |
| Through the Looking Glass | 12 | 77.8% | 80.1% |
| The 90s Are Back | 9 | 21.5% | 23.1% |
| Dull Echoes | 12 | 25.6% | 27.2% |
| Chorus Delay | 12 | 42.5% | 44.3% |
| Dark Plate | 12 | 46.1% | 48.1% |

A program draws a few points less while frozen, since the damping filters are
bypassed: Dark Plate holds 46.1% running and 40.7% frozen. Nine of the ten
peaks fall in an interval where a pot was moving and the delay lines were being
recomputed; Through the Looking Glass reaches its peak without one.
These figures replace a capture of image `623fe82a`, which predated the tenth
program and the corrected floating-point build flag and read half a point to
three points higher.

A profiling build (`CLOUDSEED_PROFILE=1`) counts the cycles of every section of
the callback with the Cortex-M7's cycle counter and hands the application one
report per second to print through any sink (`Engine::PrintBuild`,
`PrintProgramIfChanged`, `PrintReport`). Each report carries the image's CRC-32,
which `make` prints at the end of every build, so a log names its own firmware.
TECHNICAL.md explains every field.

## 📦 The kernel without the engine

`cloudseed::ReverbController`
([`reverb_controller.h`](src/cloudseed/reverb_controller.h)) is the reverb by
itself: parameters as the plugin's normalized 0..1 values, stereo processing in
blocks of up to 48 samples, freeze, and placement of its delay memory in
caller-provided pools. It takes that memory from a `MemoryPool` the caller
fills, has no dependency on libDaisy, and runs on a desktop host, which is how
its tests work.

`Staging` ([`staging.h`](src/cloudseed/staging.h)) is the staging manager. It is
a template on a transport type, so another STM32H7 board or another DMA can
supply its own; [`test/queued_transport.h`](test/queued_transport.h) is the
smallest one there is.

## 🧪 Tests

The host suites need a C++14 compiler, Python 3, GNU make and git.
[`test/all.sh`](test/all.sh) runs them all; pass a CloudSeed checkout to include
the fidelity suite. The [GitHub workflow](.github/workflows/test.yml) runs the
suites and builds the example on every push.

Between them the suites hold the DSP and the staging to bit-identical output
between the staged and direct paths, under ASan and UBSan, across block sizes,
wraps, partial and failed staging, freezes, reloads and every program. The
engine's overload detection and recovery are tested against libDaisy's own
`CpuLoadMeter` on a deterministic clock, the MDMA transport at register level
against a stub channel, and the port's `sin()` and `cos()` against fdlibm over
8,024,008 values, all identical. One suite builds the quick start above as a
real submodule consumer, so drift between this page and the library fails
loudly; another, given a CloudSeed checkout, compares the whole kernel against
the plugin's own.

[CONTRIBUTING.md](CONTRIBUTING.md) lists each suite, what it needs and what a
change to the arithmetic has to respect. [SECURITY.md](SECURITY.md) covers
reporting a vulnerability.

[`test/inventory.cpp`](test/inventory.cpp) is not a test but is useful anyway:
it prints every program's delay buffers, their placement and the staging plan.

```sh
g++ -O2 -ffp-contract=off -std=c++14 -I src test/inventory.cpp src/cloudseed/*.cpp
```

## 🏷️ Versioning

The library is at **0.x**: complete and tested, but not frozen. It is out early
so that people building on Daisy can put it in real firmware and say what is
wrong with it, and a 0.x minor release may rename or change a call to act on
that. [CHANGELOG.md](CHANGELOG.md) says when one does, and 1.0.0 follows once
that feedback has landed.

So pin a [release](https://github.com/benjaminvdb/cloudseed-daisy/releases)
rather than tracking `main`, and commit the submodule pointer. That is what
makes a firmware's build reproducible, and it is the right advice at 1.x too.

From 1.0.0, [semantic versioning](https://semver.org/spec/v2.0.0.html) applies
over the documented interface: [`engine.h`](src/cloudseed_daisy/engine.h),
[`reverb_controller.h`](src/cloudseed/reverb_controller.h),
[`presets.h`](src/cloudseed/presets.h),
[`seed_system.h`](src/cloudseed_daisy/seed_system.h) and the build options
`cloudseed.mk` accepts. Anything an application cannot reach, such as a private
member or the layout of an internal buffer, is outside it, and so are the CPU
and memory figures, which move with the compiler. It is a source promise rather
than an ABI one; the library is compiled into your image.

The version lives in [`version.h`](src/cloudseed_daisy/version.h) as macros, so
firmware that supports more than one release can ask:

```cpp
#include "cloudseed_daisy/version.h"

#if CLOUDSEED_DAISY_VERSION_AT_LEAST(1, 1, 0)
  // something added in 1.1
#endif
```

`engine.h` includes it, and the release workflow refuses a tag that disagrees
with it.

Found something, or want something? [Open an
issue](https://github.com/benjaminvdb/cloudseed-daisy/issues/new/choose). While
the library is at 0.x that feedback is what 1.0.0 is waiting on, and
[CONTRIBUTING.md](CONTRIBUTING.md) covers sending a change.

## 📁 Repository layout

- [`src/cloudseed/`](src/cloudseed/) is the reverb kernel: a platform-neutral
  port of Cloud Seed's DSP (`ReverbController`), the staging manager (`Staging`)
  and the programs (`presets.h`). Compiles on any host.
- [`src/cloudseed_daisy/`](src/cloudseed_daisy/) is the Daisy Seed layer: the
  engine, the MDMA transport, the Seed's system settings.
- [`cloudseed.mk`](cloudseed.mk) is the make integration for libDaisy projects.
- [`examples/seed/`](examples/seed/) is Cloud Seed on a bare Daisy Seed, the
  smallest complete firmware.
- [`demo/`](demo/) is the clips above, the renderer and their CC0 sources.
- [`test/`](test/) is the host test suites.

## 🎹 Reference application

This library took shape inside the [Löwenzahnhonig
firmware](https://github.com/wgd-modular/loewenzahnhonig-firmware) and now lives
on its own. That firmware's Cloud Seed build is the library's reference
application: it maps a program selector, a mix, a decay and a tone pot and two
CV inputs onto the engine, uses this library as a submodule, and is the firmware
every measurement here was taken from, on its module. Its README describes the
module.

## 📚 Further reading

- [Daisy documentation](https://docs.daisy.audio/), Electrosmith's own
  [hardware](https://docs.daisy.audio/hardware/) and
  [software](https://docs.daisy.audio/software/) pages, and the [Daisy
  forum](https://forum.electro-smith.com/), where Daisy questions get answered.
- [libDaisy](https://github.com/electro-smith/libDaisy) and its [API
  reference](https://electro-smith.github.io/libDaisy/);
  [DaisyExamples](https://github.com/electro-smith/DaisyExamples) for the shape
  of a Daisy project.
- [Cloud Seed](https://github.com/ValdemarOrn/CloudSeed), the original plugin,
  and the [Cloud Seed 2 core](https://github.com/GhostNoteAudio/CloudSeedCore)
  that Dark Plate comes from.
- TECHNICAL.md's [Sources](TECHNICAL.md#sources) has the rest: ST's datasheet,
  reference manual and errata for the STM32H750, the SDRAM's datasheet, and the
  DSP and worst-case-timing references behind the design.

## 📄 License and credits

MIT, see [LICENSE](LICENSE). The work by others this library contains is listed
in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), and the full texts are in
[`src/cloudseed/license.txt`](src/cloudseed/license.txt).

| Component | Attribution |
|---|---|
| The reverb kernel | A port of [Cloud Seed](https://github.com/ValdemarOrn/CloudSeed), © 2018 Valdemar Erlingsson, MIT License. |
| The Dark Plate program | Adapted from [Cloud Seed 2's core](https://github.com/GhostNoteAudio/CloudSeedCore), © 2024 Ghost Note Engineering Ltd, MIT License. |
| SHA-256 | Olivier Gay, Modified BSD License. |
| Trigonometry | fdlibm-derived; retains Sun Microsystems' notice. |

## 🤖 On the use of AI

I built this library with an AI coding assistant, heavily: the port, the engine,
the tests and these documents all went through one. I've tried to port this
reverb many times, but didn't succeed. With the latest AI models, I was finally
able to pull it off. I didn't create this project to show off my DSP wizardry.
Rather, I think this reverb is amazing and I'm just happy for others to have
access to it without the hassle I had to go through!

I won't pretend I can explain every line of it. What I can say is that it is
checked by things that don't rely on my judgement: the output is compared
against Valdemar Erlingsson's own kernel, the staged and direct paths are
compared against each other, the flash and RAM figures come out of a linker map
in CI, and TECHNICAL.md labels every quantitative claim by how it was
established. And I flash a build to a Seed and listen to it before I call
something a release; [docs/release-acceptance.md](docs/release-acceptance.md) is
that checklist.

If you find something wrong here, an issue is welcome, whoever or whatever wrote
it.
