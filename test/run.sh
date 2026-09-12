#!/bin/bash
#
# Builds and runs test/fidelity.cpp on the PC: the port in src/cloudseed
# against the corrected legacy Cloud Seed reference, with 4 and 12 delay
# lines.
#
# Usage: ./run.sh path/to/CloudSeed
#   where path/to/CloudSeed is a checkout of
#   https://github.com/ValdemarOrn/CloudSeed (its CloudSeed.Native folder is
#   used). Needs g++ and Python 3.
set -euo pipefail

CLOUDSEED=${1:?usage: $0 path/to/CloudSeed-checkout}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

python3 "$HERE/verify_presets.py" "$CLOUDSEED"
python3 "$HERE/prepare_reference.py" "$CLOUDSEED" "$WORK/ref"

for LINES in 4 12; do
  "${CXX:-g++}" -O2 -std=c++14 -D__forceinline=inline \
    -include cmath -include cstring -include cstdlib \
    -DCLOUDSEED_MAX_LINES="$LINES" -I "$WORK/ref" -I "$HERE/../src" \
    "$WORK"/ref/AudioLib/*.cpp "$WORK"/ref/FastSin.cpp \
    "$WORK"/ref/Utils/Sha256.cpp "$HERE"/../src/cloudseed/*.cpp \
    "$HERE"/fidelity.cpp -o "$WORK/fidelity"

  echo "=== $LINES lines, modulation off: corrected-reference regression ==="
  "$WORK/fidelity" nomod
  echo
  echo "=== $LINES lines, modulation on: envelope regression (different phases) ==="
  "$WORK/fidelity"
done
