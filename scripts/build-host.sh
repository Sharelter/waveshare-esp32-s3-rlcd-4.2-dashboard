#!/usr/bin/env bash
# Build the Rust host.  Produces host-rust/target/release/rlcd-host
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export RUSTUP_HOME="$ROOT/tools/rustup"
export CARGO_HOME="$ROOT/tools/cargo"
export PATH="$CARGO_HOME/bin:$PATH"

if [ ! -x "$CARGO_HOME/bin/cargo" ]; then
  echo ">> rust toolchain missing, installing"
  "$ROOT/scripts/fetch-rust.sh"
fi

cd "$ROOT/host-rust"
exec cargo build --release "$@"
