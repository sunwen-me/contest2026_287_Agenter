#!/usr/bin/env python3
"""Load one file into K1 U-Boot RAM through its ``loadx`` command.

This helper uses XMODEM-CRC over the existing USB-TTL console.  It does not
issue ``go``, modify U-Boot environment, or access any storage device.
"""

from __future__ import annotations

import argparse
import os
import pathlib
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

XMODEM_SOH = 0x01
XMODEM_STX = 0x02
XMODEM_EOT = 0x04
XMODEM_ACK = 0x06
XMODEM_NAK = 0x15
XMODEM_CAN = 0x18
XMODEM_CRC = ord("C")
XMODEM_BLOCK_SIZE_128 = 128
XMODEM_BLOCK_SIZE_1K = 1024
XMODEM_RETRIES = 10
XMODEM_INTER_BLOCK_DELAY = 0.005
SERIAL_BYTE_DELAY = 0.002
XMODEM_CHUNK_SIZE = 32 * 1024


class XmodemError(RuntimeError):
    """The U-Boot XMODEM receiver did not complete the transfer."""


def parse_address(value: str) -> int:
    try:
        address = int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid address: {value}") from error

    if address < 0 or address > 0xffffffffffffffff:
        raise argparse.ArgumentTypeError(f"address out of range: {value}")

    return address


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Copy one local file into K1 U-Boot RAM using XMODEM-CRC"
    )
    parser.add_argument("file", type=pathlib.Path, help="local file to transfer")
    parser.add_argument("--address", required=True, type=parse_address)
    parser.add_argument("--device", default="/dev/ttyUSB0", help="USB-TTL device")
    parser.add_argument("--baud", type=int, choices=BAUD_RATES, default=115200)
    parser.add_argument(
        "--log", type=pathlib.Path, help="write the raw U-Boot transcript here"
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


def xmodem_crc16(data: bytes) -> int:
    crc = 0
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xffff
            else:
                crc = (crc << 1) & 0xffff

    return crc


class K1Xmodem:
    def __init__(
        self,
        device: str,
        baud: int,
        log_path: pathlib.Path | None,
        reconnect_on_reenumeration: bool = False,
    ) -> None:
        self.device = device
        self.baud = baud
        self.reconnect_on_reenumeration = reconnect_on_reenumeration
        self.fd = self._open_device(clear_input=True)
        self.opened_device = os.path.realpath(device)
        self.log = log_path.open("wb", buffering=0) if log_path else None
        self.received = bytearray()

    def _open_device(self, clear_input: bool) -> int:
        fd = os.open(self.device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        configure_serial(fd, self.baud)
        if clear_input:
            termios.tcflush(fd, termios.TCIFLUSH)
        return fd

    def _reopen_if_reenumerated(self) -> bool:
        """Follow a stable serial-by-id link after a USB-TTL re-enumeration."""

        if not self.reconnect_on_reenumeration:
            return False

        current_device = os.path.realpath(self.device)
        if current_device == self.opened_device or not os.path.exists(current_device):
            return False

        try:
            new_fd = self._open_device(clear_input=False)
        except OSError:
            # The USB node may vanish between the existence check and open.
            return False

        old_fd = self.fd
        self.fd = new_fd
        self.opened_device = current_device
        os.close(old_fd)
        print(f"[serial] re-opened {current_device}", file=sys.stderr)
        return True

    def close(self) -> None:
        if self.log is not None:
            self.log.close()

        os.close(self.fd)

    def write(self, data: bytes) -> None:
        offset = 0
        while offset < len(data):
            try:
                written = os.write(self.fd, data[offset:])
            except BlockingIOError:
                written = 0

            if written > 0:
                offset += written
                continue

            _, writable, _ = select.select([], [self.fd], [], 1.0)
            if not writable:
                raise XmodemError("timed out writing USB-TTL serial data")

    def write_text(self, data: bytes) -> None:
        """Send a U-Boot command at a rate its UART FIFO can accept."""

        for index, byte in enumerate(data):
            self.write(bytes((byte,)))
            if index + 1 < len(data):
                time.sleep(SERIAL_BYTE_DELAY)

    def read(self, timeout: float) -> bytes:
        self._reopen_if_reenumerated()
        try:
            readable, _, _ = select.select([self.fd], [], [], timeout)
        except OSError:
            # A disconnected tty can report EIO until udev publishes its new
            # stable-link target.  The next polling pass will reopen it.
            return b""

        if not readable:
            return b""

        try:
            data = os.read(self.fd, 4096)
        except BlockingIOError:
            # A USB serial adapter may report readiness and consume its FIFO
            # before this non-blocking read reaches the kernel.  Treat that
            # race as no data and continue until the caller's deadline.
            data = b""
        except OSError:
            return b""
        if data:
            self.received.extend(data)
            if self.log is not None:
                self.log.write(data)

            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()

        return data

    def wait_for_text(
        self, marker: bytes, timeout: float, start: int | None = None
    ) -> int:
        deadline = time.monotonic() + timeout
        offset = len(self.received) if start is None else start
        while time.monotonic() < deadline:
            marker_at = self.received.find(marker, offset)
            if marker_at >= 0:
                return marker_at + len(marker)

            self.read(max(0.0, min(0.2, deadline - time.monotonic())))

        output = bytes(self.received[offset:]).decode("utf-8", "replace")
        raise XmodemError(f"timed out waiting for {marker!r}: {output}")

    def wait_for_control(
        self, accepted: set[int], timeout: float, start: int | None = None,
    ) -> int:
        deadline = time.monotonic() + timeout
        offset = len(self.received) if start is None else start
        while time.monotonic() < deadline:
            data = bytes(self.received[offset:])
            offset = len(self.received)
            if XMODEM_CAN in data:
                raise XmodemError("U-Boot cancelled the XMODEM transfer")

            # U-Boot can leave its initial CRC request in the USB-TTL receive
            # buffer with a subsequent packet ACK.  The ACK proves the
            # receiver accepted this block, so it must win over the stale
            # request.
            if XMODEM_ACK in accepted and XMODEM_ACK in data:
                return XMODEM_ACK

            for value in data:
                if value in accepted:
                    return value

            self.read(max(0.0, min(0.2, deadline - time.monotonic())))

        raise XmodemError("timed out waiting for U-Boot XMODEM response")

    def command(self, text: str) -> None:
        print(f"[serial] {text}", file=sys.stderr)
        self.write_text(text.encode("ascii") + b"\r")

    def send_block(self, number: int, data: bytes,
                   inter_block_delay: float = 0.0) -> None:
        if len(data) == XMODEM_BLOCK_SIZE_128:
            start = XMODEM_SOH
        elif len(data) == XMODEM_BLOCK_SIZE_1K:
            start = XMODEM_STX
        else:
            raise XmodemError("invalid XMODEM block length")

        crc = xmodem_crc16(data)
        frame = bytes(
            (start, number & 0xff, (~number) & 0xff)
        ) + data + crc.to_bytes(2, "big")

        for _ in range(XMODEM_RETRIES):
            self.write(frame)
            response = self.wait_for_control(
                {XMODEM_ACK, XMODEM_NAK, XMODEM_CRC}, 3.0
            )
            if response == XMODEM_ACK:
                # The K1 console's xyzModem receiver can otherwise fall
                # behind a continuous 1 KiB UART burst after several blocks.
                if inter_block_delay > 0:
                    time.sleep(inter_block_delay)
                return

            # U-Boot xyzModem emits C, not NAK, after a timed-out, malformed,
            # or CRC-failed packet.  It still expects this same block number.
            print(f"[xmodem] retrying block {number}", file=sys.stderr)

        raise XmodemError(f"U-Boot rejected XMODEM block {number}")

    def begin_transfer(self, address: int) -> None:
        self.command(f"loadx 0x{address:x}")
        control_start = self.wait_for_text(b"Ready for binary", 10.0)
        if self.wait_for_control({XMODEM_CRC}, 10.0, control_start) != \
                XMODEM_CRC:
            raise XmodemError("U-Boot did not request XMODEM-CRC")

    def cancel_transfer(self) -> None:
        start = len(self.received)
        self.write(bytes((XMODEM_CAN,)) * 8)
        self.wait_for_text(b"=>", 5.0, start)

    def transfer(self, source: pathlib.Path, address: int) -> None:
        payload = source.read_bytes()
        # K1's U-Boot xyzModem receiver accepts both SOH (128-byte) and STX
        # (1024-byte) XMODEM blocks.  A firmware image with thousands of
        # 128-byte packets spends long enough awaiting per-packet ACKs to
        # collide with the board's U-Boot watchdog window.

        block_sizes = [XMODEM_BLOCK_SIZE_128]
        if len(payload) > XMODEM_BLOCK_SIZE_128:
            block_sizes.insert(0, XMODEM_BLOCK_SIZE_1K)

        for block_size_index, block_size in enumerate(block_sizes):
            transfer_active = False
            try:
                self.begin_transfer(address)
                transfer_active = True
                block_count = (len(payload) + block_size - 1) // block_size
                print(
                    f"[xmodem] {source} -> 0x{address:x}: {len(payload)} bytes, "
                    f"{block_count} blocks of {block_size} bytes",
                    file=sys.stderr,
                )
                for index in range(block_count):
                    data = payload[index * block_size:(index + 1) * block_size]
                    data = data.ljust(block_size, b"\x1a")
                    self.send_block(
                        index + 1, data,
                        XMODEM_INTER_BLOCK_DELAY
                        if block_size == XMODEM_BLOCK_SIZE_1K else 0.0,
                    )

                completion_start = len(self.received)
                for _ in range(XMODEM_RETRIES):
                    self.write(bytes((XMODEM_EOT,)))
                    response = self.wait_for_control({XMODEM_ACK, XMODEM_NAK},
                                                     3.0)
                    if response == XMODEM_ACK:
                        break
                else:
                    raise XmodemError(
                        "U-Boot did not acknowledge XMODEM end-of-transfer"
                    )

                self.wait_for_text(b"=>", 10.0, completion_start)
                return
            except XmodemError:
                if transfer_active:
                    try:
                        self.cancel_transfer()
                    except XmodemError as error:
                        print(f"[serial] could not cancel loadx: {error}",
                              file=sys.stderr)

                if block_size_index + 1 == len(block_sizes):
                    raise

                print("[xmodem] 1 KiB packets exhausted their retries; "
                      "falling back to 128-byte packets", file=sys.stderr)

    def transfer_chunked(self, source: pathlib.Path, address: int,
                         chunk_size: int = XMODEM_CHUNK_SIZE) -> None:
        """Load a large image through bounded U-Boot XMODEM sessions.

        Keeping each session short makes a transient USB-TTL re-enumeration
        recoverable at a U-Boot prompt and avoids long uninterrupted UART
        bursts on adapters with small receive FIFOs.  Non-final chunks are
        block aligned so XMODEM padding cannot create gaps in RAM.
        """

        payload = source.read_bytes()
        if chunk_size <= 0 or chunk_size % XMODEM_BLOCK_SIZE_1K != 0:
            raise XmodemError("chunk size must be a positive 1 KiB multiple")

        offset = 0
        total = (len(payload) + chunk_size - 1) // chunk_size
        while offset < len(payload):
            end = min(offset + chunk_size, len(payload))
            chunk = payload[offset:end]
            temporary = pathlib.Path(
                f"/tmp/k1-xmodem-{os.getpid()}-{offset:x}.bin"
            )
            temporary.write_bytes(chunk)
            try:
                print(
                    f"[xmodem] chunk {offset // chunk_size + 1}/{total}: "
                    f"{len(chunk)} bytes -> 0x{address + offset:x}",
                    file=sys.stderr,
                )
                self.transfer(temporary, address + offset)
            finally:
                try:
                    temporary.unlink()
                except FileNotFoundError:
                    pass

            offset = end


def main() -> int:
    args = parse_args()
    if not args.file.is_file():
        raise XmodemError(f"file is not readable: {args.file}")

    xmodem = K1Xmodem(args.device, args.baud, args.log)
    try:
        xmodem.transfer(args.file, args.address)
    finally:
        xmodem.close()

    print(f"PASS: loaded {args.file} to 0x{args.address:x}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except XmodemError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
