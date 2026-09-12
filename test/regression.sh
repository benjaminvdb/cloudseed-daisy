#!/bin/bash
# Host memory/undefined-behavior checks of the DSP and the staging, without
# any hardware or legacy code.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
for LINES in 4 12; do
  for STAGED in 1 0; do
    # STAGED=0 compiles different interleaved/wrap-free ring kernels.
    "${CXX:-g++}" -O2 -ffp-contract=off -g -std=c++14 -Wall -Wextra -Werror \
      -fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer \
      -fno-sanitize-recover=all -DCLOUDSEED_MAX_LINES="$LINES" \
      -DCLOUDSEED_STAGED_MEMORY="$STAGED" -I "$HERE/../src" \
      "$HERE/regression.cpp" "$HERE"/../src/cloudseed/*.cpp -o "$WORK/regression"
    "$WORK/regression"
  done
done
for LINES in 0 13; do
  if "${CXX:-g++}" -std=c++14 -DCLOUDSEED_MAX_LINES="$LINES" \
      -x c++ -c "$HERE/../src/cloudseed/config.h" -o "$WORK/invalid.o" \
      > "$WORK/invalid.log" 2>&1; then
    echo "FAIL: invalid line count $LINES compiled" >&2
    exit 1
  fi
done
