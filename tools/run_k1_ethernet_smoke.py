#!/usr/bin/env python3
"""RAM-boot a K1 NuttX payload and run the Ethernet smoke test.

The tool deliberately does not write U-Boot environment or eMMC sectors.  It
only uses ``ext4load`` to copy already-staged files into RAM and ``go`` to run
the small handoff wrapper.  A raw serial transcript is retained for each run.
"""

from __future__ import annotations

import argparse
import datetime as dt
import ipaddress
import json
import os
import pathlib
import select
import socket
import subprocess
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

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
CONTEST_ROOT = SCRIPT_DIR.parent
WORKSPACE_ROOT = CONTEST_ROOT.parent
DEFAULT_PAYLOAD = (
    WORKSPACE_ROOT / "out/k1-ethernet-udp/contest-nuttx-flat.bin"
)
DEFAULT_WRAPPER = (
    WORKSPACE_ROOT / "out/k1-ethernet-udp/k1-go-wrapper.bin"
)
UDP_ECHO_PACKET_COUNT = 4
UDP_ECHO_MAX_PAYLOAD = 1472
SERIAL_BYTE_DELAY = 0.002


class SmokeFailure(RuntimeError):
    """A serial operation did not reach its required state."""


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


class SerialConsole:
    def __init__(self, device: str, baud: int, log_path: pathlib.Path) -> None:
        self.fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        configure_serial(self.fd, baud)
        termios.tcflush(self.fd, termios.TCIFLUSH)
        self.log = log_path.open("wb", buffering=0)
        self.transcript = bytearray()

    def close(self) -> None:
        self.log.close()
        os.close(self.fd)

    def mark(self) -> int:
        return len(self.transcript)

    def write(self, data: bytes) -> None:
        """Write every byte to the USB-TTL adapter without losing U-Boot input."""

        offset = 0
        while offset < len(data):
            try:
                written = os.write(self.fd, data[offset:offset + 1])
            except BlockingIOError:
                written = 0

            if written > 0:
                offset += written
                if offset < len(data):
                    time.sleep(SERIAL_BYTE_DELAY)
                continue

            _, writable, _ = select.select([], [self.fd], [], 1.0)
            if not writable:
                raise SmokeFailure("timed out writing to serial device")

    def read(self, timeout: float) -> None:
        readable, _, _ = select.select([self.fd], [], [], timeout)
        if not readable:
            return

        while True:
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                break
            if not data:
                break
            self.log.write(data)
            self.transcript.extend(data)
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()

    def wait_for(self, marker: bytes, mark: int, timeout: float) -> bytes:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if marker in self.transcript[mark:]:
                return bytes(self.transcript[mark:])
            self.read(min(0.2, deadline - time.monotonic()))
        output = bytes(self.transcript[mark:]).decode("utf-8", "replace")
        raise SmokeFailure(
            f"timed out waiting for {marker!r}; serial output since step:\n{output}"
        )

    def command(
        self, command: str, marker: bytes, timeout: float, terminator: bytes = b"\r"
    ) -> bytes:
        print(f"\n[serial] {command}", file=sys.stderr)
        mark = self.mark()
        self.write(command.encode("ascii") + terminator)
        return self.wait_for(marker, mark, timeout)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="RAM-boot K1 NuttX and test Ethernet without persistent writes"
    )
    parser.add_argument("--device", default="/dev/ttyUSB0", help="USB-TTL device")
    parser.add_argument("--baud", type=int, choices=BAUD_RATES, default=115200)
    parser.add_argument("--adb", default="adb", help="ADB executable")
    parser.add_argument("--host-interface", required=True, help="host USB Ethernet interface")
    parser.add_argument("--host-address", default="192.168.50.1")
    parser.add_argument("--board-address", default="192.168.50.2")
    parser.add_argument(
        "--udp-echo",
        action="store_true",
        help="require byte-identical UDPv4 echo after the ICMP checks",
    )
    parser.add_argument(
        "--udp-port", type=int, default=33333, help="board UDP echo port"
    )
    parser.add_argument(
        "--resume-nsh",
        action="store_true",
        help="run the ping checks on an already started and configured NSH",
    )
    parser.add_argument("--payload", type=pathlib.Path, default=DEFAULT_PAYLOAD)
    parser.add_argument("--wrapper", type=pathlib.Path, default=DEFAULT_WRAPPER)
    parser.add_argument(
        "--remote-payload",
        default="/musepi/contest-nuttx-ethernet-udp-flat.bin",
    )
    parser.add_argument(
        "--remote-wrapper", default="/musepi/k1-go-wrapper-ethernet-udp.bin"
    )
    parser.add_argument(
        "--log-dir",
        type=pathlib.Path,
        default=WORKSPACE_ROOT / "out/k1-serial",
    )
    return parser.parse_args()


def require_file(path: pathlib.Path, label: str) -> int:
    if not path.is_file():
        raise SmokeFailure(f"{label} is not a file: {path}")
    return path.stat().st_size


def require_host_address(interface: str, address: str) -> None:
    """Require the static IPv4 endpoint before taking the board out of Linux."""
    result = subprocess.run(
        ["ip", "-j", "address", "show", "dev", interface],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        text=True,
        timeout=10,
    )
    if result.returncode != 0:
        raise SmokeFailure(
            f"cannot inspect host interface {interface}: {result.stdout.strip()}"
        )

    try:
        interfaces = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise SmokeFailure(
            f"invalid ip address output for {interface}: {error}"
        ) from error

    target = ipaddress.IPv4Address(address)
    for entry in interfaces:
        for info in entry.get("addr_info", []):
            if info.get("family") == "inet" and info.get("local") == str(target):
                return

    raise SmokeFailure(
        f"host interface {interface} does not have required IPv4 address "
        f"{address}; restore the direct-link static configuration before retrying"
    )


def halt_at_uboot(console: SerialConsole, adb: str) -> None:
    """Reboot through ADB and stop only the main U-Boot autoboot loop."""
    # A previous test might already have stopped at U-Boot.  Probe before
    # rebooting so the tool is safe to resume after a host-side interruption.
    mark = console.mark()
    console.write(b"\r")
    try:
        console.wait_for(b"=>", mark, 1.5)
        print("[serial] existing U-Boot prompt acquired", file=sys.stderr)
        return
    except SmokeFailure:
        pass

    state = subprocess.run(
        [adb, "get-state"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        text=True,
        timeout=10,
    )
    if state.returncode != 0 or state.stdout.strip() != "device":
        raise SmokeFailure(
            "board is neither at a visible U-Boot prompt nor available via ADB: "
            + state.stdout.strip()
        )

    subprocess.run([adb, "reboot"], check=True, timeout=10)
    print("[serial] waiting for U-Boot prompt", file=sys.stderr)

    mark = console.mark()
    deadline = time.monotonic() + 35
    abort_next = time.monotonic()

    while time.monotonic() < deadline:
        output = console.transcript[mark:]
        now = time.monotonic()

        # K1 U-Boot has a zero-second boot delay.  Keep abort characters in
        # flight across SPL and main U-Boot; clear the resulting command line
        # after the prompt, before submitting any command.
        if now >= abort_next:
            console.write(b"s" * 32)
            abort_next = now + 0.03

        if b"=>" in output:
            print("[serial] U-Boot prompt acquired", file=sys.stderr)
            return
        console.read(min(0.03, deadline - now))

    output = bytes(console.transcript[mark:]).decode("utf-8", "replace")
    raise SmokeFailure(f"did not acquire a U-Boot prompt:\n{output}")


def require_loaded_size(output: bytes, expected: int, label: str) -> None:
    marker = f"{expected} bytes read".encode("ascii")
    if marker not in output:
        text = output.decode("utf-8", "replace")
        raise SmokeFailure(f"{label} did not report {marker!r}:\n{text}")


def run_host_ping(interface: str, board_address: str) -> None:
    print(f"\n[host] ping -I {interface} -c 3 -W 2 {board_address}", file=sys.stderr)
    result = subprocess.run(
        ["ping", "-I", interface, "-c", "3", "-W", "2", board_address],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        text=True,
        timeout=15,
    )
    print(result.stdout, end="")
    if result.returncode != 0 or "3 received" not in result.stdout:
        raise SmokeFailure("host-to-board ping was not a complete 3/3 success")


def wait_for_link(console: SerialConsole, seconds: float = 3.0) -> None:
    """Allow the polling PHY worker to publish carrier before counting ping."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        console.read(min(0.2, deadline - time.monotonic()))


def run_udp_echo(
    console: SerialConsole,
    host_address: str,
    board_address: str,
    port: int,
) -> None:
    payloads = (
        b"k1-udp-echo-00",
        b"k1-udp-echo-01\x00binary",
        bytes(range(128)),
        bytes(range(256)) * (UDP_ECHO_MAX_PAYLOAD // 256)
        + bytes(range(UDP_ECHO_MAX_PAYLOAD % 256)),
    )
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3.0)

    try:
        sock.bind((host_address, 0))
        source_port = sock.getsockname()[1]
        print(
            f"\n[serial] k1_udpecho "
            f"(UDP/{port}, {UDP_ECHO_PACKET_COUNT} datagrams)",
            file=sys.stderr,
        )
        mark = console.mark()
        console.write(b"k1_udpecho\n")
        console.wait_for(b"k1_udpecho: listening", mark, 5)
        completion_mark = console.mark()

        for index, payload in enumerate(payloads, start=1):
            print(
                f"[host] UDP {index}/{UDP_ECHO_PACKET_COUNT}: {len(payload)} bytes",
                file=sys.stderr,
            )
            sent = sock.sendto(payload, (board_address, port))
            if sent != len(payload):
                raise SmokeFailure(
                    f"UDP send {index}/{UDP_ECHO_PACKET_COUNT} was short: "
                    f"{sent}/{len(payload)}"
                )

            echoed, source = sock.recvfrom(UDP_ECHO_MAX_PAYLOAD)
            expected_source = (board_address, port)
            if source != expected_source:
                raise SmokeFailure(
                    f"UDP echo {index}/{UDP_ECHO_PACKET_COUNT} came from "
                    f"{source}, "
                    f"expected {expected_source}"
                )
            if echoed != payload:
                raise SmokeFailure(
                    f"UDP echo {index}/{UDP_ECHO_PACKET_COUNT} payload mismatch: "
                    f"sent {len(payload)} bytes, received {len(echoed)}"
                )

        output = console.wait_for(b"nsh>", completion_mark, 10)
        if b"k1_udpecho: PASS 4/4 datagrams echoed" not in output:
            text = output.decode("utf-8", "replace")
            raise SmokeFailure(f"board UDP echo did not report PASS:\n{text}")
        print(
            f"[host] UDP echo source port {source_port} verified", file=sys.stderr
        )
    except OSError as error:
        raise SmokeFailure(f"host UDP validation failed: {error}") from error
    finally:
        sock.close()


def main() -> int:
    args = parse_args()
    payload_size = require_file(args.payload.resolve(), "payload")
    wrapper_size = require_file(args.wrapper.resolve(), "wrapper")
    args.log_dir.mkdir(parents=True, exist_ok=True)
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    log_path = args.log_dir / f"k1-ethernet-smoke-{timestamp}.log"
    console = SerialConsole(args.device, args.baud, log_path)

    try:
        require_host_address(args.host_interface, args.host_address)
        if args.resume_nsh:
            mark = console.mark()
            console.write(b"\n")
            console.wait_for(b"nsh>", mark, 5)
        else:
            halt_at_uboot(console, args.adb)
            # Clear possible abort characters before the first real command.
            settle_mark = console.mark()
            console.write(b"\x15\r")
            console.wait_for(b"=>", settle_mark, 5)
            console.command("wdt dev PMIC_WDT", b"=>", 20)
            pmic_stop = console.command("wdt stop", b"=>", 20)
            if b"No device set" in pmic_stop:
                raise SmokeFailure("failed to stop PMIC watchdog")
            console.command("wdt dev watchdog@D4080000", b"=>", 20)
            console.command("wdt stop", b"=>", 20)

            wrapper = console.command(
                f"ext4load mmc 2:5 0x12000000 {args.remote_wrapper}", b"=>", 15
            )
            require_loaded_size(wrapper, wrapper_size, "wrapper")
            payload = console.command(
                f"ext4load mmc 2:5 0x11000000 {args.remote_payload}", b"=>", 20
            )
            require_loaded_size(payload, payload_size, "payload")

            start_mark = console.mark()
            print("\n[serial] go 0x12000000", file=sys.stderr)
            console.write(b"go 0x12000000\r")
            console.wait_for(b"nsh>", start_mark, 35)
            started = bytes(console.transcript[start_mark:])
            if b"K1: entry" not in started:
                raise SmokeFailure("NuttX shell appeared without the K1 entry marker")

            console.command("ifup eth0", b"nsh>", 15, b"\n")
            console.command(
                f"ifconfig eth0 {args.board_address} netmask 255.255.255.0",
                b"nsh>",
                10,
                b"\n",
            )

        wait_for_link(console)
        board_ping = console.command(
            f"ping -c 3 {args.host_address}", b"nsh>", 20, b"\n"
        )
        if b"3 packets transmitted, 3 received" not in board_ping:
            text = board_ping.decode("utf-8", "replace")
            raise SmokeFailure(f"board-to-host ping did not complete successfully:\n{text}")
        run_host_ping(args.host_interface, args.board_address)
        if args.udp_echo:
            run_udp_echo(
                console,
                args.host_address,
                args.board_address,
                args.udp_port,
            )

        print(f"\nPASS: Ethernet smoke test complete. Serial log: {log_path}")
        return 0
    finally:
        console.close()
        print(f"Serial log: {log_path}", file=sys.stderr)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (SmokeFailure, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
