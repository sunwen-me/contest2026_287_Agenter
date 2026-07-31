#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#
# Capture an unmodified K1 serial stream and write reproducibility metadata.

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import select
import signal
import sys
import termios
import time
import tty


BAUD_RATES = {
    9600: termios.B9600,
    19200: termios.B19200,
    38400: termios.B38400,
    57600: termios.B57600,
    115200: termios.B115200,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Capture K1 UART output")
    parser.add_argument("--device", required=True, help="serial device")
    parser.add_argument("--output", required=True, help="raw log output path")
    parser.add_argument("--baud", type=int, choices=BAUD_RATES, default=115200)
    parser.add_argument(
        "--duration",
        type=float,
        default=0,
        help="stop after N seconds; zero waits until Ctrl-C",
    )
    return parser.parse_args()


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def configure_serial(fd: int, baud: int) -> None:
    tty.setraw(fd, termios.TCSANOW)
    attrs = termios.tcgetattr(fd)
    attrs[4] = BAUD_RATES[baud]
    attrs[5] = BAUD_RATES[baud]
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[2] &= ~(termios.PARENB | termios.CSTOPB)
    attrs[2] &= ~termios.CSIZE
    attrs[2] |= termios.CS8
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIFLUSH)


def main() -> int:
    args = parse_args()
    output = pathlib.Path(args.output).resolve()
    metadata = output.with_suffix(output.suffix + ".json")
    output.parent.mkdir(parents=True, exist_ok=True)

    stopped = False

    def stop_capture(_signum: int, _frame: object) -> None:
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop_capture)
    signal.signal(signal.SIGTERM, stop_capture)

    started_utc = utc_now()
    started = time.monotonic()
    byte_count = 0
    digest = hashlib.sha256()
    fd = os.open(
        args.device,
        os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK,
    )

    try:
        configure_serial(fd, args.baud)
        with output.open("wb", buffering=0) as stream:
            while not stopped:
                if args.duration > 0 and time.monotonic() - started >= args.duration:
                    break

                readable, _, _ = select.select([fd], [], [], 0.25)
                if not readable:
                    continue

                data = os.read(fd, 4096)
                if not data:
                    continue

                stream.write(data)
                digest.update(data)
                byte_count += len(data)
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
    finally:
        os.close(fd)

    record = {
        "device": args.device,
        "baud": args.baud,
        "format": "8N1 raw bytes",
        "started_utc": started_utc,
        "ended_utc": utc_now(),
        "duration_seconds": round(time.monotonic() - started, 3),
        "bytes": byte_count,
        "sha256": digest.hexdigest(),
        "log": str(output),
    }
    metadata.write_text(
        json.dumps(record, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"\nCaptured {byte_count} bytes to {output}", file=sys.stderr)
    print(f"Metadata: {metadata}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
