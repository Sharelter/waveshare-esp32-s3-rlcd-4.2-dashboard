# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env python3
"""Tiny pyserial-free serial monitor for the board's USB-Serial-JTAG port."""

from __future__ import annotations

import argparse
import os
import sys
import termios
import time


def open_port(port: str) -> int:
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="ignored by USB-CDC, kept for symmetry")
    args = ap.parse_args()

    fd = open_port(args.port)
    sys.stdout.write(f"--- {args.port} ---\n")
    sys.stdout.flush()
    buf = b""
    try:
        while True:
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                time.sleep(0.03)
                continue
            except OSError:
                break
            if not chunk:
                time.sleep(0.03)
                continue
            buf += chunk
            while b"\n" in buf:
                line, _, buf = buf.partition(b"\n")
                sys.stdout.write(line.decode("utf-8", "replace").rstrip("\r") + "\n")
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
