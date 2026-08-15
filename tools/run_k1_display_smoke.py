#!/usr/bin/env python3
"""Stage, RAM-boot, and validate the K1 inherited framebuffer profile.

Only two named files are copied to the mounted bootfs.  The tool does not use
``saveenv`` or write raw eMMC sectors.  It verifies framebuffer registration,
geometry, mapping, update ioctls, and completion of the NuttX ``fb`` example.
Visual scanout still requires a connected display and human observation.
"""

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
CONTEST_ROOT = SCRIPT_DIR.parent
WORKSPACE_ROOT = CONTEST_ROOT.parent
PACKAGE_DIR = WORKSPACE_ROOT / "out/k1-display-fb"
DEFAULT_PAYLOAD = PACKAGE_DIR / "contest-nuttx-flat.bin"
DEFAULT_WRAPPER = PACKAGE_DIR / "k1-go-wrapper.bin"
BOOTFS_DIR = "/boot/musepi"
UBOOT_DIR = "/musepi"
PAYLOAD_NAME = "contest-nuttx-display-fb-flat.bin"
WRAPPER_NAME = "k1-go-wrapper-display-fb.bin"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="RAM-boot and validate the K1 inherited framebuffer"
    )
    parser.add_argument("--device", default="/dev/ttyUSB0", help="USB-TTL device")
    parser.add_argument("--baud", type=int, choices=BAUD_RATES, default=115200)
    parser.add_argument("--adb", default="adb", help="ADB executable")
    parser.add_argument("--payload", type=pathlib.Path, default=DEFAULT_PAYLOAD)
    parser.add_argument("--wrapper", type=pathlib.Path, default=DEFAULT_WRAPPER)
    parser.add_argument(
        "--log-dir",
        type=pathlib.Path,
        default=WORKSPACE_ROOT / "out/k1-serial",
    )
    parser.add_argument(
        "--boot-timeout",
        type=float,
        default=60,
        help="seconds to wait for U-Boot and NSH (default: 60)",
    )
    return parser.parse_args()


def boot_and_test(
    console: SerialConsole,
    payload_size: int,
    wrapper_size: int,
    timeout: float,
) -> None:
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
    if b"K1 display: inherited /dev/fb0 registered" not in started:
        raise SmokeFailure("NuttX started without framebuffer registration")

    listing = console.command("ls -l /dev/fb0", b"nsh>", 10, b"\n")
    if b"/dev/fb0" not in listing or b"No such" in listing:
        raise SmokeFailure("/dev/fb0 was not present after registration")

    result = console.command("fb", b"nsh>", 30, b"\n")
    if b"FB test finished" not in result:
        raise SmokeFailure("framebuffer test did not complete")
    required = (b"xres: 800", b"yres: 480", b"bpp: 32", b"Mapped FB: 0x7f700000")
    missing = [marker.decode("ascii") for marker in required if marker not in result]
    if missing:
        raise SmokeFailure("framebuffer test missed: " + ", ".join(missing))
    if b"ERROR:" in result:
        raise SmokeFailure("framebuffer test reported an error")


def main() -> int:
    args = parse_args()
    payload = args.payload.resolve()
    wrapper = args.wrapper.resolve()
    payload_size = require_file(payload, "payload")
    wrapper_size = require_file(wrapper, "wrapper")
    args.log_dir.mkdir(parents=True, exist_ok=True)
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    log_path = args.log_dir / f"k1-display-fb-{timestamp}.log"
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
        boot_and_test(console, payload_size, wrapper_size, args.boot_timeout)
        print("PASS: K1 framebuffer node and 800x480/32bpp update path validated")
        print("NOTE: visual colour rectangles still require a connected display.")
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
