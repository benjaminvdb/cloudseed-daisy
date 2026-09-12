# ☁️ cloudseed-daisy 🌼

**Cloud Seed — Valdemar Erlingsson's algorithmic reverb — as a library for the
Electro-Smith Daisy Seed.**

[![Tests](https://github.com/benjaminvdb/cloudseed-daisy/actions/workflows/test.yml/badge.svg)](https://github.com/benjaminvdb/cloudseed-daisy/actions/workflows/test.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform: Daisy Seed](https://img.shields.io/badge/platform-Daisy%20Seed%20%C2%B7%20STM32H750-8a2be2.svg)](https://electro-smith.com/products/daisy-seed)
[![Standard: C++14](https://img.shields.io/badge/C%2B%2B-14-00599c.svg)](#requirements)

[Cloud Seed](https://github.com/ValdemarOrn/CloudSeed) is an open-source reverb
built "for emulating huge, endless spaces and modulated echoes" and released as
a VST plugin under the MIT license. This library ports its C++ kernel to single
precision and wraps it in an engine for the Daisy Seed (STM32H750) that runs the
plugin's programs **at their full size**: all twelve delay lines of the largest
program, with the CPU load measured and bounded.

It ships the plugin's **nine factory programs** and the built-in program of its
successor, Ghost Note Audio's Cloud Seed 2, from its MIT-licensed
[core](https://github.com/GhostNoteAudio/CloudSeedCore).

📘 **[TECHNICAL.md](TECHNICAL.md)** is the engineering reference: the
architecture, the numerical decisions, the optimization history with its
measurements, the verification, every defect found and corrected, and the
lessons worth keeping.

## ✨ Highlights

- **Ten programs.** Cloud Seed's nine factory presets plus Cloud Seed 2's
  Dark Plate.
- **Full size, not a reduction.** The largest program runs all twelve of its
  late delay lines inside the audio deadline.
- **Bit-identical paths.** Staged and direct processing agree bit for bit, and
  sit 82 to 147 dB below the plugin's own kernel with modulation off.
- **Self-protecting.** Every block is measured; a block over budget stops the
  program, reloads it one delay line lighter, and fades the wet signal back in.
- **Three-line build integration.** Point a libDaisy Makefile at
  `cloudseed.mk`; DaisySP is not needed.
- **Testable on a desktop.** The DSP kernel is platform-neutral and compiles on
  any host; six suites run in CI.

## 🎧 Hear it

Every clip plays the same dry phrase — a strummed guitar chord, a snare, a piano
note left ringing — and then that phrase through one program. One gain is
applied to all ten, so a program that is louder than another sounds louder here
too, and the programs run at their own dry/wet balance.

**Medium Space.** Three delay lines, a two-second tail — the everyday room.

https://github.com/user-attachments/assets/9cf6fa96-1c63-4339-9804-9893a17e9734

**Through the Looking Glass.** Twelve delay lines and twenty-six seconds of
tail: the largest program, and the one the CPU budget was built for.

https://github.com/user-attachments/assets/b8815225-e07d-4579-9295-8e0e1b878c90

**Chorus Delay.** Twelve lines used as modulated echoes rather than as a room.

https://github.com/user-attachments/assets/058be392-9ca3-4e4f-8664-725a7e5d4507

<details>
<summary><b>The other seven programs</b></summary>

**Small Room.** Three lines, a one-second tail.

https://github.com/user-attachments/assets/12d6ba9e-c94d-4374-8ac6-474f61f2979b

**Noise in the Hallway.** Eight lines, four seconds, close and narrow.

https://github.com/user-attachments/assets/18685d83-567d-4f5c-9182-3d9e9445b6f1

**Hyperplane.** Nine lines and twenty-two seconds, the widest of the nine
factory programs.

https://github.com/user-attachments/assets/781e48a0-3076-4c11-8cc1-859831ceda39

**Rubi-Ka Fields.** Four lines, fourteen seconds.

https://github.com/user-attachments/assets/01ae1c47-2390-4872-a9eb-b4f5a7e8134e

**The 90s Are Back.** Nine lines, four seconds.

https://github.com/user-attachments/assets/21e2f431-b878-40fd-b97c-7b5d29611bb7

**Dull Echoes.** Twelve lines, a short tail of discrete echoes.

https://github.com/user-attachments/assets/7cc55168-2b4a-4b64-9559-0f159aba8b85

**Dark Plate.** Twelve lines, fourteen seconds — the program from Cloud Seed 2.

https://github.com/user-attachments/assets/e8208ab8-9eb2-41cf-bf61-d0b1e594856c

</details>

The tail is the time from the last note until the reverb is 60 dB below where
it started; the width is how far the two channels have come apart (0 is mono,
1 is uncorrelated). Both are measured from these renders.

| Program | Lines | Tail | Width |
|---|---:|---:|---:|
| Small Room | 3 | 1 s | 0.31 |
| Medium Space | 3 | 2 s | 0.69 |
| Noise in the Hallway | 8 | 4 s | 0.72 |
| Hyperplane | 9 | 22 s | 0.98 |
| Rubi-Ka Fields | 4 | 14 s | 0.93 |
| Through the Looking Glass | 12 | 26 s | 0.92 |
| The 90s Are Back | 9 | 4 s | 0.89 |
| Dull Echoes | 12 | 1 s | 0.94 |
| Chorus Delay | 12 | 10 s | 0.92 |
| Dark Plate | 12 | 14 s | 0.99 |

[`demo/`](demo/) holds the renderer, the CC0 sources and the script that builds
every clip: `demo/make_demos.sh`.

## 📖 Table of contents

- [Hear it](#-hear-it)
- [Quick start](#-quick-start)
  - [Requirements](#requirements)
  - [Add the library](#add-the-library)
  - [Write the firmware](#write-the-firmware)
  - [Build and flash](#build-and-flash)
- [How it works](#-how-it-works)
- [Repository layout](#-repository-layout)
- [The engine](#-the-engine)
  - [The Seed's system settings](#the-seeds-system-settings)
- [Build options](#-build-options)
- [Memory and flash](#-memory-and-flash)
- [Performance](#-performance)
  - [Measuring the load](#measuring-the-load)
- [The kernel without the engine](#-the-kernel-without-the-engine)
- [Tests](#-tests)
- [Reference application](#-reference-application)
- [License and credits](#-license-and-credits)

## 🚀 Quick start

### Requirements

| Requirement | Detail |
|---|---|
| 🎛️ **Hardware** | A Daisy Seed (any revision) with its 64 MB SDRAM. The 480 MHz clock needs an STM32H750 of silicon revision V or X, which the library detects. |
| 📚 **libDaisy** | [libDaisy](https://github.com/electro-smith/libDaisy), built. Developed against revision `cc146d5065dd8286078a662e2830bf820c37a612`; the tests read its `CpuLoadMeter` and STM32 headers from whatever checkout `LIBDAISY_DIR` names. |
| 🔨 **Toolchain** | The Arm GNU toolchain (built and tested with GCC 12.2 and 16.2) and GNU make. A C++14 compiler for the host tests. **DaisySP is not needed.** |
| 🎚️ **Audio** | A block size of 48 samples, or a divisor or multiple of it (see `Engine::Config`). |

### Add the library

Add it next to libDaisy, as a git submodule or a plain checkout:

```sh
git submodule add https://github.com/benjaminvdb/cloudseed-daisy lib/cloudseed-daisy
```

A project's Makefile names its own sources and libDaisy, sets the options it
wants, and includes `cloudseed.mk` — which adds the library's sources, include
path and flags, and then includes libDaisy's core Makefile:

```make
TARGET = myreverb
CPP_SOURCES = main.cpp
LIBDAISY_DIR = lib/libDaisy
OPT = -Os                      # the reverb's kernels are -O2 regardless
include lib/cloudseed-daisy/cloudseed.mk
```

### Write the firmware

The firmware hands the engine its programs and its audio configuration, calls it
from the audio callback, and services it from the main loop. This is the whole
contract; `examples/seed/main.cpp` is the complete version:

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

Flash `examples/seed/build/cloudseed_seed.bin` with the
[Daisy Web Programmer](https://electro-smith.github.io/Programmer/) or with
`make program-dfu`.

## 🧠 How it works

The delay memory of a twelve-line program is far larger than the Seed's 16 KB
data cache, and on a bare port the memory system — not the arithmetic — sets the
cost. The library gets the plugin's largest program under the audio deadline
with three mechanisms, **all of them transparent to the application**:

1. **Per-program placement.** Every program's delay buffers are sized for that
   program and placed in the internal SRAM (448 KB of AXI SRAM and 232 KB of
   D2 SRAM by default); what does not fit stays in the SDRAM.
2. **Staging in the tightly coupled memories.** For every delay ring the program
   processes, the STM32H750's MDMA copies the window the next block will read
   into the ITCM and DTCM and copies the block's writes back, streamed through
   the audio callback — so the kernels run from zero-wait-state memory without
   wrap checks or cache lines.
3. **Recovery.** The engine measures every block. A block over the CPU budget
   stops the program, the main loop reloads it with one delay line fewer, and
   the wet signal fades back in; a transport fault falls back to the CPU paths.
   The application only ever sees a `bool` telling it whether the wet signal is
   valid.

Around that:

- **Fidelity.** The plugin's arithmetic is kept expression for expression, so
  output is bit-identical between the staged and the direct paths, and within
  82 to 147 dB of the plugin's own kernel with modulation off.
- **Playability.** A freeze, and smooth parameter changes.
- **Observability.** A profiling build reports the load of every section of the
  callback over USB serial, with the image's CRC-32 so that a log names its
  build.

## 📁 Repository layout

| Path | Contents |
|---|---|
| `src/cloudseed/` | The reverb kernel: a platform-neutral port of Cloud Seed's DSP (`ReverbController`), the staging manager (`Staging`), and the programs (`presets.h`). Compiles on any host. |
| `src/cloudseed_daisy/` | The Daisy Seed layer: the engine (`engine.h`), the MDMA transport, the Seed's system settings (`seed_system.h`). |
| `cloudseed.mk` | The make integration for libDaisy projects. |
| `examples/seed/` | Cloud Seed on a bare Daisy Seed: the smallest complete firmware. |
| `demo/` | The demo clips in the README, the renderer that makes them and their CC0 sources. |
| `test/` | The host test suites (see [Tests](#-tests)). |

## 🧩 The engine

`cloudseed_daisy::Engine` (`src/cloudseed_daisy/engine.h`) owns:

- the reverb and its delay memory in every memory of the STM32H750,
- the staging and its transport,
- the loading of programs outside the audio interrupt,
- the recovery from overload and transport faults.

The callback and the main loop share the reverb through **one lock-free state**:
the callback owns it while it runs, main owns it while it loads or recovers, and
the callback fades the wet signal out before handing over and back in
afterwards. The engine's header documents every call; this is the shape.

| Call | Context | What it does |
|---|---|---|
| `Init(config)` | main, once | The delay memory, the reverb, the staging, the load meter. False when the delay memory does not fit the sample rate. |
| `Start(program)` | main, before the audio | Loads the first program. |
| `BeginBlock()` | callback, first | Starts the block's load measurement and profiling. |
| `RequestProgram(i)` | callback, every block | Selects the program; a change fades out, main loads, fades in. |
| `SetParameter(p, value, threshold)` | callback | A normalized 0..1 parameter, applied when the callback owns the reverb and the value moved by more than the threshold (a pot's noise must not recompute the delay lines). |
| `SetFrozen(bool)` | callback | Freezes the late reverb: unity feedback, damping bypassed, no new input. |
| `Process(in_l, in_r, wet_l, wet_r, size)` | callback | Renders the wet signal with the program fade applied; false while a program is loaded or recovered, when the application passes the dry signal alone. |
| `EndBlock()` | callback, last | Ends the measurement; decides an overload. |
| `Service()` | main loop | Loads requested programs, reduces overloaded ones, recovers from faults. |
| `state()`, `program()`, `line_count()`, `line_limit(i)`, `frozen()`, `overloaded()` | any | Status; `overloaded()` is what an LED should show. |
| `reverb()` | main, inside the load hook | The `ReverbController`, for parameters the application fixes after every load (`Config::on_program_loaded`). |

A `Program` is a preset and the late delay lines per channel it runs with. The
engine remembers, per program and until reset, the line count at which the
program last exceeded the budget, so revisiting a heavy program does not retry a
workload that failed.

**Place the engine and the callback's buffers in the DTCM**
(`CLOUDSEED_DAISY_DTCM`): it is neither cached nor subject to wait states, and
the delay-memory streams evict everything else from the data cache. libDaisy's
start-up code does not clear that section; the engine does, before any object
placed there is constructed.

### The Seed's system settings

`src/cloudseed_daisy/seed_system.h` holds the settings the engine's performance
was measured with, as functions an application calls after `DaisySeed::Init()`.
**None of them is specific to the reverb.**

- **`SupportsBoost()`** — whether the silicon takes the 480 MHz clock (revision
  V or X of the STM32H750; ST's errata ES0392 limits older revisions to
  400 MHz), for `DaisySeed::Init(boost)`.
- **`ConfigureSdramRefresh()`** — libDaisy's driver refreshes the Seed's
  AS4C16M32MSA SDRAM 2.6 times too slowly for its datasheet (8192 rows per
  64 ms at the 100 MHz SDRAM clock). This is a *retention risk for any firmware
  that uses the SDRAM*, not a performance setting.
- **`SetSdramTiming(datasheet)`** — the part's row and column timings, either
  the datasheet's (test the memory afterwards, `Engine::TestDelayMemory()`) or
  libDaisy's conservative ones with the two minima it cuts short corrected.
- **`ConfigureSramNoWriteAllocate()`** — the internal SRAM as write-back without
  write allocation, so that sample streams written once and read long after
  leave through the store buffer instead of fetching their cache lines.
- **`ConfigureSdramWriteAllocate()`** — the opposite policy for the SDRAM, for
  firmwares whose SDRAM the CPU alone uses.

## 🔧 Build options

Set them in the project's Makefile **before** including `cloudseed.mk`, or on
the make command line. A changed option rebuilds every object (the options are
recorded in a build prerequisite).

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

`cloudseed.mk` compiles every object with `-ffp-contract=off`: the port's
arithmetic is unfused to match the host-built reference bit for bit, and on the
Cortex-M7 unfused is faster too (TECHNICAL.md, "Numerical decisions").

## 💾 Memory and flash

Static, from clean builds with ARM GCC 16.2.0 at the defaults, for the bare Seed
example and the reference firmware (which adds its own controls, mixing and, in
the profiling build, logging).

| Resource | Example | Reference default | Reference profiling | Capacity |
|---|---:|---:|---:|---:|
| Flash | 109,296 B | 113,420 B | 127,768 B | 131,072 B |
| DTCM | 106,240 B | 106,240 B | 106,744 B | 131,072 B |
| AXI SRAM | 487,416 B | 489,280 B | 501,552 B | 524,288 B |
| D2 SRAM | 293,952 B | 294,272 B | 294,272 B | 294,912 B |
| SDRAM | 15,974,400 B | 15,974,400 B | 15,974,400 B | 67,108,864 B |

- The **DTCM** figure is the engine's objects and the staging memory; the stack
  takes the rest of the DTCM (25 KB, of which the profiling build measured
  16 KB never used).
- The **ITCM** is entirely staging memory, placed by address, so the linker
  reports it as empty.
- The **SDRAM** holds the worst-case delay memory of every program at
  `CLOUDSEED_MAX_LINES`.

## 📊 Performance

Measured on the reference module with the profiling build at **480 MHz, 48 kHz,
48-sample blocks**, over 125 one-second reports with no overload. The line
counts are the programs' own. TECHNICAL.md, "Measured performance", has the
breakdown per section.

| Program | Lines | Mean load | Peak block |
|---|---:|---:|---:|
| Small Room | 3 | 20.1% | 21.6% |
| Medium Space | 3 | 23.7% | 25.0% |
| Noise in the Hallway | 8 | 20.1% | 21.2% |
| Hyperplane | 9 | 56.2% | 59.3% |
| Rubi-Ka Fields | 4 | 27.7% | 29.1% |
| Through the Looking Glass | 12 | 80.9% | 84.1% |
| The 90s Are Back | 9 | 22.7% | 24.3% |
| Dull Echoes | 12 | 27.3% | 28.8% |
| Chorus Delay | 12 | 44.0% | 46.0% |
| Dark Plate | 12 | *not yet measured* | |

### Measuring the load

A profiling build (`CLOUDSEED_PROFILE=1`) counts the cycles of every section of
the callback with the Cortex-M7's cycle counter and hands the application one
report per second, which it prints through any sink (`Engine::PrintBuild`,
`PrintProgramIfChanged`, `PrintReport`; the reference firmware prints over
libDaisy's USB serial logger).

<details>
<summary><b>A report, and how to read it</b></summary>

```
program 5 "Through the Looking Glass" lines=12 limit=12 early=8 late=8 taps=50 predelay=0 image=1588b43c
  mod=eld interp=1 filters=-l--c latetap=1 streams=222 sdram=48 placed=447+232KB unplaced=36 clock=480MHz
  staging=mdma staged=234 unstaged=2 taps=98 copies<=894 tcm=76/87KB itcm=ok
load program=5 lines=12 mixed=0 avg=80.9% max=82.6% | input=0.6 predelay=0.7 taps=3.6 early=3.0 linemix=2.2 linedelay=4.8 linediff=36.5 linefilt=3.5 out=1.8 ctrl=0.1 param=0.0 clip=1.3 stage=13.3 wait=8.6 other=0.3 | freeze=0 updates=0 overloads=0 blocks=1000/1000
controls=607 722 1 22 1 999 1 999 1 722
staging copies=564 failures=0 stack_free=16332 segments=406888 mdma_errors=0 mdma_status=0x0 mdma_maxpolls=3208 list=712/726 us timed=1000 late=2714 stale=1000
```

| Line | What it carries |
|---|---|
| `program` | The loaded program's workload — stages, taps, filters, delay-memory streams, where its memory was placed, what the staging serves — and the image's CRC-32, which `make` prints at the end of every build. |
| `load` | The callback's load (`avg` and the worst block, as the overload guard sees it) and the share of the block time of every section, as a percentage of a second's block time. |
| `controls` | The values the application handed in with `SetProfileControls()`. |
| `staging` | The transport's copies per block, its faults, the stack never used, and the MDMA's segments, errors and list timings. |

TECHNICAL.md explains every field.

</details>

## 📦 The kernel without the engine

`cloudseed::ReverbController` (`src/cloudseed/reverb_controller.h`) is the reverb
itself:

- parameters as the plugin's normalized 0..1 values (`Parameter`, `presets.h`),
- stereo processing in blocks of up to 48 samples,
- freeze,
- placement of its delay memory in caller-provided pools.

It takes its delay memory from a `MemoryPool` the caller fills, has **no
dependency on libDaisy**, and compiles and runs on a desktop host — which is how
its tests work.

`Staging` (`src/cloudseed/staging.h`) is the staging manager, a template on a
transport type, so that another STM32H7 board or another DMA can supply its own;
`test/queued_transport.h` is the smallest one.

## 🧪 Tests

Every suite runs on the host with a C++14 compiler; **`test/all.sh` runs them
all**, and the GitHub workflow runs the suites and builds the example.

| Suite | What it establishes |
|---|---|
| `test/regression.sh` | The DSP and the staging under ASan and UBSan: bit-identical output between staged and direct paths across block sizes, wraps, partial and failed staging, freezes and reloads; the filters; every program; both kernel families at 4 and 12 lines. |
| `test/engine.sh` | The engine's callback and main-loop sides with the real DSP and libDaisy's `CpuLoadMeter` on a deterministic clock: overload detection and recovery, per-program limits, the selector handoff without main, transport failures and unfinished aborts, parameter thresholds, the profiling mailbox. Needs `LIBDAISY_DIR`. |
| `test/mdma.sh` | The MDMA transport at register level against a stub channel: descriptors and links, timing paths, bounded aborts, the unexpectedly enabled channel. Needs `LIBDAISY_DIR`. |
| `test/trig.sh` | The port's `sin()`/`cos()` against fdlibm: 8,024,008 values identical. |
| `test/run.sh path/to/CloudSeed` | Fidelity against the plugin's own kernel (a corrected copy of the checkout): 82 to 147 dB below the signal with modulation off. Needs a [CloudSeed](https://github.com/ValdemarOrn/CloudSeed) checkout and Python 3. |
| `test/benchmark.sh [--stress]` | Desktop timings of the direct paths; `--stress` renders two minutes per program frozen and checks every output stays finite. |

`test/inventory.cpp` prints every program's delay buffers, their placement and
the staging plan:

```sh
g++ -O2 -std=c++14 -I src test/inventory.cpp src/cloudseed/*.cpp
```

## 🎹 Reference application

This library took shape inside the [Löwenzahnhonig
firmware](https://github.com/wgd-modular/loewenzahnhonig-firmware) and now lives
on its own. That firmware's Cloud Seed build (`src/cloudseed` there) is the
library's **reference application**: it maps a program selector, a mix, a decay
and a tone pot and two CV inputs onto the engine, uses this library as a
submodule, and is the firmware every measurement here was taken from — on its
module. Its README describes the module.

## 📄 License and credits

MIT, see [LICENSE](LICENSE). The full texts are in
`src/cloudseed/license.txt`.

| Component | Attribution |
|---|---|
| The reverb kernel | A port of [Cloud Seed](https://github.com/ValdemarOrn/CloudSeed), © 2018 Valdemar Erlingsson, MIT License. |
| The Dark Plate program | Adapted from [Cloud Seed 2's core](https://github.com/GhostNoteAudio/CloudSeedCore), © 2024 Ghost Note Engineering Ltd, MIT License. |
| SHA-256 | Olivier Gay, Modified BSD License. |
| Trigonometry | fdlibm-derived; retains Sun Microsystems' notice. |
