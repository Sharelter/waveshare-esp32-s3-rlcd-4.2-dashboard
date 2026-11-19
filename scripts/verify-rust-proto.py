# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env python3
"""Cross-check the Rust host's wire output against the Python protocol decoder.

Runs the Rust binary against a PTY, captures its frames, and decodes them with
the exact struct format the Python host uses (`host/rlcd_host.py`), which in turn
mirrors `firmware/rlcd_dash/protocol.h`.  Anything the firmware would misread
shows up here as a decode error or a nonsense field.

    python3 scripts/verify-rust-proto.py [path-to-rlcd-host]
"""

from __future__ import annotations

import importlib.util
import os
import pty
import select
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_python_host():
    spec = importlib.util.spec_from_file_location("rlcd_host", os.path.join(ROOT, "host", "rlcd_host.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main() -> int:
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, "host-rust", "target", "release", "rlcd-host")
    if not os.path.exists(binary):
        print(f"!! {binary} not found; run scripts/build-host.sh", file=sys.stderr)
        return 2

    ph = load_python_host()
    master, slave = pty.openpty()
    port = os.ttyname(slave)

    proc = subprocess.Popen(
        [binary, "-p", port, "-r", "20", "-q"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )

    buf = b""
    frames: list[bytes] = []
    deadline = time.time() + 6.0
    try:
        while time.time() < deadline and len(frames) < 40:
            ready, _, _ = select.select([master], [], [], 0.3)
            if not ready:
                continue
            try:
                buf += os.read(master, 65536)
            except OSError:
                break
            # resync on the magic
            while True:
                i = buf.find(ph.MAGIC)
                if i < 0:
                    buf = b""
                    break
                need = 3 + ph.PAYLOAD_LEN + 2
                if len(buf) - i < need:
                    buf = buf[i:]
                    break
                frame = buf[i:i + need]
                buf = buf[i + need:]
                if frame[2] != ph.PAYLOAD_LEN:
                    continue
                payload = frame[3:3 + ph.PAYLOAD_LEN]
                want = struct.unpack("<H", frame[3 + ph.PAYLOAD_LEN:])[0]
                if ph.crc16(payload) != want:
                    print("!! CRC mismatch on a captured frame")
                    return 1
                frames.append(payload)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        os.close(master)
        os.close(slave)

    if not frames:
        print("!! no frames captured")
        return 1

    print(f"captured {len(frames)} frames, all CRC-valid, len={ph.PAYLOAD_LEN}")
    print(f"{'field':<22}{'frame[0]':>12}{'frame[-1]':>12}   {'python now':>12}")
    print("-" * 62)

    def unpack(p: bytes):
        vals = struct.unpack(ph._PAYLOAD_FMT, p)
        (seq, ncpu, cpu_total, loads, mu, mt, su, st, l1, l5, l15, temp, up,
         nif, _flags, hh, mm, ss, host, ifblob) = vals
        ifaces = []
        for k in range(ph.MAX_IFACES):
            nm, rx, tx, upf, _ = struct.unpack(
                ph._IFACE_FMT, ifblob[k * 20:(k + 1) * 20])
            ifaces.append((nm.rstrip(b"\0").decode(), rx, tx, upf))
        return dict(seq=seq, ncpu=ncpu, cpu_total=cpu_total, loads=loads, mu=mu, mt=mt,
                    su=su, st=st, load=(l1, l5, l15), temp=temp, uptime=up, nif=nif,
                    hms=(hh, mm, ss), host=host.rstrip(b"\0").decode(), ifaces=ifaces)

    a = unpack(frames[0])
    b = unpack(frames[-1])

    # live python-side values for comparison
    cpu_s = ph.CpuSampler(); cpu_s.sample(); time.sleep(0.2)
    cpu = cpu_s.sample()
    mem = ph.read_meminfo()
    net = ph.NetSampler(None).sample()
    py = dict(
        ncpu=len(cpu), cpu_total=int(round(sum(cpu) / max(len(cpu), 1) * 10)),
        mu=(mem["MemTotal"] - mem.get("MemAvailable", 0)) // 1024, mt=mem["MemTotal"] // 1024,
        su=(mem["SwapTotal"] - mem["SwapFree"]) // 1024, st=mem["SwapTotal"] // 1024,
        temp=ph.read_temp(ph.find_cpu_temp_path()),
        load=ph.read_loadavg(), host=__import__("socket").gethostname()[:15],
        ifaces=[(n, r, t, u) for n, r, t, u in net],
    )

    rows = [
        ("seq", a["seq"], b["seq"], ""),
        ("ncpu", a["ncpu"], b["ncpu"], py["ncpu"]),
        ("cpu_total x10", a["cpu_total"], b["cpu_total"], py["cpu_total"]),
        ("load[0] x100", a["load"][0], b["load"][0], py["load"][0]),
        ("mem_used_mb", a["mu"], b["mu"], py["mu"]),
        ("mem_total_mb", a["mt"], b["mt"], py["mt"]),
        ("swap_used_mb", a["su"], b["su"], py["su"]),
        ("swap_total_mb", a["st"], b["st"], py["st"]),
        ("temp x10", a["temp"], b["temp"], py["temp"]),
        ("uptime_s", a["uptime"], b["uptime"], ""),
        ("niface", a["nif"], b["nif"], len(py["ifaces"])),
        ("clock h:m:s", a["hms"], b["hms"], time.localtime()[3:6]),
        ("host", a["host"], b["host"], py["host"]),
    ]
    for name, x, y, z in rows:
        print(f"{name:<22}{str(x):>12}{str(y):>12}   {str(z):>12}")

    print(f"{'cpu_load[0..7]':<22}{str(a['loads'][:8]):>12}")
    for i in range(max(a["nif"], 1)):
        print(f"  iface[{i}] rx/tx", a["ifaces"][i], " python:", py["ifaces"][i] if i < len(py["ifaces"]) else "-")

    ok = True
    checks = [
        (a["ncpu"] == py["ncpu"], "ncpu"),
        (abs(a["mu"] - py["mu"]) < 512, "mem_used within 512 MB"),
        (a["mt"] == py["mt"], "mem_total exact"),
        (a["st"] == py["st"], "swap_total exact"),
        (abs(a["temp"] - py["temp"]) < 200, "temp within 20 C"),
        (a["host"] == py["host"], "hostname"),
        (a["nif"] >= 1, "at least one iface"),
        (a["ifaces"][0][0] == py["ifaces"][0][0] if py["ifaces"] else True, "primary iface name"),
        (b["seq"] == (a["seq"] + len(frames) - 1) & 0xFFFF, "seq increments by 1"),
    ]
    for good, what in checks:
        if not good:
            ok = False
            print(f"  FAIL: {what}")
    print("\nRESULT:", "OK" if ok else "MISMATCH")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
