#!/bin/bash
# Every host suite. The fidelity suite needs a CloudSeed checkout as its
# argument; the engine and MDMA suites need LIBDAISY_DIR (see their files).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export LIBDAISY_DIR=${LIBDAISY_DIR:-$HERE/../../libDaisy}
python3 "$HERE/release.py"
bash "$HERE/build.sh"
python3 "$HERE/render.py"
bash "$HERE/regression.sh"
bash "$HERE/engine.sh"
bash "$HERE/mdma.sh"
bash "$HERE/trig.sh"
if [ $# -ge 1 ]; then bash "$HERE/run.sh" "$1"; fi
bash "$HERE/benchmark.sh" --stress

# The firmware suites, when the Arm toolchain is installed and LIBDAISY_DIR
# names a *built* libDaisy. They are skipped rather than failed on a machine
# set up for the host suites alone; CI always runs them.
if command -v arm-none-eabi-gcc >/dev/null 2>&1 &&
   [ -f "$LIBDAISY_DIR/build/libdaisy.a" ]; then
  bash "$HERE/consumer.sh"
  bash "$HERE/size.sh"
else
  echo 'all: skipping consumer.sh and size.sh (no Arm toolchain, or libDaisy is not built)'
fi
