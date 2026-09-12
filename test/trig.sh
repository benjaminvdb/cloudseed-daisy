#!/bin/bash
# Checks the port's sin/cos reduction (src/cloudseed/fdlibm_trig.cpp)
# against the fdlibm source: unfused it must match bit for bit, fused (the
# Daisy Seed's libm) within one ulp.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
for MODE in unfused fused; do
  DEFS="-DCLOUDSEED_TRIG_FDLIBM=1"
  [ "$MODE" = unfused ] && DEFS="$DEFS -DCLOUDSEED_TRIG_UNFUSED=1"
  "${CXX:-g++}" -O2 -std=c++14 -Wall -Wextra -Werror -ffp-contract=off $DEFS \
    -I "$HERE/../src" "$HERE/trig.cpp" "$HERE/../src/cloudseed/fdlibm_trig.cpp" \
    -o "$WORK/trig"
  "$WORK/trig" "$MODE"
done
