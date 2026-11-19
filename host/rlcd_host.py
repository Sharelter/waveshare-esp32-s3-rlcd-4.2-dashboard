# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env python3
"""Host side of the ESP32-S3-RLCD-4.2 load dashboard.

Samples CPU / memory / network on this machine and pushes a compact binary
frame to the board's native USB serial port (USB-Serial-JTAG) at a steady
30 Hz.  The board renders it on the 300x400 reflective LCD.

Standard library only -- no pyserial needed, the tty is configured with termios.
"""

from __future__ import annotations

import argparse
import errno
import math
import os
import socket
import struct
import sys
import termios
import time

# ---------------------------------------------------------------- protocol ---
MAGIC = b"\xa5\x5a"
MAX_CPUS = 32
MAX_IFACES = 4
IFNAME_LEN = 8
HOST_LEN = 16
NO_TEMP = -32768

_IFACE_FMT = "<8sIIB3s"
_IFACE_SIZE = struct.calcsize(_IFACE_FMT)
# the interface array travels as one opaque blob so the format string stays flat
_PAYLOAD_FMT = "<" + "HHH32sIIIIHHHhIBBBBB16s" + f"{_IFACE_SIZE * MAX_IFACES}s"
PAYLOAD_LEN = struct.calcsize(_PAYLOAD_FMT)
FRAME_LEN = PAYLOAD_LEN + 5


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


# ---------------------------------------------------------------- sampling ---
def _read_file(path: str) -> str:
    with open(path, "rb") as fh:
        return fh.read().decode("utf-8", "replace")


class CpuSampler:
    def __init__(self) -> None:
        self.prev: list[tuple[int, int]] | None = None

    def sample(self) -> list[float]:
        """Return per-cpu busy percentages since the previous call."""
        per_cpu: list[tuple[int, int]] = []
        with open("/proc/stat", "rb") as fh:
            for line in fh:
                if not line.startswith(b"cpu"):
                    break
                parts = line.split()
                name = parts[0]
                if not name[3:].isdigit():
                    continue  # the aggregate "cpu " line
                vals = [int(x) for x in parts[1:]]
                idle = vals[3] + (vals[4] if len(vals) > 4 else 0)  # idle + iowait
                total = sum(vals)
                per_cpu.append((total - idle, total))

        if self.prev is None or len(self.prev) != len(per_cpu):
            self.prev = per_cpu
            return [0.0] * len(per_cpu)

        out = []
        for (pbusy, ptotal), (busy, total) in zip(self.prev, per_cpu):
            dt = total - ptotal
            out.append(100.0 * (busy - pbusy) / dt if dt > 0 else 0.0)
        self.prev = per_cpu
        return out


def read_meminfo() -> dict[str, int]:
    info = {}
    with open("/proc/meminfo", "rb") as fh:
        for line in fh:
            key, _, rest = line.decode().partition(":")
            info[key] = int(rest.split()[0])
    return info


def read_netdev() -> dict[str, tuple[int, int]]:
    out = {}
    with open("/proc/net/dev", "rb") as fh:
        for line in fh.readlines()[2:]:
            name, _, rest = line.decode().partition(":")
            fields = rest.split()
            if len(fields) >= 9:
                out[name.strip()] = (int(fields[0]), int(fields[8]))
    return out


def is_physical(name: str) -> bool:
    return os.path.exists(f"/sys/class/net/{name}/device")


def operstate(name: str) -> bool:
    try:
        return _read_file(f"/sys/class/net/{name}/operstate").strip() == "up"
    except OSError:
        return False


# Checked in this order -- NOT in /sys/class/hwmon enumeration order.  ACPI
# thermal zones ("acpitz") report a made up value (often a flat 20 C) and must
# never win over a real die sensor, even though they usually sit at hwmon0.
TEMP_PRIORITY = ("k10temp", "coretemp", "zenpower", "cpu_thermal", "soc_thermal", "acpitz")
# Within one chip, prefer the package/die reading over per-core ones.
TEMP_LABEL_HINTS = ("tctl", "tdie", "package", "cpu", "composite")
# Sensors that are definitely not the CPU.
TEMP_NEVER = ("nvme", "amdgpu", "radeon", "r8169_0_200:00")


def _hwmon_sensors() -> list[tuple[str, list[tuple[str, str]]]]:
    """[(chip name, [(label, sysfs path), ...]), ...] in hwmon index order."""
    out = []
    try:
        entries = sorted(os.listdir("/sys/class/hwmon"))
    except OSError:
        return out
    for entry in entries:
        base = f"/sys/class/hwmon/{entry}"
        try:
            name = _read_file(f"{base}/name").strip()
        except OSError:
            continue
        temps = []
        for fname in sorted(os.listdir(base)):
            if not (fname.startswith("temp") and fname.endswith("_input")):
                continue
            prefix = f"{base}/{fname[:-len('_input')]}"
            label = _read_file(f"{prefix}_label").strip() if os.path.exists(f"{prefix}_label") else ""
            temps.append((label, f"{base}/{fname}"))
        if temps:
            out.append((name, temps))
    return out


def _pick_from_chip(temps: list[tuple[str, str]]) -> str:
    for hint in TEMP_LABEL_HINTS:
        for label, path in temps:
            if hint in label.lower():
                return path
    for label, path in temps:  # otherwise prefer temp1
        if path.endswith("temp1_input"):
            return path
    return temps[0][1]


def find_cpu_temp_path(explicit: str | None = None) -> str | None:
    """Locate a CPU package/die temperature sensor, best first."""
    if explicit:
        return explicit
    sensors = _hwmon_sensors()
    by_name = {}
    for name, temps in sensors:
        by_name.setdefault(name, temps)
    for want in TEMP_PRIORITY:
        if want in by_name:
            return _pick_from_chip(by_name[want])

    extra = [t for name, temps in sensors if name not in TEMP_NEVER
             for t in [_pick_from_chip(temps)]]
    return extra[0] if extra else None


def describe_temp_sensor(path: str | None) -> str:
    if not path:
        return "none"
    parts = path.split("/")
    if len(parts) >= 5 and parts[1] == "class" and parts[2] == "hwmon":
        base = "/".join(parts[:4])
        try:
            chip = _read_file(f"{base}/name").strip()
        except OSError:
            chip = "?"
        prefix = path[: -len("_input")]
        label = _read_file(f"{prefix}_label").strip() if os.path.exists(f"{prefix}_label") else ""
        return f"{chip}/{parts[4]}{(' ' + label) if label else ''}"
    return path


def read_temp(path: str | None) -> int:
    if not path:
        return NO_TEMP
    try:
        return int(_read_file(path).strip()) // 100  # millidegrees -> decidegrees
    except (OSError, ValueError):
        return NO_TEMP


def read_loadavg() -> tuple[int, int, int]:
    parts = _read_file("/proc/loadavg").split()
    return tuple(int(round(float(parts[i]) * 100)) for i in range(3))  # type: ignore[return-value]


def read_uptime() -> int:
    return int(float(_read_file("/proc/uptime").split()[0]))


class NetSampler:
    def __init__(self, forced: str | None, top_n: int = MAX_IFACES) -> None:
        self.forced = forced
        self.top_n = top_n
        self.prev: dict[str, tuple[int, int]] = read_netdev()
        self.prev_t = time.monotonic()
        self.ema: dict[str, tuple[float, float]] = {}

    def _candidates(self, rates: dict[str, tuple[float, float]]) -> list[str]:
        if self.forced:
            return [self.forced]
        names = [n for n in rates if n != "lo"]
        physical = [n for n in names if is_physical(n)]
        pool = physical or names
        up = [n for n in pool if operstate(n)]
        pool = up or pool
        pool.sort(key=lambda n: rates[n][0] + rates[n][1], reverse=True)
        return pool[: self.top_n]

    def sample(self) -> list[tuple[str, int, int, int]]:
        now = time.monotonic()
        cur = read_netdev()
        dt = max(now - self.prev_t, 1e-6)
        rates: dict[str, tuple[float, float]] = {}
        for name, (rx, tx) in cur.items():
            prx, ptx = self.prev.get(name, (rx, tx))
            rates[name] = (max(0, rx - prx) / dt, max(0, tx - ptx) / dt)
        self.prev, self.prev_t = cur, now

        chosen = self._candidates(rates)
        alpha = 1.0 - math.exp(-dt / 0.4)  # rate independent smoothing
        out = []
        for name in chosen:
            rx, tx = rates[name]
            erx, etx = self.ema.get(name, (rx, tx))
            erx = erx + alpha * (rx - erx)
            etx = etx + alpha * (tx - etx)
            self.ema[name] = (erx, etx)
            out.append((name, int(erx), int(etx), 1 if operstate(name) else 0))
        return out


class Ema:
    """Time constant based exponential smoothing, so the displayed value feels
    the same whether we run at 10 Hz or 60 Hz."""

    def __init__(self, tau: float = 0.5) -> None:
        self.tau = tau
        self.value: float | None = None
        self.t: float | None = None

    def __call__(self, v: float, now: float | None = None) -> float:
        now = time.monotonic() if now is None else now
        if self.value is None or self.t is None:
            self.value = v
        else:
            dt = max(now - self.t, 1e-6)
            self.value += (1.0 - math.exp(-dt / self.tau)) * (v - self.value)
        self.t = now
        return self.value


class EmaList:
    """One Ema per logical CPU, resized when the core count changes."""

    def __init__(self, tau: float = 0.6) -> None:
        self.tau = tau
        self.emas: list[Ema] = []

    def __call__(self, values: list[float], now: float | None = None) -> list[float]:
        if len(self.emas) != len(values):
            self.emas = [Ema(self.tau) for _ in values]
        return [ema(v, now) for ema, v in zip(self.emas, values)]


# ------------------------------------------------------------------ serial ---
class SerialLink:
    """Raw termios writer/reader for the ESP32-S3 USB-Serial-JTAG tty."""

    def __init__(self, port: str) -> None:
        self.port = port
        self.fd: int | None = None
        self.pending = b""
        self.open()

    def open(self) -> None:
        fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(fd)
        # raw, 8N1, no flow control, no echo
        attrs[0] = 0  # iflag
        attrs[1] = 0  # oflag
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0  # lflag
        attrs[4] = termios.B115200
        attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        try:
            termios.tcflush(fd, termios.TCIOFLUSH)
        except termios.error:
            pass
        self.fd = fd
        self.pending = b""

    def reopen(self) -> None:
        self.close()
        for _ in range(100):
            try:
                self.open()
                return
            except OSError:
                time.sleep(0.5)
        raise SystemExit(f"could not reopen {self.port}")

    def close(self) -> None:
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None

    def write(self, data: bytes) -> bool:
        """Non blocking write; returns False if a frame had to be dropped.

        At most one frame is ever queued, so a backed up link costs one frame of
        latency instead of growing an unbounded backlog.
        """
        if self.fd is None:
            return False
        ok = not self.pending  # a still-queued frame means the previous one was lost
        self.pending = data
        while self.pending:
            try:
                n = os.write(self.fd, self.pending)
            except BlockingIOError:
                return False
            except OSError as exc:
                if exc.errno in (errno.EIO, errno.ENODEV, errno.EBADF):
                    self.pending = b""
                    self.reopen()
                    return False
                raise
            self.pending = self.pending[n:]
        return ok

    def drain(self) -> bytes:
        if self.fd is None:
            return b""
        chunks = []
        while True:
            try:
                chunk = os.read(self.fd, 8192)
            except BlockingIOError:
                break
            except OSError as exc:
                if exc.errno in (errno.EIO, errno.ENODEV, errno.EBADF):
                    return b"".join(chunks)
                raise
            if not chunk:
                break
            chunks.append(chunk)
        return b"".join(chunks)


# -------------------------------------------------------------------- main ---
def build_frame(seq: int, cpu: list[float], ema_total: Ema, mem: dict[str, int],
                ifaces: list[tuple[str, int, int, int]], loadv: tuple[int, int, int],
                temp: int, uptime: int, host: str) -> bytes:
    ncpu = min(len(cpu), MAX_CPUS)
    total = ema_total(sum(cpu) / len(cpu) if cpu else 0.0)
    loads = bytes(min(100, max(0, int(round(v)))) for v in cpu[:MAX_CPUS])
    loads = loads + bytes(MAX_CPUS - len(loads))

    mt = mem.get("MemTotal", 0) // 1024
    ma = mem.get("MemAvailable", mem.get("MemFree", 0)) // 1024
    st = mem.get("SwapTotal", 0) // 1024
    sf = mem.get("SwapFree", 0) // 1024

    iface_blob = b""
    for name, rx, tx, up in ifaces[:MAX_IFACES]:
        iface_blob += struct.pack(_IFACE_FMT, name.encode()[:IFNAME_LEN], rx, tx, up, b"\0\0\0")
    iface_blob += struct.pack(_IFACE_FMT, b"", 0, 0, 0, b"\0\0\0") * (MAX_IFACES - len(ifaces[:MAX_IFACES]))

    now = time.localtime()
    payload = struct.pack(
        _PAYLOAD_FMT,
        seq & 0xFFFF,
        ncpu,
        min(1000, max(0, int(round(total * 10)))),
        loads,
        max(0, mt - ma),
        mt,
        max(0, st - sf),
        st,
        loadv[0],
        loadv[1],
        loadv[2],
        temp,
        uptime,
        len(ifaces[:MAX_IFACES]),
        0,
        now.tm_hour,
        now.tm_min,
        now.tm_sec,
        host.encode()[:HOST_LEN].ljust(HOST_LEN, b"\0"),
        iface_blob,
    )
    assert len(payload) == PAYLOAD_LEN, (len(payload), PAYLOAD_LEN)
    return MAGIC + bytes([PAYLOAD_LEN]) + payload + struct.pack("<H", crc16(payload))


def human_rate(bps: float) -> str:
    if bps >= 1024 * 1024:
        return f"{bps / 1024 / 1024:.2f} MB/s"
    if bps >= 1024:
        return f"{bps / 1024:.1f} KB/s"
    return f"{bps:.0f} B/s"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", default="/dev/ttyACM0", help="serial device (default: %(default)s)")
    ap.add_argument("-r", "--hz", type=float, default=10.0, help="frame rate (default: %(default)s)")
    ap.add_argument("-i", "--iface", default=None, help="force a network interface (default: auto)")
    ap.add_argument("--temp", default=None,
                    help="force a temperature sensor, e.g. /sys/class/hwmon/hwmon3/temp1_input")
    ap.add_argument("-q", "--quiet", action="store_true", help="do not print board status lines")
    ap.add_argument("--dry-run", action="store_true", help="sample and print, do not open the serial port")
    ap.add_argument("--print-every", type=float, default=0.0,
                    help="seconds between host-side metric printouts (0 = off)")
    args = ap.parse_args()

    if args.dry_run:
        args.port = None

    host = socket.gethostname()[: HOST_LEN - 1]
    cpu_sampler = CpuSampler()
    net_sampler = NetSampler(args.iface)
    ema_total = Ema(0.5)
    ema_cores = EmaList(0.6)
    temp_path = find_cpu_temp_path(args.temp)
    if args.temp and not os.path.exists(args.temp):
        print(f"[host] warning: {args.temp} does not exist", file=sys.stderr)

    link = SerialLink(args.port) if args.port else None
    if link:
        print(f"[host] {host}: streaming to {args.port} at {args.hz:g} Hz "
              f"({FRAME_LEN} B/frame = {FRAME_LEN * args.hz / 1024:.1f} KB/s)", flush=True)
    print(f"[host] cpu temp sensor: {describe_temp_sensor(temp_path)}"
          f"{'' if temp_path else '  (dashboard will show --)'}", flush=True)

    cpu_sampler.sample()  # prime the deltas
    net_sampler.sample()

    period = 1.0 / args.hz
    next_t = time.perf_counter()
    seq = 0
    sent = dropped = 0
    missed = 0
    rx_buf = b""
    last_report = time.monotonic()
    last_print = last_report
    try:
        while True:
            next_t += period
            now_mono = time.monotonic()
            cpu = ema_cores(cpu_sampler.sample(), now_mono)
            mem = read_meminfo()
            ifaces = net_sampler.sample()
            loadv = read_loadavg()
            frame = build_frame(seq, cpu, ema_total, mem, ifaces, loadv,
                                read_temp(temp_path), read_uptime(), host)
            seq += 1

            if args.dry_run:
                if args.print_every and now_mono - last_print >= args.print_every:
                    last_print = now_mono
                    print(f"cpu={sum(cpu)/max(len(cpu),1):5.1f}%  "
                          f"mem={(mem.get('MemTotal',0)-mem.get('MemAvailable',0))/1048576:.1f}G  "
                          f"temp={read_temp(temp_path)/10:.1f}C  "
                          + "  ".join(f"{n} {human_rate(r)}/{human_rate(t)}" for n, r, t, _ in ifaces),
                          flush=True)
            elif link:
                if link.write(frame):
                    sent += 1
                else:
                    dropped += 1

            if link and sent and seq % 15 == 0:
                data = link.drain()
                if data:
                    rx_buf += data
                    while b"\n" in rx_buf:
                        line, _, rx_buf = rx_buf.partition(b"\n")
                        text = line.decode("utf-8", "replace").strip()
                        if text and not args.quiet:
                            print(f"[board] {text}", flush=True)
                    rx_buf = rx_buf[-256:]

            now = time.monotonic()
            if now - last_report >= 5.0:
                dt = now - last_report
                print(f"[host] {sent / dt:5.1f} frames/s sent, {dropped} dropped, {missed} late",
                      flush=True)
                sent = dropped = missed = 0
                last_report = now

            slack = next_t - time.perf_counter()
            if slack > 0:
                time.sleep(slack)
            else:
                missed += 1
                if slack < -0.5:
                    next_t = time.perf_counter()
    except KeyboardInterrupt:
        print("\n[host] stopped", flush=True)
    finally:
        if link:
            link.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
