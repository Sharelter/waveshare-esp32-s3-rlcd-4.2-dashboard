#!/usr/bin/env bash
# Flash the compiled firmware and open a serial monitor.
#
#   scripts/flash.sh              # build (if needed) + upload + monitor
#   scripts/flash.sh --no-build   # upload the existing build/ output
#   scripts/flash.sh --no-monitor # upload only
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
require_cli

DO_BUILD=1
DO_MONITOR=1
for arg in "$@"; do
  case "$arg" in
    --no-build) DO_BUILD=0 ;;
    --no-monitor) DO_MONITOR=0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

if [ "$DO_BUILD" = 1 ]; then
  "$ROOT/scripts/build.sh"
fi

echo ">> uploading to $PORT"
"$CLI" upload -p "$PORT" --fqbn "$FQBN" --input-dir "$BUILD_DIR" "$SKETCH"

if [ "$DO_MONITOR" = 1 ]; then
  echo ">> monitoring $PORT (ctrl-c to quit)"
  sleep 1
  python3 "$ROOT/host/serial_monitor.py" -p "$PORT"
fi
