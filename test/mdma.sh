#!/bin/bash
# Register-level checks of the MDMA transport against a stub channel. Needs
# a libDaisy checkout for the STM32 headers: LIBDAISY_DIR (default: a
# checkout next to this repository).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
LIBDAISY_DIR=${LIBDAISY_DIR:-$HERE/../../libDaisy}
if [ ! -d "$LIBDAISY_DIR/Drivers/STM32H7xx_HAL_Driver/Inc" ]; then
  echo "LIBDAISY_DIR=$LIBDAISY_DIR is not a libDaisy checkout" >&2
  exit 1
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
# The ARM-only vendor inline functions cast pointers to 32-bit registers.
# They are unused here; -fpermissive lets a 64-bit host parse those system
# headers. Warnings in our own code remain errors.
"${CXX:-g++}" -std=c++14 -O1 -g -fpermissive -Wall -Wextra -Werror -DSTM32H750xx \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I "$HERE/mdma_stubs" -I "$HERE/../src" \
  -isystem "$LIBDAISY_DIR/src/sys" \
  -isystem "$LIBDAISY_DIR/Drivers/CMSIS_5/CMSIS/Core/Include" \
  -isystem "$LIBDAISY_DIR/Drivers/CMSIS-Device/ST/STM32H7xx/Include" \
  -isystem "$LIBDAISY_DIR/Drivers/STM32H7xx_HAL_Driver/Inc" \
  "$HERE/mdma.cpp" -o "$WORK/mdma"
"$WORK/mdma"
