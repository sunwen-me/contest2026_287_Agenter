#!/usr/bin/env python3
"""Stage and RAM-boot the read-only K1 USB register probe."""

from __future__ import annotations

import argparse
import datetime as dt
import pathlib
import subprocess
import sys

from run_k1_ethernet_smoke import BAUD_RATES
from run_k1_ethernet_smoke import SerialConsole
from run_k1_ethernet_smoke import SmokeFailure
from run_k1_ethernet_smoke import require_loaded_size
from run_k1_usb_host_smoke import adb_run
from run_k1_usb_host_smoke import adb_state
from run_k1_usb_host_smoke import reboot_uboot_to_linux
from run_k1_usb_host_smoke import require_file
from run_k1_usb_host_smoke import stage_file
from run_k1_usb_host_smoke import wait_for_adb
from run_k1_usb_host_smoke import wait_for_uboot


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
WORKSPACE_ROOT = SCRIPT_DIR.parent.parent
PACKAGE_DIR = WORKSPACE_ROOT / "out/k1-usb-probe"
DEFAULT_PAYLOAD = PACKAGE_DIR / "contest-nuttx-flat.bin"
DEFAULT_WRAPPER = PACKAGE_DIR / "k1-go-wrapper.bin"
BOOTFS_DIR = "/boot/musepi"
UBOOT_DIR = "/musepi"
PAYLOAD_NAME = "contest-nuttx-usb-probe-flat.bin"
WRAPPER_NAME = "k1-go-wrapper-usb-probe.bin"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="RAM-boot and record the K1 DWC3/PHY read-only probe"
    )
    parser.add_argument("--device", default="/dev/ttyUSB0", help="USB-TTL device")
    parser.add_argument("--baud", type=int, choices=BAUD_RATES, default=115200)
    parser.add_argument("--adb", default="adb", help="ADB executable")
    parser.add_argument("--payload", type=pathlib.Path, default=DEFAULT_PAYLOAD)
    parser.add_argument("--wrapper", type=pathlib.Path, default=DEFAULT_WRAPPER)
    parser.add_argument(
        "--log-dir", type=pathlib.Path, default=WORKSPACE_ROOT / "out/k1-serial"
    )
    parser.add_argument("--boot-timeout", type=float, default=45)
    return parser.parse_args()


def boot_probe(
    console: SerialConsole,
    payload_size: int,
    wrapper_size: int,
    timeout: float,
) -> bytes:
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
    required = (
        b"K1: entry",
        b"K1 USB: DWC3 GSNPSID=0x",
        b"K1 USB: GCTL=0x",
        b"K1 USB: GSTS=0x",
        b"K1 USB: GHWPARAMS0=0x",
        b"K1 USB: GHWPARAMS1=0x",
        b"K1 USB: GHWPARAMS2=0x",
        b"K1 USB: GHWPARAMS3=0x",
        b"K1 USB: USB2PHYCFG=0x",
        b"K1 USB: USB3PIPECTL=0x",
        b"K1 USB: DCFG=0x",
        b"K1 USB: DSTS=0x",
        b"K1 USB: USB2 PHY REG0=0x",
        b"K1 USB: combo PHY REG0=0x",
        b"K1 USB: xHCI caplen=0x",
    )
    missing = [marker.decode("ascii") for marker in required if marker not in started]
    if missing:
        raise SmokeFailure("USB probe missed: " + ", ".join(missing))
    if b"unexpected DWC3 signature" in started or b"USB probe failed" in started:
        raise SmokeFailure("USB probe reported an invalid signature or failure")

    return started


def main() -> int:
    args = parse_args()
    payload = args.payload.resolve()
    wrapper = args.wrapper.resolve()
    payload_size = require_file(payload, "payload")
    wrapper_size = require_file(wrapper, "wrapper")
    args.log_dir.mkdir(parents=True, exist_ok=True)
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    log_path = args.log_dir / f"k1-usb-probe-{timestamp}.log"
    console = SerialConsole(args.device, args.baud, log_path)

    try:
        if adb_state(args.adb) != "device":
            try:
                wait_for_adb(args.adb, 10)
            except SmokeFailure:
                reboot_uboot_to_linux(console, args.adb)

        adb_run(args.adb, ["shell", "test", "-d", BOOTFS_DIR])
        stage_file(args.adb, wrapper, f"{BOOTFS_DIR}/{WRAPPER_NAME}")
        stage_file(args.adb, payload, f"{BOOTFS_DIR}/{PAYLOAD_NAME}")
        print("[adb] reboot", file=sys.stderr)
        adb_run(args.adb, ["reboot"])
        wait_for_uboot(console, args.boot_timeout)
        boot_probe(console, payload_size, wrapper_size, args.boot_timeout)
        print("PASS: K1 DWC3/PHY/xHCI read-only register probe completed")
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
