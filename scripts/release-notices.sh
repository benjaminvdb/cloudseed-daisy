#!/bin/bash
# Companion notices for the example binary, from the actual build inputs.
# Usage: release-notices.sh libDaisy toolchain-license.txt > notices.txt
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
LIBDAISY_DIR=${1:?provide the libDaisy checkout used for the image}
TOOLCHAIN_LICENSE=${2:?provide the Arm toolchain distribution license.txt}

notice() {
  printf '\n%s\n\n' "$1"
  cat "$2"
  printf '\n'
}

cat <<'TEXT'
Cloud Seed Daisy example firmware notices

Keep this file with the firmware when redistributing it. The source archive
contains the library sources and their file-level notices. libDaisy and the
Arm toolchain are separate build inputs; their notices follow below.
The toolchain's complete notice collection includes compiler tools as well
as the newlib, libgloss and GCC runtime code linked into this image.
TEXT
notice 'cloudseed-daisy' "$REPO/LICENSE"
notice 'Cloud Seed, Cloud Seed 2, SHA-256 and fdlibm' "$REPO/src/cloudseed/license.txt"
notice 'libDaisy' "$LIBDAISY_DIR/LICENSE"
notice 'STM32 HAL' "$LIBDAISY_DIR/Drivers/STM32H7xx_HAL_Driver/LICENSE.md"
notice 'STM32 USB device library' "$LIBDAISY_DIR/Middlewares/ST/STM32_USB_Device_Library/LICENSE.md"
notice 'CMSIS core' "$LIBDAISY_DIR/Drivers/CMSIS_5/LICENSE.txt"
notice 'CMSIS STM32H7 device support' "$LIBDAISY_DIR/Drivers/CMSIS-Device/ST/STM32H7xx/LICENSE.md"
cat <<'TEXT'

STM32 USB host library: Copyright (c) 2015 STMicroelectronics.
Licensed under SLA0044 (Ultimate Liberty):
https://www.st.com/resource/en/license_agreement/dm00218346.pdf
This image is a software update for an STMicroelectronics STM32H750 device.

TEXT
notice 'Arm GNU Toolchain distribution notices (including linked runtimes)' "$TOOLCHAIN_LICENSE"
