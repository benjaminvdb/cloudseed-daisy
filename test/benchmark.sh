#!/bin/bash
# Desktop timings of the ring paths, per program. Optional --stress renders
# 120 seconds per program at the DSP's compile-time line cap, frozen after
# a second, and checks that every output stays finite.
# CLOUDSEED_DSP_ROOT can point to a saved pre-change copy of this repository.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
DSP_ROOT=${CLOUDSEED_DSP_ROOT:-$HERE/..}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"${CXX:-g++}" -O2 -ffp-contract=off -std=c++14 -Wall -Wextra \
  -I "$DSP_ROOT/src" "$HERE/benchmark.cpp" "$DSP_ROOT"/src/cloudseed/*.cpp \
  -o "$WORK/benchmark"
"$WORK/benchmark" "$@"
