#!/usr/bin/env python3
"""Stage and RAM-boot the K1 USB Host profile without persistent boot changes.

The tool only writes two explicitly named test files into the mounted bootfs.
U-Boot receives ``ext4load`` and ``go`` commands; it never receives ``saveenv``
or a raw block-write command.  The resulting serial log is the first-board
evidence for the USB2817 Hub/xHCI path.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import pathlib
import subprocess
import sys
import time

from run_k1_ethernet_smoke import BAUD_RATES
from run_k1_ethernet_smoke import SerialConsole
from run_k1_ethernet_smoke import SmokeFailure
from run_k1_ethernet_smoke import require_loaded_size


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
CONTEST_ROOT = SCRIPT_DIR.parent
WORKSPACE_ROOT = CONTEST_ROOT.parent
PACKAGE_DIR = WORKSPACE_ROOT / "out/k1-usb-host"
DEFAULT_PAYLOAD = PACKAGE_DIR / "contest-nuttx-flat.bin"
DEFAULT_WRAPPER = PACKAGE_DIR / "k1-go-wrapper.bin"
BOOTFS_DIR = "/boot/musepi"
UBOOT_DIR = "/musepi"
PAYLOAD_NAME = "contest-nuttx-usb-host-flat.bin"
WRAPPER_NAME = "k1-go-wrapper-usb-host.bin"
UAS_INITIALIZATION_SETTLE_SECONDS = 8.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="RAM-boot and record K1 USB2817/xHCI bring-up"
    )
    parser.add_argument("--device", default="/dev/ttyUSB0", help="USB-TTL device")
    parser.add_argument("--baud", type=int, choices=BAUD_RATES, default=115200)
    parser.add_argument("--adb", default="adb", help="ADB executable")
    parser.add_argument("--payload", type=pathlib.Path, default=DEFAULT_PAYLOAD)
    parser.add_argument("--wrapper", type=pathlib.Path, default=DEFAULT_WRAPPER)
    parser.add_argument(
        "--resume-uboot",
        action="store_true",
        help="reuse verified staged files from an existing U-Boot prompt",
    )
    parser.add_argument(
        "--log-dir",
        type=pathlib.Path,
        default=WORKSPACE_ROOT / "out/k1-serial",
    )
    parser.add_argument(
        "--boot-timeout",
        type=float,
        default=45,
        help="seconds to wait for U-Boot and NSH (default: 45)",
    )
    parser.add_argument(
        "--require-msc",
        action="store_true",
        help="require a mass-storage device to register /dev/sda in NSH",
    )
    parser.add_argument(
        "--require-uas",
        action="store_true",
        help="require UAS alt 1, stream 1, and initial SCSI commands to pass",
    )
    return parser.parse_args()


def require_file(path: pathlib.Path, label: str) -> int:
    if not path.is_file():
        raise SmokeFailure(f"{label} is not a file: {path}")
    return path.stat().st_size


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def adb_state(adb: str) -> str:
    result = subprocess.run(
        [adb, "get-state"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        text=True,
        timeout=10,
    )
    return result.stdout.strip() if result.returncode == 0 else ""


def wait_for_adb(adb: str, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if adb_state(adb) == "device":
            return
        time.sleep(1)

    raise SmokeFailure("ADB did not return after reset; boot the stock Linux image first")


def reboot_uboot_to_linux(console: SerialConsole, adb: str) -> None:
    """Use an existing NuttX or U-Boot prompt to recover Linux for staging."""

    mark = console.mark()
    console.write(b"\r\n")
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        output = console.transcript[mark:]
        if b"=>" in output:
            print("[serial] reset to stock Linux for ADB staging", file=sys.stderr)
            console.write(b"reset\r")
            wait_for_adb(adb, 70)
            return
        if b"nsh>" in output:
            print("[serial] reboot NuttX to stock Linux for ADB staging", file=sys.stderr)
            console.write(b"reboot\n")
            wait_for_adb(adb, 70)
            return
        console.read(min(0.2, deadline - time.monotonic()))

    raise SmokeFailure(
        "board is unavailable through ADB and has no visible NuttX/U-Boot prompt; "
        "press RST once and rerun this command"
    )


def adb_run(adb: str, args: list[str]) -> str:
    result = subprocess.run(
        [adb, *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        text=True,
        timeout=45,
    )
    print(result.stdout, end="")
    if result.returncode != 0:
        raise SmokeFailure(f"ADB command failed: {' '.join(args)}")
    return result.stdout


def stage_file(adb: str, source: pathlib.Path, remote: str) -> None:
    expected = sha256(source)
    print(f"[adb] push {source.name} -> {remote}", file=sys.stderr)
    adb_run(adb, ["push", str(source), remote])
    output = adb_run(adb, ["shell", "sha256sum", remote])
    actual = output.split(maxsplit=1)[0] if output.split() else ""
    if actual != expected:
        raise SmokeFailure(
            f"hash mismatch for {remote}: expected {expected}, got {actual or 'none'}"
        )


def wait_for_uboot(console: SerialConsole, timeout: float) -> None:
    """Stop only main U-Boot autoboot after an ADB reboot."""

    print("[serial] waiting for and stopping U-Boot autoboot", file=sys.stderr)
    mark = console.mark()
    deadline = time.monotonic() + timeout
    abort_next = time.monotonic()

    while time.monotonic() < deadline:
        now = time.monotonic()
        if b"=>" in console.transcript[mark:]:
            return
        if now >= abort_next:
            console.write(b"s" * 32)
            abort_next = now + 0.03
        console.read(min(0.03, deadline - now))

    output = bytes(console.transcript[mark:]).decode("utf-8", "replace")
    raise SmokeFailure(f"did not acquire U-Boot prompt:\n{output}")


def boot_nuttx(
    console: SerialConsole,
    payload_size: int,
    wrapper_size: int,
    timeout: float,
) -> bytes:
    """Run the established volatile U-Boot handoff sequence."""

    settle_mark = console.mark()
    console.write(b"\x15\r")
    console.wait_for(b"=>", settle_mark, 5)

    console.command("wdt dev PMIC_WDT", b"=>", 20)
    pmic_stop = console.command("wdt stop", b"=>", 20)
    if b"No device set" in pmic_stop:
        raise SmokeFailure("failed to select the PMIC watchdog")
    console.command("wdt dev watchdog@D4080000", b"=>", 20)
    console.command("wdt stop", b"=>", 20)

    wrapper = console.command(
        f"ext4load mmc 2:5 0x12000000 {UBOOT_DIR}/{WRAPPER_NAME}", b"=>", 15
    )
    require_loaded_size(wrapper, wrapper_size, "wrapper")
    payload = console.command(
        f"ext4load mmc 2:5 0x11000000 {UBOOT_DIR}/{PAYLOAD_NAME}", b"=>", 20
    )
    require_loaded_size(payload, payload_size, "payload")

    print("[serial] go 0x12000000", file=sys.stderr)
    start_mark = console.mark()
    console.write(b"go 0x12000000\r")
    started = console.wait_for(b"nsh>", start_mark, timeout)
    if b"K1: entry" not in started:
        raise SmokeFailure("NuttX shell appeared without the K1 entry marker")
    if b"K1 USB host: xHCI started" not in started:
        raise SmokeFailure("NuttX started but did not report xHCI initialization")

    # USB2817 needs time to train both its USB2 and USB3 links after GPIO79
    # enables VBUS.  The K1's first xHCI interrupt can arrive near the end of
    # that interval, so retain a complete worker pass after it instead of
    # closing the serial log immediately after the interrupt handler marker.
    irq_deadline = time.monotonic() + 8.0
    while time.monotonic() < irq_deadline:
        console.read(min(0.2, irq_deadline - time.monotonic()))
        if b"K1 USB: first xHCI IRQ" in console.transcript[start_mark:]:
            break

    if b"K1 USB: first xHCI IRQ" in console.transcript[start_mark:]:
        worker_deadline = time.monotonic() + 3.0
        while time.monotonic() < worker_deadline:
            console.read(min(0.2, worker_deadline - time.monotonic()))

    started = bytes(console.transcript[start_mark:])
    if b"K1 USB: waiter root port=" not in started:
        raise SmokeFailure(
            "NuttX reached NSH but no root-port connection was reported; "
            "inspect the retained serial log for final PORTSC state"
        )

    if (b"K1 USB: enumerate port=0x0000000000000002" not in started or
            started.count(
                b"K1 USB: enumerate ret=0x0000000000000000") < 2):
        raise SmokeFailure(
            "xHCI started but USB2817 USB2 and USB3 Hub functions did not "
            "both enumerate successfully; inspect the retained serial log"
        )

    return started


def require_msc(console: SerialConsole) -> None:
    """Require the NuttX MSC driver to expose its first block device."""

    output = console.command("ls /dev", b"nsh>", 10, terminator=b"\n")
    devices = {
        line.strip()
        for line in output.decode("utf-8", "replace").splitlines()
    }
    if "sda" not in devices:
        raise SmokeFailure(
            "USB Host reached NSH but no /dev/sda was registered; "
            "inspect the retained log for MSC BOT/UAS initialization errors"
        )


def require_uas(console: SerialConsole) -> None:
    """Require UAS stream setup, transfer, and initial SCSI device creation."""

    markers = (
        b"K1 USB: xHCI stream context enabled",
        b"K1 USB: xHCI UAS async stream transfer",
    )
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline:
        transcript = bytes(console.transcript)
        if all(marker in transcript for marker in markers):
            # The first status transfer confirms stream execution, but the
            # storage class still needs to finish initial SCSI discovery.
            settle_deadline = time.monotonic() + UAS_INITIALIZATION_SETTLE_SECONDS
            while time.monotonic() < settle_deadline:
                console.read(min(0.2, settle_deadline - time.monotonic()))

            require_msc(console)
            return
        console.read(min(0.2, deadline - time.monotonic()))

    missing = [
        marker.decode("ascii")
        for marker in markers
        if marker not in console.transcript
    ]
    raise SmokeFailure(
        "UAS did not reach the expected xHCI stream path: "
        + ", ".join(missing)
    )


def main() -> int:
    args = parse_args()
    payload = args.payload.resolve()
    wrapper = args.wrapper.resolve()
    payload_size = require_file(payload, "payload")
    wrapper_size = require_file(wrapper, "wrapper")
    args.log_dir.mkdir(parents=True, exist_ok=True)
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    log_path = args.log_dir / f"k1-usb-host-{timestamp}.log"
    console = SerialConsole(args.device, args.baud, log_path)

    try:
        if args.resume_uboot:
            mark = console.mark()
            console.write(b"\r")
            console.wait_for(b"=>", mark, 5)
        else:
            if adb_state(args.adb) != "device":
                try:
                    wait_for_adb(args.adb, 30)
                except SmokeFailure:
                    reboot_uboot_to_linux(console, args.adb)

            adb_run(args.adb, ["shell", "test", "-d", BOOTFS_DIR])
            stage_file(args.adb, wrapper, f"{BOOTFS_DIR}/{WRAPPER_NAME}")
            stage_file(args.adb, payload, f"{BOOTFS_DIR}/{PAYLOAD_NAME}")

            print("[adb] reboot", file=sys.stderr)
            adb_run(args.adb, ["reboot"])
            wait_for_uboot(console, args.boot_timeout)

        started = boot_nuttx(console, payload_size, wrapper_size, args.boot_timeout)

        print("PASS: K1 USB Host profile reached NSH and enumerated USB2817 "
              "USB2/USB3 Hub functions")
        if args.require_uas:
            require_uas(console)
            print("PASS: UAS alt 1, xHCI stream 1, and /dev/sda registration")
        if args.require_msc:
            require_msc(console)
            print("PASS: K1 USB mass-storage device registered /dev/sda")
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
