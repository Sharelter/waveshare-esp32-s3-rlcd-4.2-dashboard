#!/usr/bin/env bash
# Pre-download the Arduino ESP32 core + toolchain using resumable curl transfers,
# then hand the cache to arduino-cli.  arduino-cli's own HTTP client gives up on
# slow/flaky links; curl -C - does not.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS="$ROOT/tools"
CLI="$TOOLS/bin/arduino-cli"
UPSTREAM_INDEX_URL="https://espressif.github.io/arduino-esp32/package_esp32_index.json"
# The upstream index also pulls the 566 MB RISC-V toolchain, which an ESP32-S3
# build never touches.  We keep a pruned copy of the index instead.
LOCAL_INDEX="$TOOLS/local_index/package_esp32_index.json"
INDEX_URL="file://$LOCAL_INDEX"
PLATFORM="esp32:esp32@3.3.0"
U8G2_VERSION="2.36.18"

export ARDUINO_DIRECTORIES_DATA="$TOOLS/arduino15"
export ARDUINO_DIRECTORIES_DOWNLOADS="$TOOLS/downloads"
export ARDUINO_DIRECTORIES_USER="$TOOLS/user"

mkdir -p "$ARDUINO_DIRECTORIES_DOWNLOADS/packages" "$TOOLS/local_index"

# --- make sure we have a pruned index to work from ----------------------------
prune_index() {
  python3 - "$1" "$LOCAL_INDEX" <<'PY'
import json, os, sys
src, dst = sys.argv[1], sys.argv[2]
SKIP = {"esp-rv32", "riscv32-esp-elf-gdb", "xtensa-esp-elf-gdb", "openocd-esp32"}
d = json.load(open(src))
for pkg in d["packages"]:
    if pkg["name"] != "esp32":
        continue
    for plat in pkg["platforms"]:
        if plat["version"] == "3.3.0":
            plat["toolsDependencies"] = [
                t for t in plat.get("toolsDependencies", []) if t["name"] not in SKIP
            ]
json.dump(d, open(dst, "w"))
print(f"pruned index -> {dst} ({os.path.getsize(dst)} bytes)")
PY
}

if [ ! -s "$LOCAL_INDEX" ]; then
  if [ ! -s "$ARDUINO_DIRECTORIES_DATA/package_esp32_index.json" ]; then
    echo ">> fetching upstream package index"
    "$CLI" core update-index --additional-urls "$UPSTREAM_INDEX_URL"
  fi
  prune_index "$ARDUINO_DIRECTORIES_DATA/package_esp32_index.json"
fi

HOST="x86_64-pc-linux-gnu"

# --- collect the URLs we need -------------------------------------------------
python3 - "$LOCAL_INDEX" "$HOST" <<'PY' > /tmp/esp32-urls.txt
import json, sys
path, host = sys.argv[1], sys.argv[2]
d = json.load(open(path))
urls = []
for pkg in d["packages"]:
    if pkg["name"] != "esp32":
        continue
    for plat in pkg["platforms"]:
        if plat["version"] != "3.3.0":
            continue
        urls.append((plat["url"], "platform"))
        for dep in plat.get("toolsDependencies", []):
            for t in pkg["tools"]:
                if t["name"] == dep["name"] and t["version"] == dep["version"]:
                    for s in t["systems"]:
                        if s["host"] == host:
                            urls.append((s["url"], t["name"]))
for u, n in urls:
    print(u)
PY

if [ ! -s /tmp/esp32-urls.txt ]; then
  echo "!! could not derive download list; is the package index present?" >&2
  exit 1
fi

echo "=== $(wc -l < /tmp/esp32-urls.txt) artifacts to download ==="
cat /tmp/esp32-urls.txt

# --- resumable parallel fetch -------------------------------------------------
fetch_one() {
  local url="$1" name dest
  name="$(basename "$url")"
  dest="$ARDUINO_DIRECTORIES_DOWNLOADS/packages/$name"
  for attempt in 1 2 3 4 5 6 7 8 9 10; do
    if curl -fL --connect-timeout 20 --retry 3 --retry-delay 5 --speed-time 60 --speed-limit 20000 \
            -C - -o "$dest" "$url"; then
      echo "OK   $name ($(du -h "$dest" | cut -f1))"
      return 0
    fi
    echo "retry $attempt: $name ($(du -h "$dest" 2>/dev/null | cut -f1))" >&2
    sleep 3
  done
  echo "FAIL $name" >&2
  return 1
}
export -f fetch_one
export ARDUINO_DIRECTORIES_DOWNLOADS

xargs -a /tmp/esp32-urls.txt -P 3 -I{} bash -c 'fetch_one "$@"' _ {}

echo "=== downloads finished, handing over to arduino-cli ==="
"$CLI" core install "$PLATFORM" --additional-urls "$INDEX_URL"

# U8g2 is a plain Library Manager dependency; there is no need to vendor 45 MB
# of fonts into this repository.  Pinned because the ST7305 driver is written
# against this generation of the u8g2 API.
echo "=== installing U8g2 $U8G2_VERSION ==="
"$CLI" lib install "U8g2@$U8G2_VERSION"

echo "=== core list ==="
"$CLI" core list
echo "=== libraries ==="
"$CLI" lib list 2>/dev/null | grep -A1 U8g2 || true
