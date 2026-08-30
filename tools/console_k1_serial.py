#!/usr/bin/env python3
"""Interactive K1 serial console.

The stock U-Boot console accepts CR as the command terminator, while the
minimal NuttX readline configuration used by this board accepts LF.  The
console mode follows the prompt seen on the wire and translates the local
Enter key accordingly.
"""

from __future__ import annotations

import argparse
import os
import select
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

SERIAL_BYTE_DELAY = 0.002


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Interactive K1 UART console")
    parser.add_argument("--device", required=True, help="serial device")
    parser.add_argument("--baud", type=int, choices=BAUD_RATES, default=115200)
    parser.add_argument(
        "--stop-autoboot",
        action="store_true",
        help="send U-Boot interrupt characters after its banner is detected",
    )
    parser.add_argument(
        "--exit-on-uboot",
        action="store_true",
        help="exit after stopping at a U-Boot prompt; do not read local input",
    )
    return parser.parse_args()


def configure_serial(fd: int, baud: int) -> None:
    tty.setraw(fd, termios.TCSANOW)
    attrs = termios.tcgetattr(fd)
    attrs[4] = BAUD_RATES[baud]
    attrs[5] = BAUD_RATES[baud]
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[2] &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE)
    attrs[2] |= termios.CS8
    if hasattr(termios, "CRTSCTS"):
        attrs[2] &= ~termios.CRTSCTS
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def write_serial(fd: int, data: bytes) -> None:
    """Write all bytes to the non-blocking serial descriptor."""
    offset = 0
    while offset < len(data):
        try:
            written = os.write(fd, data[offset:offset + 1])
        except BlockingIOError:
            written = 0

        if written > 0:
            offset += written
            if offset < len(data):
                time.sleep(SERIAL_BYTE_DELAY)
            continue

        _, writable, _ = select.select([], [fd], [], 1.0)
        if not writable:
            raise TimeoutError("timed out writing to serial device")


def main() -> int:
    args = parse_args()
    serial_fd = os.open(
        args.device,
        os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK,
    )
    stdin_fd = sys.stdin.fileno()
    old_stdin = None
    mode = "cr"
    output_tail = bytearray()
    stopping_autoboot = False
    autoboot_stopped = False
    autoboot_sent = False
    autoboot_send_at = 0.0
    autoboot_stop_at = 0.0

    try:
        configure_serial(serial_fd, args.baud)
        if not args.exit_on_uboot:
            old_stdin = termios.tcgetattr(stdin_fd)
            tty.setraw(stdin_fd, termios.TCSANOW)

        termios.tcflush(serial_fd, termios.TCIFLUSH)

        print(
            "Interactive K1 console. Ctrl-C is sent to the board; Ctrl-D exits.",
            file=sys.stderr,
        )

        while True:
            timeout = 0.1 if stopping_autoboot and not autoboot_stopped else None
            read_fds = [serial_fd]
            if not args.exit_on_uboot:
                read_fds.append(stdin_fd)

            readable, _, _ = select.select(read_fds, [], [], timeout)

            if serial_fd in readable:
                try:
                    data = os.read(serial_fd, 4096)
                except BlockingIOError:
                    data = b""

                if data:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()

                    # A reboot can occur while this console process remains
                    # attached.  Reset the one-shot U-Boot interrupt state
                    # when the next SPL banner arrives, otherwise a previous
                    # ``=>`` prompt suppresses interception on later boots.
                    if b"U-Boot SPL" in data:
                        stopping_autoboot = False
                        autoboot_stopped = False
                        autoboot_sent = False
                        output_tail.clear()

                    output_tail.extend(data)
                    if len(output_tail) > 4096:
                        del output_tail[:-4096]

                    tail = bytes(output_tail)
                    if b"nsh>" in tail:
                        if mode != "lf":
                            mode = "lf"
                            print("\n[Enter -> LF for NSH]", file=sys.stderr)
                    elif b"=>" in tail:
                        stopping_autoboot = False
                        autoboot_stopped = True
                        if mode != "cr":
                            mode = "cr"
                            print("\n[Enter -> CR for U-Boot]", file=sys.stderr)

                        if args.exit_on_uboot:
                            return 0

                    # SPL prints "U-Boot SPL" before the main U-Boot console
                    # is ready.  Interrupt only after the main U-Boot banner
                    # and delay until the known K1 autoboot window.
                    if (args.stop_autoboot and not autoboot_stopped and
                            b"U-Boot 2022" in tail):
                        if not stopping_autoboot:
                            stopping_autoboot = True
                            now = time.monotonic()
                            autoboot_sent = False
                            autoboot_send_at = now + 2.2
                            autoboot_stop_at = now + 5.0
                            print("\n[Waiting for K1 U-Boot stop window]",
                                  file=sys.stderr)

            # K1 reaches "Autoboot in 0 seconds" about 2.4 seconds after
            # the main U-Boot banner.  Send exactly one character shortly
            # before that point.  A repeated batch leaves stale characters
            # in the U-Boot input FIFO after it reaches the prompt.
            if stopping_autoboot and not autoboot_stopped:
                now = time.monotonic()
                if now >= autoboot_stop_at:
                    stopping_autoboot = False
                    print("\n[U-Boot stop window expired]", file=sys.stderr)
                elif not autoboot_sent and now >= autoboot_send_at:
                    write_serial(serial_fd, b"s")
                    autoboot_sent = True

            if not args.exit_on_uboot and stdin_fd in readable:
                data = os.read(stdin_fd, 4096)
                if not data:
                    return 0

                if b"\x04" in data:
                    return 0

                translated = bytearray()
                for byte in data:
                    if byte in (0x0A, 0x0D):
                        translated.append(0x0A if mode == "lf" else 0x0D)
                    else:
                        translated.append(byte)

                if translated:
                    write_serial(serial_fd, bytes(translated))
    finally:
        if old_stdin is not None:
            termios.tcsetattr(stdin_fd, termios.TCSANOW, old_stdin)

        os.close(serial_fd)


if __name__ == "__main__":
    raise SystemExit(main())
