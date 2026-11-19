#!/usr/bin/env bash
# Install a self-contained Rust toolchain under tools/ (no root, nothing touched
# outside the project).
#
# The host also needs a C toolchain for linking; on AOSC OS that is the
# `devel-base` package (gcc / make / pkg-config).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS="$ROOT/tools"

export RUSTUP_HOME="$TOOLS/rustup"
export CARGO_HOME="$TOOLS/cargo"
export PATH="$CARGO_HOME/bin:$PATH"

mkdir -p "$RUSTUP_HOME" "$CARGO_HOME"

if [ ! -x "$CARGO_HOME/bin/cargo" ]; then
  echo ">> downloading rustup-init"
  curl -fL --retry 5 --retry-delay 3 -o "$TOOLS/rustup-init" \
    https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init
  chmod +x "$TOOLS/rustup-init"
  echo ">> installing stable toolchain (minimal profile, ~200 MB)"
  "$TOOLS/rustup-init" -y --no-modify-path --profile minimal \
    --default-toolchain stable 2>&1 | tail -20
fi

echo "=== versions ==="
"$CARGO_HOME/bin/rustc" --version
"$CARGO_HOME/bin/cargo" --version

if ! command -v cc >/dev/null 2>&1; then
  echo
  echo "!! no C toolchain found: Rust needs a linker (cc) to build binaries."
  echo "   on AOSC OS:  sudo oma install devel-base"
fi
