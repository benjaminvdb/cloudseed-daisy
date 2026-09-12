#!/bin/bash
# Every host suite. The fidelity suite needs a CloudSeed checkout as its
# argument; the engine and MDMA suites need LIBDAISY_DIR (see their files).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
bash "$HERE/build.sh"
python3 "$HERE/render.py"
bash "$HERE/regression.sh"
bash "$HERE/engine.sh"
bash "$HERE/mdma.sh"
bash "$HERE/trig.sh"
if [ $# -ge 1 ]; then bash "$HERE/run.sh" "$1"; fi
bash "$HERE/benchmark.sh" --stress
