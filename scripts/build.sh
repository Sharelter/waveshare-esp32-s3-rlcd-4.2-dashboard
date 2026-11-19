#!/usr/bin/env bash
# Compile the dashboard firmware.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
require_cli

mkdir -p "$BUILD_DIR"

# Third-party libraries come from the Arduino Library Manager (see
# scripts/fetch-toolchain.sh).  An optional in-tree firmware/libraries/ still
# takes precedence when present, which is handy for pinning a patched fork.
libs_args=()
[ -d "$EXTRA_LIBS" ] && libs_args=(--libraries "$EXTRA_LIBS")

"$CLI" compile \
  --fqbn "$FQBN" \
  "${libs_args[@]}" \
  --warnings default \
  --output-dir "$BUILD_DIR" \
  "$SKETCH"
