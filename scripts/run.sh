#!/usr/bin/env bash
# Start the host collector that feeds the board.
#
#   scripts/run.sh                 use the Rust host (builds it if needed)
#   scripts/run.sh --py [args...]  use the Python reference implementation
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/host-rust/target/release/rlcd-host"

if [ "${1:-}" = "--py" ]; then
  shift
  exec python3 "$ROOT/host/rlcd_host.py" "$@"
fi

if [ ! -x "$BIN" ]; then
  echo ">> rust host not built yet, building it" >&2
  "$ROOT/scripts/build-host.sh" >&2
fi

exec "$BIN" "$@"
