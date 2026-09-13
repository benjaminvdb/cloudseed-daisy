# Release acceptance

CI builds this library's firmware but cannot run it: no hosted runner has a
Daisy Seed, and what matters most here — SDRAM timing and refresh, the MDMA's
contention with the CPU for the TCMs, the cache policies, the audio deadline —
is exactly what a build cannot show and an emulator would only pretend to.
Renode can map the SDRAM as a region; it cannot tell you that the part retains
its rows or that a block met its deadline.

So the hardware evidence for a release is a checklist, run once on a board and
recorded with the release. Copy the template below into the release notes, or
into `docs/acceptance/vX.Y.Z.md`, with the figures filled in. An unfilled
checklist is the honest outcome too — it says the release was not measured.

A profiling application can report most of these figures. Building the bare
Seed example with this flag enables the counters:

```sh
make -C examples/seed LIBDAISY_DIR=/path/to/libDaisy CLOUDSEED_PROFILE=1
```

The example does not start a logger or print reports. Use the reference
application linked in the README, or instrument your application as follows:

- Call `Engine::FillStack()` first in `main()` for a meaningful `stack_free`.
- After `seed.Init()`, start a sink, for example `seed.StartLog()`, and define
  `void Print(const char* text) { seed.Print("%s", text); }`.
- After configuring SDRAM and before `engine.Init()`, call
  `Engine::TestDelayMemory()` and record its result. It overwrites the delay
  pool; calling it after initialization corrupts the reverb's memory.
- After starting the engine, call `engine.PrintBuild(Print)` once. In the
  main loop, call `engine.PrintProgramIfChanged(Print)` and take reports with
  `cloudseed_daisy::ProfileReport report; if (engine.TakeReport(&report))
  engine.PrintReport(report, Print);`. Keep these calls outside the callback
  and inside `#if CLOUDSEED_PROFILE`.

The `image=` field is the profiling image's CRC-32, which `make` also prints.
Record that image and its source changes: adding instrumentation changes the
binary, so its measurements do not identify the uninstrumented release asset.
`TestDelayMemory()` checks immediate pattern readback; it does not by itself
establish SDRAM retention over a refresh interval.

Before publishing, also check that private vulnerability reporting is enabled
in the repository settings: `SECURITY.md` links to that reporting form. The
workflow creates a draft release; it does not enable repository settings or
perform hardware acceptance.

## Template

```markdown
# Release acceptance — vX.Y.Z

| | |
|---|---|
| Commit | `<sha>` |
| Image CRC-32 | `<image=...>` |
| Library version | `CLOUDSEED_DAISY_VERSION_STRING` |
| libDaisy commit | `<sha>` |
| Toolchain | `arm-none-eabi-gcc --version` |
| Board | Daisy Seed rev `<n>`, silicon revision `<V or X>` |
| Clock | `<480 or 400>` MHz (`SupportsBoost()`) |
| Sample rate / block | 48 kHz / 48 |
| Date, operator | |

## Boot and memory

- [ ] Boots, audio starts, no hang in `Engine::Init`
- [ ] `Engine::TestDelayMemory()` passes at the timings used, before `Init()`
- [ ] Cold boot and power cycle, three times, no failure
- [ ] Stack never used: `stack_free=` ______ B

## Per program

For each of the ten programs, at its own line count:

| Program | Lines | Mean load | Peak block | Overloads | Notes |
|---|---:|---:|---:|---:|---|
| Small Room | 3 | | | | |
| Medium Space | 3 | | | | |
| Noise in the Hallway | 8 | | | | |
| Hyperplane | 9 | | | | |
| Rubi-Ka Fields | 4 | | | | |
| Through the Looking Glass | 12 | | | | |
| The 90s Are Back | 9 | | | | |
| Dull Echoes | 12 | | | | |
| Chorus Delay | 12 | | | | |
| Dark Plate | 12 | | | | |

- [ ] No program overloads at its documented line count
- [ ] The README's "Performance" table matches these figures, or has been updated

## Behaviour

- [ ] Program changes: fade out, load, fade in, no click and no discontinuity
- [ ] Every live `SetParameter` control moves the sound without a zipper or a click
- [ ] Freeze holds the tail, and releases it cleanly
- [ ] Normal operation: no staging failures or MDMA errors
- [ ] Injected staging fault: the wet signal returns on the CPU paths,
      `staging_enabled()` is false, and `staging_failures()` increases.
      Record the injected fault and MDMA error count; a transfer-error
      injection increments the cumulative `mdma_errors` counter.
- [ ] Deliberate overload (heaviest program, worst parameters) reduces the line
      count and recovers rather than dropping audio
- [ ] Thirty minutes on the heaviest program: no overload, no fault, no drift
- [ ] `staging ... failures=` ______ , `mdma_errors=` ______

## Signature

Ran by ______ on ______ . Deviations:
```
