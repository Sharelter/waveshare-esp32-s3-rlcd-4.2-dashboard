#!/usr/bin/env bash
# Common environment for the arduino-cli based ESP32 builds.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export ROOT

export ARDUINO_DIRECTORIES_DATA="${ARDUINO_DIRECTORIES_DATA:-$ROOT/tools/arduino15}"
export ARDUINO_DIRECTORIES_DOWNLOADS="${ARDUINO_DIRECTORIES_DOWNLOADS:-$ROOT/tools/downloads}"
export ARDUINO_DIRECTORIES_USER="${ARDUINO_DIRECTORIES_USER:-$ROOT/tools/user}"

CLI="${CLI:-$ROOT/tools/bin/arduino-cli}"
export CLI

SKETCH="$ROOT/firmware/rlcd_dash"
BUILD_DIR="$ROOT/build"
# Optional: an in-tree library dir (e.g. a patched U8g2 fork) that overrides the
# Library Manager copy.  Absent by default; see scripts/build.sh.
EXTRA_LIBS="$ROOT/firmware/libraries"
PORT="${PORT:-/dev/ttyACM0}"

# ESP32-S3-WROOM-1-N16R8, native USB-Serial-JTAG as the console/data link.
FQBN="${FQBN:-esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,FlashMode=qio,PartitionScheme=huge_app,PSRAM=disabled,CPUFreq=240,UploadSpeed=921600,DebugLevel=none}"
export SKETCH BUILD_DIR EXTRA_LIBS PORT FQBN

require_cli() {
  if [ ! -x "$CLI" ]; then
    echo "arduino-cli not found at $CLI" >&2
    echo "run scripts/fetch-toolchain.sh first" >&2
    exit 1
  fi
}
