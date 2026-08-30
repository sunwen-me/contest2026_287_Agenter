#!/usr/bin/env python3
"""Run U-Boot commands one at a time over the K1 USB-TTL console."""

from __future__ import annotations

import argparse
import pathlib
import sys

from load_k1_xmodem import BAUD_RATES, K1Xmodem, XmodemError


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run one or more K1 U-Boot commands without touching storage"
    )
    parser.add_argument(
        "--command", action="append", required=True, help="one U-Boot command"
    )
    parser.add_argument("--device", default="/dev/ttyUSB0", help="USB-TTL device")
    parser.add_argument("--baud", type=int, choices=BAUD_RATES, default=115200)
    parser.add_argument("--log", type=pathlib.Path, help="write the serial transcript")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    serial = K1Xmodem(args.device, args.baud, args.log)

    try:
        # The autoboot interceptor can leave a partial command line in the
        # U-Boot FIFO.  Clear it and confirm the prompt before issuing the
        # caller's first command.
        serial.write_text(b"\x15\r")
        serial.wait_for_text(b"=>", 5.0)

        for command in args.command:
            start = len(serial.received)
            serial.command(command)
            serial.wait_for_text(b"=>", 10.0, start)
    finally:
        serial.close()

    print(f"PASS: completed {len(args.command)} U-Boot command(s)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except XmodemError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
