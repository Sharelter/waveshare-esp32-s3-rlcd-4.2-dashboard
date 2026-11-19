#!/usr/bin/env bash
# Install / remove the rlcd-host dashboard as a systemd *system* service.
#
#   sudo scripts/install-service.sh              install, enable, start
#   sudo scripts/install-service.sh --uninstall  stop, disable, remove
#        scripts/install-service.sh --dry-run    print the unit, change nothing
#
# Options:
#   -u, --user NAME   account to run as    [default: $SUDO_USER, else $USER]
#   -p, --port DEV    serial device        [default: the board's by-id link]
#   -r, --hz N        frame rate           [default: 10]
#       --no-start    install the unit but do not enable/start it
#       --uninstall   stop, disable and remove everything
#       --dry-run     print the generated unit and exit
#
# Env overrides (used by the test suite, harmless otherwise):
#   RLCD_BIN_DIR    default /usr/local/bin
#   RLCD_UNIT_DIR   default /etc/systemd/system
#   RLCD_REPO_URL   Documentation= target; defaults to the local README
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEMPLATE="$ROOT/packaging/rlcd-host.service.in"
BIN_SRC="$ROOT/host-rust/target/release/rlcd-host"
UNIT_NAME="rlcd-host.service"

BIN_DIR="${RLCD_BIN_DIR:-/usr/local/bin}"
UNIT_DIR="${RLCD_UNIT_DIR:-/etc/systemd/system}"
BIN="$BIN_DIR/rlcd-host"
UNIT_PATH="$UNIT_DIR/$UNIT_NAME"

RUN_USER="${SUDO_USER:-${USER:-root}}"
PORT=""
HZ="10"
UNINSTALL=0
DRY_RUN=0
NO_START=0

die() { echo "error: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    -u|--user)    RUN_USER="$2"; shift 2 ;;
    -p|--port)    PORT="$2"; shift 2 ;;
    -r|--hz)      HZ="$2"; shift 2 ;;
    --no-start)   NO_START=1; shift ;;
    --uninstall)  UNINSTALL=1; shift ;;
    --dry-run)    DRY_RUN=1; shift ;;
    -h|--help)    sed -n '2,19p' "$0"; exit 0 ;;
    *)            die "unknown option: $1" ;;
  esac
done

detect_port() {
  # The by-id symlink survives re-enumeration, unlike a bare ttyACM number.
  local p
  for p in /dev/serial/by-id/*Espressif*-if00; do
    if [ -e "$p" ]; then echo "$p"; return; fi
  done
  echo "/dev/ttyACM0"
}

require_root() {
  [ "$(id -u)" = 0 ] || die "installing a system service needs root; re-run with sudo"
}

build_if_needed() {
  if [ ! -x "$BIN_SRC" ]; then
    echo ">> building the rust host"
    "$ROOT/scripts/build-host.sh"
  fi
}

# Where the unit points its Documentation= at: the project URL if you set one,
# otherwise the local README, otherwise nothing (the line is dropped).
doc_url() {
  if [ -n "${RLCD_REPO_URL:-}" ]; then
    echo "$RLCD_REPO_URL"
  elif [ -f "$ROOT/README.md" ]; then
    echo "file://$ROOT/README.md"
  fi
}

render_unit() {
  local doc
  doc="$(doc_url)"
  sed -e "s|@RLCD_USER@|$RUN_USER|g" \
      -e "s|@RLCD_BIN@|$BIN|g" \
      -e "s|@RLCD_PORT@|$PORT|g" \
      -e "s|@RLCD_HZ@|$HZ|g" \
      -e "s|@RLCD_DOC@|$doc|g" \
      "$TEMPLATE" | sed -e '/^Documentation=$/d'
}

# ------------------------------------------------------------- uninstall ---
if [ "$UNINSTALL" = 1 ]; then
  require_root
  echo ">> stopping and disabling $UNIT_NAME"
  systemctl disable --now "$UNIT_NAME" 2>/dev/null || true
  rm -f "$UNIT_PATH" "$BIN"
  systemctl daemon-reload
  systemctl reset-failed "$UNIT_NAME" 2>/dev/null || true
  echo ">> removed $UNIT_PATH and $BIN"
  exit 0
fi

# --------------------------------------------------------------- install ---
[ -n "$PORT" ] || PORT="$(detect_port)"

if [ "$DRY_RUN" = 1 ]; then
  build_if_needed
  echo "# binary : $BIN_SRC -> $BIN"
  echo "# unit   : $UNIT_PATH"
  echo "# runs as: $RUN_USER"
  echo
  render_unit
  exit 0
fi

require_root
build_if_needed

echo ">> installing $BIN_SRC -> $BIN"
install -D -m 0755 "$BIN_SRC" "$BIN"

echo ">> writing $UNIT_PATH"
install -d "$UNIT_DIR"
render_unit > "$UNIT_PATH"
chmod 0644 "$UNIT_PATH"

if [ "$NO_START" = 1 ]; then
  echo ">> --no-start: not enabling; do it with:"
  echo "     sudo systemctl daemon-reload && sudo systemctl enable --now $UNIT_NAME"
  exit 0
fi

echo ">> enabling and starting"
systemctl daemon-reload
systemctl enable --now "$UNIT_NAME"

sleep 2
systemctl --no-pager status "$UNIT_NAME" --lines=6 || true

echo
echo "----------------------------------------------------------------"
echo "logs:   journalctl -u $UNIT_NAME -f"
echo "reload: sudo systemctl restart $UNIT_NAME"
echo "remove: sudo $ROOT/scripts/install-service.sh --uninstall"
echo "----------------------------------------------------------------"
