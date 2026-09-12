#!/bin/bash
# Inspect the real dependency's expanded compile recipes: libDaisy assigns
# CPPFLAGS itself, so testing only cloudseed.mk's text misses flag loss.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
LIBDAISY_DIR=${LIBDAISY_DIR:-$HERE/../../libDaisy}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
for OPTIONS in '1 0' '1 1' '0 0' '2 0'; do
  read -r STAGING PROFILE <<< "$OPTIONS"
  make -C "$HERE/../examples/seed" -Bn \
    LIBDAISY_DIR="$LIBDAISY_DIR" BUILD_DIR="$WORK/objects" \
    CLOUDSEED_STAGING="$STAGING" CLOUDSEED_PROFILE="$PROFILE" \
    > "$WORK/recipes"
  python3 - "$WORK/recipes" <<'PY'
import pathlib
import shlex
import sys

commands = [shlex.split(line) for line in pathlib.Path(sys.argv[1]).read_text().splitlines()
            if ' -c ' in line and '.cpp' in line]
assert len(commands) >= 9, 'expected application, DSP and engine compiler recipes'
for command in commands:
    contract = [arg for arg in command if arg.startswith('-ffp-contract=')]
    assert contract and contract[-1] == '-ffp-contract=off', command
    if any(arg.endswith('/reverb_channel.cpp') for arg in command):
        assert [arg for arg in command if arg.startswith('-O')][-1] == '-O2', command
PY
done
echo 'libDaisy build integration: 4 configurations passed'
