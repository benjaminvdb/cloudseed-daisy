# Contributing

## Before a change

The port is kept **expression for expression** against Cloud Seed's own
kernel: the same operations, in the same order, at single precision. That is
what lets [`test/run.sh`](test/run.sh) hold the output to within 82 to 147 dB
of the plugin with modulation off, and what makes a change that merely looks
equivalent — reassociating a sum, fusing a multiply and an add, hoisting a
division — a change to the sound. `-ffp-contract=off` is part of the contract,
not a preference. [TECHNICAL.md](TECHNICAL.md), "Numerical decisions", has the
reasoning.

The Seed is close to full: the D2 SRAM pool leaves an application about
960 bytes, and the largest program uses most of the CPU budget. Flash and RAM
are reviewed on every change ([`test/size.sh`](test/size.sh)).

## Running the tests

The host suites need a C++14 compiler, Python 3, GNU make and git. The build,
engine and MDMA checks also need the libDaisy checkout named below. The
firmware suites additionally need the Arm toolchain and a built libDaisy.

```sh
bash test/all.sh                      # every host suite
bash test/all.sh /path/to/CloudSeed   # and the fidelity suite
```

Set the dependency path before running the suites. Build it for the firmware
checks (the host checks only read its Makefile and headers):

```sh
export LIBDAISY_DIR=/path/to/libDaisy
make -C "$LIBDAISY_DIR" -j
```

| Suite | What it wants |
|---|---|
| [`test/all.sh`](test/all.sh) | The host suites, in order. |
| [`test/release.py`](test/release.py) | Checks release metadata and exercises failure paths in the consumer, size and suite-runner scripts without an Arm compiler. |
| [`test/consumer.sh`](test/consumer.sh) `[ref]` | Builds the selected commit's README and library as a real submodule consumer (default `HEAD`). Commit the intended changes before running it; unstaged and untracked files are excluded. Run it after changing `cloudseed.mk`, the layout of `src/`, or the quick-start code blocks. |
| [`test/size.sh`](test/size.sh) | The example's flash and RAM against [`test/size_budget.txt`](test/size_budget.txt). Deliberate growth: `test/size.sh --update`, in the same commit, with the README's "Memory and flash" table updated too. |

The host suites compile with `-Wall -Wextra -Werror`, most of them under ASan
and UBSan. Keep it that way.

## Sending a change

Fork, branch, and open a pull request against `main`. The pull-request
template asks which suites you ran; please fill it in, and say whether the
change can affect what the hardware does — CI builds the firmware but cannot
run it, so anything touching the engine, the staging or the transport wants a
note about what you tried on a Seed.

Label pull requests: `dsp`, `engine`, `bug`, `performance`, `documentation`,
`tests`, `ci`, `breaking`. The release notes are grouped by those labels
([`.github/release.yml`](.github/release.yml)).

Match the surrounding code. Two spaces for C++, four for Python, and tabs for
Makefile recipes; 80 columns where the existing file keeps to them. Comments
explain why rather than what. An
[`.editorconfig`](.editorconfig) covers the mechanical part.

## Versioning

See [README.md](README.md#-versioning). In short: `src/cloudseed_daisy/version.h`
is the version, and the release workflow refuses a tag that disagrees with it.
The interface becomes stable within a major version at 1.0.0; the library is
at 0.x until the first users have had a go at it, so a change that improves an
awkward name or signature is welcome now rather than after the freeze — say so
in the pull request, and it goes in the changelog.

## Reporting a vulnerability

Not here — see [SECURITY.md](SECURITY.md).
