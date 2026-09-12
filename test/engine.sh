#!/bin/bash
# The engine's callback and main-loop sides, under ASan and UBSan, with the
# real DSP and libDaisy's CpuLoadMeter on a deterministic clock. Needs a
# libDaisy checkout for its headers: LIBDAISY_DIR (default: a checkout next
# to this repository).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
LIBDAISY_DIR=${LIBDAISY_DIR:-$HERE/../../libDaisy}
if [ ! -f "$LIBDAISY_DIR/src/util/CpuLoadMeter.h" ]; then
  echo "LIBDAISY_DIR=$LIBDAISY_DIR is not a libDaisy checkout" >&2
  exit 1
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
for OPTIONS in '4 0 1' '1 0 1' '4 1 1' '4 0 0' '12 1 1'; do
  read -r LINES PROFILE STAGING <<< "$OPTIONS"
  "${CXX:-g++}" -O1 -ffp-contract=off -g -std=c++14 -Wall -Wextra -Werror \
  -DCLOUDSEED_MAX_LINES="$LINES" -DCLOUDSEED_PROFILE="$PROFILE" \
  -DCLOUDSEED_STAGING="$STAGING" -DCLOUDSEED_STAGED_MEMORY="$STAGING" \
  -fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer \
  -fno-sanitize-recover=all -I "$HERE/stubs" -I "$HERE/../src" \
  -I "$LIBDAISY_DIR/src" \
  "$HERE/engine.cpp" "$HERE"/../src/cloudseed/*.cpp -o "$WORK/engine"
  "$WORK/engine"
done
