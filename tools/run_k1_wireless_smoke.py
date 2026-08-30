#!/usr/bin/env python3
"""RAM-boot a K1 wireless image and retain its SDIO/H5 diagnostics.

The board files are sent through XMODEM into volatile RAM.  The U-Boot
sequence deliberately omits saveenv and every eMMC, FDL, or fastboot write.
"""

from __future__ import annotations

import argparse
import datetime as dt
import gzip
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

from load_k1_xmodem import BAUD_RATES, K1Xmodem, XmodemError


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
CONTEST_ROOT = SCRIPT_DIR.parent
WORKSPACE_ROOT = CONTEST_ROOT.parent
DEFAULT_PACKAGE = WORKSPACE_ROOT / "out/k1-wireless-r10"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="RAM-boot K1 wireless diagnostics without persistent writes"
    )
    parser.add_argument(
        "--device", default="auto",
        help="USB-TTL device, or auto to select one stable serial-by-id link",
    )
    parser.add_argument("--baud", type=int, choices=BAUD_RATES, default=115200)
    parser.add_argument("--adb", default="adb", help="ADB executable")
    parser.add_argument(
        "--payload", type=pathlib.Path,
        default=DEFAULT_PACKAGE / "contest-nuttx-flat.bin",
    )
    parser.add_argument(
        "--wrapper", type=pathlib.Path,
        default=DEFAULT_PACKAGE / "k1-go-wrapper.bin",
    )
    parser.add_argument(
        "--gzip-payload", action="store_true",
        help=("compress the payload on the host, load it at 0x13000000, and "
              "use U-Boot unzip into the volatile 0x11000000 payload area"),
    )
    parser.add_argument(
        "--log-dir", type=pathlib.Path,
        default=WORKSPACE_ROOT / "out/k1-serial",
    )
    parser.add_argument(
        "--boot-timeout", type=float, default=120,
        help="seconds to wait for U-Boot and NSH (default: 120)",
    )
    reset_mode = parser.add_mutually_exclusive_group()
    reset_mode.add_argument(
        "--manual-reset",
        action="store_true",
        help="wait for one physical RST instead of rebooting stock Linux through ADB",
    )
    reset_mode.add_argument(
        "--nsh-reboot",
        action="store_true",
        help="reboot the currently running K1 NuttX image through NSH",
    )
    reset_mode.add_argument(
        "--uboot-ready",
        action="store_true",
        help="start RAM loading from an existing U-Boot prompt",
    )
    voice_prompt = parser.add_mutually_exclusive_group()
    voice_prompt.add_argument(
        "--voice-prompt",
        dest="voice_prompt",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    voice_prompt.add_argument(
        "--no-voice-prompt",
        dest="voice_prompt",
        action="store_false",
        help="do not speak before a manual RST (the terminal prompt remains)",
    )
    parser.set_defaults(voice_prompt=True)
    parser.add_argument(
        "--require-h5", action="store_true",
        help="fail unless the Bluetooth H5 local-version exchange succeeds",
    )
    parser.add_argument(
        "--require-bt-hci-reset", action="store_true",
        help="fail unless the Bluetooth H5 HCI Reset command completes",
    )
    parser.add_argument(
        "--require-bt-hci-device", action="store_true",
        help="fail unless the K1 H5 transport registers /dev/ttyHCI0",
    )
    parser.add_argument(
        "--verify-bt-hci-open", action="store_true",
        help="open /dev/ttyHCI0 and require its H5 link setup to remain open",
    )
    parser.add_argument(
        "--require-bt-host-scan", action="store_true",
        help=("require the NuttX Bluetooth Host netdev and a successful "
              "btsak scan start/get/stop cycle"),
    )
    parser.add_argument(
        "--bt-ifname", default="bnep0",
        help="Bluetooth Host network interface used by --require-bt-host-scan",
    )
    parser.add_argument(
        "--bt-scan-seconds", type=float, default=5.0,
        help="scan dwell time for --require-bt-host-scan (default: 5)",
    )
    parser.add_argument(
        "--require-wifi-function", action="store_true",
        help="fail unless Wi-Fi Function 1 enables and completes a CMD53 read",
    )
    parser.add_argument(
        "--require-cmd53-write", action="store_true",
        help="fail unless the 12-byte Function 1 CMD53 write diagnostic succeeds",
    )
    parser.add_argument(
        "--require-first-vendor-cmd53", action="store_true",
        help="fail unless the first vendor-shaped CMD53 write/read sequence succeeds",
    )
    parser.add_argument(
        "--require-dle-scc", action="store_true",
        help="fail unless the RTL8852BS2 SDIO/SCC DLE initialization completes",
    )
    parser.add_argument(
        "--require-hci-flow-control", action="store_true",
        help="fail unless the RTL8852BS2 SDIO HCI flow-control initialization completes",
    )
    parser.add_argument(
        "--require-firmware-preboot", action="store_true",
        help="fail unless the RTL8852BS2 WLAN CPU H2C preboot completes and cleans up",
    )
    parser.add_argument(
        "--require-firmware-layout", action="store_true",
        help="fail unless the RTL8852BS2 U2 NICCE image layout validates",
    )
    parser.add_argument(
        "--require-firmware-mss-efuse", action="store_true",
        help="fail unless the RTL8852BS2 MSS eFuse selector is read",
    )
    parser.add_argument(
        "--require-firmware-mss-legacy-signature", action="store_true",
        help="fail unless RTL8852BS2 legacy MSS signature selection validates",
    )
    parser.add_argument(
        "--require-firmware-full-download", action="store_true",
        help="fail unless RTL8852BS2 full firmware download reaches WCPU ready",
    )
    parser.add_argument(
        "--require-firmware-runtime", action="store_true",
        help="fail unless the eFuse MAC and running RTL8852BS2 WCPU state validate",
    )
    parser.add_argument(
        "--require-runtime-transport", action="store_true",
        help=("fail unless RTL8852BS2 post-firmware SDIO transport registers "
              "and the TX-page CMD53 window validate"),
    )
    parser.add_argument(
        "--require-runtime-h2c-loopback", action="store_true",
        help=("fail unless the RTL8852BS2 runtime H2C command and C2H RX "
              "FIFO loopback completes"),
    )
    parser.add_argument(
        "--require-runtime-data-tx-descriptor", action="store_true",
        help=("fail unless the RTL8852BS2 normal-data TX descriptor and "
              "TXPG_WP preflight complete without submitting a frame"),
    )
    parser.add_argument(
        "--require-runtime-mac-core", action="store_true",
        help=("fail unless the RTL8852BS2 static runtime MAC-core fields "
              "are initialized and read back"),
    )
    parser.add_argument(
        "--require-runtime-bb-rf", action="store_true",
        help=("fail unless the RTL8852BS2 BB/RF release sequence completes "
              "with its register and XTAL read-backs"),
    )
    parser.add_argument(
        "--require-runtime-phy-cr", action="store_true",
        help=("fail unless the complete RTL8852BS2 static BB PHY CR image "
              "loads and its spread read-back sentinels validate"),
    )
    parser.add_argument(
        "--require-runtime-bb-reset", action="store_true",
        help=("fail unless the RTL8852BS2 BB reset pulse that follows the "
              "PHY CR image is acknowledged by the firmware"),
    )
    parser.add_argument(
        "--require-scan-phy-counters", action="store_true",
        help=("fail unless the RMAC receive counters are sampled before and "
              "after the passive scan-offload dwells"),
    )
    parser.add_argument(
        "--require-scan-rf-readback", action="store_true",
        help=("fail unless the radio mode, channel and RC calibration "
              "registers are read back before and after the passive scan"),
    )
    parser.add_argument(
        "--require-rf-context", action="store_true",
        help=("fail unless the RTL8852BS2 board RF context report completes "
              "before the firmware download"),
    )
    parser.add_argument(
        "--require-runtime-rf-cr", action="store_true",
        help=("fail unless the RTL8852BS2 RF radio A/B parameter image is "
              "accepted for this board RFE/CV and every offload batch is "
              "acknowledged"),
    )
    parser.add_argument(
        "--require-runtime-control-plane", action="store_true",
        help=("fail unless the RTL8852BS2 firmware role-control H2C payloads "
              "serialize in RAM"),
    )
    parser.add_argument(
        "--require-runtime-address-cam", action="store_true",
        help=("fail unless the RTL8852BS2 address/BSSID CAM H2C payload "
              "serializes in RAM"),
    )
    parser.add_argument(
        "--require-runtime-role-cam-done-ack", action="store_true",
        help=("fail unless the RTL8852BS2 no-link role and address/BSSID CAM "
              "H2Cs receive successful firmware done acknowledgements"),
    )
    parser.add_argument(
        "--require-runtime-scanofld-channel-done-ack", action="store_true",
        help=("fail unless the RTL8852BS2 passive 2.4 GHz 1-13 scan-offload "
              "table receives a successful firmware done acknowledgement"),
    )
    parser.add_argument(
        "--require-runtime-scanofld-passive", action="store_true",
        help=("fail unless the RTL8852BS2 one-shot passive 2.4 GHz 1-13 "
              "scan-offload receives its done acknowledgement and C2H events"),
    )
    parser.add_argument(
        "--require-runtime-scanofld-rx", action="store_true",
        help=("fail unless the passive scan RX diagnostic observes at least "
              "one valid Beacon/Probe Response BSSID"),
    )
    parser.add_argument(
        "--require-runtime-scanofld-active", action="store_true",
        help=("fail unless a wildcard Probe Request is accepted into the "
              "firmware packet-offload table, the scan-offload channel table "
              "is resubmitted naming it, and at least one Probe Response is "
              "received"),
    )
    parser.add_argument(
        "--require-runtime-auth", action="store_true",
        help=("fail unless an open-system Authentication Request is "
              "transmitted to an access point chosen by a sweep and an "
              "Authentication frame addressed to the eFuse self MAC is "
              "received back"),
    )
    parser.add_argument(
        "--require-runtime-join", action="store_true",
        help=("fail unless the firmware acknowledges the JOININFO and the "
              "infrastructure address CAM update for the access point a sweep "
              "chose, and the authentication exchange and the Beacon receive "
              "still work afterwards"),
    )
    parser.add_argument(
        "--require-runtime-assoc-response", action="store_true",
        help=("fail unless an Association Request is transmitted to an access "
              "point that advertises no Privacy and an Association Response "
              "addressed to the eFuse self MAC is received back, whatever "
              "status code it carries"),
    )
    parser.add_argument(
        "--require-runtime-assoc", action="store_true",
        help=("fail unless that Association Response grants a non-zero "
              "association identifier, the firmware acknowledges the JOININFO "
              "and address CAM update carrying it, and the confirming sweep "
              "still receives the target's Beacons"),
    )
    parser.add_argument(
        "--require-wlan0-scan", action="store_true",
        help=("fail unless wlan0 registers and a wapi passive scan returns at "
              "least one Beacon/Probe-Response BSS through SIOCGIWSCAN"),
    )
    parser.add_argument(
        "--wlan0-ifname", default="wlan0",
        help="wireless interface name used by --require-wlan0-scan",
    )
    parser.add_argument(
        "--wlan0-scan-timeout", type=float, default=60.0,
        help=("console timeout for the synchronous wapi passive scan "
              "(default: 60)"),
    )
    parser.add_argument(
        "--require-runtime-rx-worker", action="store_true",
        help=("fail unless the RTL8852BS2 LPWORK RX FIFO worker consumes "
              "and classifies its queued C2H loopback response"),
    )
    parser.add_argument(
        "--require-h2c-tx-resource", action="store_true",
        help="fail unless the RTL8852BS2 H2C TX resource/descriptor diagnostic completes",
    )
    parser.add_argument(
        "--require-firmware-header-packet", action="store_true",
        help="fail unless the RTL8852BS2 one-packet firmware-header diagnostic completes",
    )
    parser.add_argument(
        "--require-firmware-section-packet", action="store_true",
        help="fail unless the RTL8852BS2 one-packet firmware-section diagnostic completes",
    )
    parser.add_argument(
        "--require-firmware-section0-tail-packet", action="store_true",
        help="fail unless the RTL8852BS2 28-byte post-first-packet probe completes",
    )
    parser.add_argument(
        "--require-firmware-section0-second-packet", action="store_true",
        help="fail unless the RTL8852BS2 second section-zero packet completes",
    )
    parser.add_argument(
        "--require-firmware-section0-third-packet", action="store_true",
        help="fail unless the RTL8852BS2 third section-zero packet completes",
    )
    parser.add_argument(
        "--require-firmware-section0-fourth-packet", action="store_true",
        help="fail unless the RTL8852BS2 fourth section-zero packet completes",
    )
    parser.add_argument(
        "--require-sdio-pre-init", action="store_true",
        help="fail unless the RTL8852BS2 SDIO pre-initialization completes",
    )
    parser.add_argument(
        "--require-bringup-success", action="store_true",
        help="fail if the K1 RTL8852BS2 bring-up reports an error",
    )
    parser.add_argument(
        "--require-cmd53-hisr-read", action="store_true",
        help="fail unless the vendor-shaped Function 1 HISR CMD53 read succeeds",
    )
    return parser.parse_args()


def require_file(path: pathlib.Path, label: str) -> pathlib.Path:
    resolved = path.resolve()
    if not resolved.is_file():
        raise XmodemError(f"{label} is not a readable file: {resolved}")
    return resolved


def make_gzip_payload(payload: pathlib.Path, directory: pathlib.Path) \
        -> pathlib.Path:
    """Create a deterministic temporary gzip stream for the RAM loader."""

    output = tempfile.NamedTemporaryFile(
        prefix="k1-wireless-", suffix=".gz", dir=directory, delete=False,
    )
    compressed = pathlib.Path(output.name)

    try:
        with output:
            with gzip.GzipFile(filename="", mode="wb", fileobj=output,
                               mtime=0) as stream:
                with payload.open("rb") as source:
                    shutil.copyfileobj(source, stream)
    except Exception:
        compressed.unlink(missing_ok=True)
        raise

    print(f"[host] gzip {payload.stat().st_size} -> {compressed.stat().st_size} "
          "bytes", file=sys.stderr)
    return compressed


def announce_reset() -> None:
    """Prompt for one physical reset without delaying the serial listener."""

    speaker = shutil.which("spd-say")
    if speaker is None:
        print("[voice] spd-say is unavailable; use the terminal reset prompt",
              file=sys.stderr)
        return

    try:
        subprocess.Popen(
            [speaker, "--language=zh", "请按一下复位按钮。"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except OSError as error:
        print(f"[voice] could not start reset prompt: {error}", file=sys.stderr)


def resolve_serial_device(device: str) -> str:
    """Prefer a stable USB serial symlink over a volatile ttyUSB number."""

    if device != "auto":
        return device

    stable_dir = pathlib.Path("/dev/serial/by-id")
    candidates = sorted(path for path in stable_dir.glob("*") if path.is_symlink())
    if len(candidates) == 1:
        return str(candidates[0])

    fallback = sorted(pathlib.Path("/dev").glob("ttyUSB*"))
    fallback += sorted(pathlib.Path("/dev").glob("ttyACM*"))
    if len(fallback) == 1:
        return str(fallback[0])

    raise XmodemError(
        "cannot auto-select USB-TTL; pass --device with a /dev/serial/by-id "
        "path or explicit tty device"
    )


def adb_state(adb: str) -> str:
    result = subprocess.run(
        [adb, "get-state"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        check=False, text=True, timeout=10,
    )
    return result.stdout.strip() if result.returncode == 0 else ""


def wait_for_adb(adb: str, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if adb_state(adb) == "device":
            return
        time.sleep(0.5)
    raise XmodemError("ADB did not find the stock Linux image")


def command(serial: K1Xmodem, text: str, timeout: float = 20.0) -> bytes:
    start = len(serial.received)
    serial.command(text)
    serial.wait_for_text(b"=>", timeout, start)
    return bytes(serial.received[start:])


def halt_at_uboot_from_serial(
    serial: K1Xmodem, timeout: float, allow_running_os: bool = False,
) -> None:
    """Stop K1 at U-Boot without writing into a booted operating system."""

    start = len(serial.received)
    deadline = time.monotonic() + timeout
    saw_uboot_banner = False
    saw_spl_banner = False
    abort_deadline = 0.0
    abort_send_at = 0.0
    abort_sent = False
    next_wait_notice = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        output = serial.received[start:]
        if b"=>" in output:
            # Remove any leading interruption bytes before issuing commands.
            serial.write_text(b"\x15\r")
            serial.wait_for_text(b"=>", 5.0)
            return

        # A late invocation must never continue pushing abort characters into
        # Linux's login prompt or an already-running NuttX shell.
        if b" login:" in output or b"nsh>" in output:
            if not allow_running_os:
                raise XmodemError(
                    "board already passed U-Boot; start this tool before "
                    "pressing RST"
                )

            # In manual-reset mode the board normally already runs Linux or
            # NuttX when this listener opens.  Discard that old console tail
            # and continue waiting for the next SPL/U-Boot banner instead of
            # requiring the operator to time the reset before the tool starts.
            start = len(serial.received)
            saw_uboot_banner = False
            saw_spl_banner = False
            abort_deadline = 0.0
            abort_send_at = 0.0
            abort_sent = False
            continue

        now = time.monotonic()
        if now >= next_wait_notice:
            print("[serial] still waiting for a physical RST", file=sys.stderr)
            next_wait_notice = now + 10.0

        if not saw_spl_banner and b"U-Boot SPL 2022" in output:
            saw_spl_banner = True
            print("[serial] K1 SPL banner detected; waiting for main U-Boot",
                  file=sys.stderr)

        if not saw_uboot_banner and b"U-Boot 2022" in output:
            saw_uboot_banner = True
            if abort_deadline == 0.0:
                # The K1 accepts console input only after the main U-Boot
                # initialization has configured its serial port.  This board
                # reaches the zero-second autoboot point about 2.4 seconds
                # after its banner; one byte at 2.2 seconds is reliable.
                abort_send_at = now + 2.2
                abort_deadline = now + 5.0
            print("[serial] K1 U-Boot banner detected; waiting for stop window",
                  file=sys.stderr)

        # A USB-TTL adapter can re-enumerate between the U-Boot banner and
        # the console setup.  In that case the reopened file descriptor sees
        # the console marker but not the banner.  That marker is already in
        # U-Boot and precedes the board's zero-second autoboot, so start the
        # same bounded stop burst immediately.
        if abort_deadline == 0.0 and b"In:    serial" in output:
            saw_uboot_banner = True
            # The console marker and "Autoboot in 0 seconds" can arrive in
            # one read after USB re-enumeration.  Submit the first burst in
            # this iteration; waiting for the next poll can be too late.
            serial.write_text(b"s")
            abort_sent = True
            abort_deadline = now + 5.0
            print("[serial] K1 U-Boot console detected after re-enumeration",
                  file=sys.stderr)

        # Once U-Boot starts executing its boot command, console input can no
        # longer stop it.  Process SPL and console markers first because one
        # USB-TTL read may contain those markers and this text together.
        if (b"Try to boot" in output or b"Starting kernel" in output) and \
                abort_deadline == 0.0:
            raise XmodemError("K1 U-Boot stop window expired")

        if abort_deadline != 0.0:
            if now >= abort_deadline:
                raise XmodemError("K1 U-Boot stop window expired")
            if not abort_sent and now >= abort_send_at:
                serial.write_text(b"s")
                abort_sent = True
                print("[serial] K1 U-Boot stop byte sent", file=sys.stderr)

        serial.read(min(0.03, deadline - now))

    output = bytes(serial.received[start:]).decode("utf-8", "replace")
    raise XmodemError(f"did not acquire U-Boot prompt:\n{output}")


def halt_at_uboot(serial: K1Xmodem, adb: str, timeout: float) -> None:
    """Reboot via ADB and interrupt K1's zero-second U-Boot autoboot."""

    wait_for_adb(adb, timeout)
    print("[adb] reboot", file=sys.stderr)
    subprocess.run([adb, "reboot"], check=True, timeout=10)
    print("[serial] waiting for and stopping U-Boot autoboot", file=sys.stderr)
    halt_at_uboot_from_serial(serial, timeout)


def halt_at_uboot_from_nsh(serial: K1Xmodem, timeout: float) -> None:
    """Recover Wi-Fi power state through NuttX and a second U-Boot reset."""

    start = len(serial.received)
    serial.write_text(b"\n")
    serial.wait_for_text(b"nsh>", 5.0, start)
    print("[nsh] reboot", file=sys.stderr)
    serial.write_text(b"reboot\n")
    print("[serial] waiting for and stopping U-Boot autoboot", file=sys.stderr)
    halt_at_uboot_from_serial(serial, timeout)

    # A direct NuttX reboot can leave the RTL8852BS2 SDIO function unable to
    # answer the first CMD5.  A full U-Boot reset restores the known-good
    # pre-boot hardware state without making any persistent U-Boot change.

    print("[uboot] reset for clean Wi-Fi power state", file=sys.stderr)
    serial.write_text(b"reset\n")
    print("[serial] waiting for and stopping U-Boot after reset", file=sys.stderr)
    halt_at_uboot_from_serial(serial, timeout)


def discard_stale_serial(serial: K1Xmodem, duration: float = 0.8) -> None:
    """Drain USB-TTL bytes from the OS that was running before a manual RST."""

    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        serial.read(max(0.0, min(0.1, deadline - time.monotonic())))


def wait_for_nsh_or_reset(serial: K1Xmodem, start: int,
                          timeout: float) -> int:
    """Wait for the NSH prompt, but stop as soon as the SoC resets.

    A reset while the payload runs replays the U-Boot SPL banner on the same
    serial line.  Waiting out the whole boot timeout for a prompt that can no
    longer appear only hides that event, so report it the moment the banner
    comes back.
    """

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        marker_at = serial.received.find(b"nsh>", start)
        if marker_at >= 0:
            return marker_at + len(b"nsh>")

        if serial.received.find(b"U-Boot SPL 2022", start) >= 0:
            raise XmodemError(
                "the SoC reset while the payload was running: the U-Boot SPL "
                "banner reappeared after go, so this run measures nothing.  "
                "Check the board supply and the serial cable, then retry."
            )

        serial.read(max(0.0, min(0.2, deadline - time.monotonic())))

    output = bytes(serial.received[start:]).decode("utf-8", "replace")
    raise XmodemError("timed out waiting for the NSH prompt: " + output)


def boot_wireless(
    serial: K1Xmodem,
    wrapper: pathlib.Path,
    payload: pathlib.Path,
    compressed_payload: pathlib.Path | None,
    timeout: float,
) -> bytes:
    """Use watchdog stop, RAM loads, and go only; no persistent U-Boot action."""

    for device in ("PMIC_WDT", "watchdog@D4080000"):
        selected = command(serial, f"wdt dev {device}")
        stopped = command(serial, "wdt stop")
        if b"Can't get the watchdog timer" in selected or \
                b"No device set" in stopped or \
                b"Stopping watchdog timer" in stopped:
            raise XmodemError(f"failed to stop {device}")

    serial.transfer(wrapper, 0x12000000)
    if compressed_payload is None:
        serial.transfer_chunked(payload, 0x11000000)
    else:
        serial.transfer_chunked(compressed_payload, 0x13000000)
        decompressed = command(
            serial,
            f"unzip 0x13000000 0x11000000 0x{payload.stat().st_size:x}",
            30.0,
        )
        expected = f"Uncompressed size: {payload.stat().st_size}".encode("ascii")
        if expected not in decompressed:
            text = decompressed.decode("utf-8", "replace")
            raise XmodemError("U-Boot did not confirm payload decompression:\n" +
                              text)

    start = len(serial.received)
    serial.command("go 0x12000000")
    return bytes(serial.received[start:wait_for_nsh_or_reset(serial, start,
                                                             timeout)])


def verify_bt_hci_open(serial: K1Xmodem, timeout: float = 4.0) -> None:
    """Open the HCI pseudo device and require the diagnostic H5 active log."""

    start = len(serial.received)
    deadline = time.monotonic() + timeout
    serial.write_text(b"cat /dev/ttyHCI0\n")

    while time.monotonic() < deadline:
        output = bytes(serial.received[start:])
        if b"nsh>" in output:
            raise XmodemError(
                "opening /dev/ttyHCI0 returned before H5 link setup completed"
            )

        if b"h5: active" in output:
            print("PASS: K1 /dev/ttyHCI0 reached active H5 state",
                  file=sys.stderr)
            return

        serial.read(max(0.0, min(0.2, deadline - time.monotonic())))

    output = bytes(serial.received[start:])
    if b"K1 Bluetooth: H5 transport opened" not in output:
        raise XmodemError("/dev/ttyHCI0 did not open the K1 H5 transport")

    raise XmodemError("/dev/ttyHCI0 did not reach active H5 state")


def nsh_command(serial: K1Xmodem, text: str, timeout: float = 10.0) -> bytes:
    """Run one NSH command and return the complete command transcript."""

    start = len(serial.received)
    serial.write_text(text.encode("ascii") + b"\n")
    serial.wait_for_text(b"nsh>", timeout, start)
    return bytes(serial.received[start:])


def nsh_command_retry_shell_error(serial: K1Xmodem, text: str,
                                  timeout: float = 10.0) -> bytes:
    """Retry a known NSH command if startup output corrupts its input."""

    output = b""
    for attempt in range(3):
        output = nsh_command(serial, text, timeout)
        if b"nsh: " not in output:
            return output

        if attempt < 2:
            time.sleep(0.2)

    return output


def wait_for_serial(serial: K1Xmodem, duration: float) -> None:
    """Keep draining the console while the controller collects LE reports."""

    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        serial.read(max(0.0, min(0.2, deadline - time.monotonic())))


def wait_for_bt_host_registration(serial: K1Xmodem,
                                  timeout: float = 15.0) -> bytes:
    """Wait for asynchronous K1 H5 Host registration after the NSH banner."""

    start = len(serial.received)
    deadline = time.monotonic() + timeout
    ready = b"K1 Bluetooth: H5 host stack registered"
    failures = (
        b"K1 Bluetooth: H5 Host registration error=",
        b"ERROR:  bt_initialize() failed:",
        b"ERROR: HCI driver open failed",
        b"err: bluetooth driver open timeout",
    )

    while time.monotonic() < deadline:
        output = bytes(serial.received[start:])
        if ready in output:
            return output

        if any(marker in output for marker in failures):
            text = output.decode("utf-8", "replace")
            raise XmodemError(
                "K1 Bluetooth Host registration failed after NSH:\n" + text
            )

        serial.read(max(0.0, min(0.2, deadline - time.monotonic())))

    output = bytes(serial.received[start:]).decode("utf-8", "replace")
    raise XmodemError(
        "timed out waiting for K1 Bluetooth Host registration after NSH:\n" +
        output
    )


def wait_for_bt_raw_registration(serial: K1Xmodem,
                                 timeout: float = 15.0) -> bytes:
    """Wait for asynchronous raw HCI registration after the NSH banner."""

    start = len(serial.received)
    deadline = time.monotonic() + timeout
    ready = b"K1 Bluetooth: H5 raw HCI registered /dev/ttyHCI0"
    failures = (
        b"K1 Bluetooth: H5 local-version error=",
        b"K1 Bluetooth: H5 stack registration error=",
        b"K1 EXCEPTION",
    )

    while time.monotonic() < deadline:
        output = bytes(serial.received[start:])
        if ready in output:
            return output

        if any(marker in output for marker in failures):
            text = output.decode("utf-8", "replace")
            raise XmodemError(
                "K1 Bluetooth raw HCI registration failed after NSH:\n" + text
            )

        serial.read(max(0.0, min(0.2, deadline - time.monotonic())))

    output = bytes(serial.received[start:]).decode("utf-8", "replace")
    raise XmodemError(
        "timed out waiting for K1 Bluetooth raw HCI registration after NSH:\n" +
        output
    )


def wait_for_runtime_rx_worker(serial: K1Xmodem,
                               timeout: float = 10.0) -> bytes:
    """Wait for the asynchronous LPWORK RX loopback classification."""

    start = len(serial.received)
    deadline = time.monotonic() + timeout
    complete = re.compile(
        rb"K1 Wi-Fi: runtime RX transfer=(?:0x)?0*30 "
        rb"frames=(?:0x)?0*1 data=(?:0x)?0* C2H=(?:0x)?0*1 "
        rb"dispatched=(?:0x)?0*1"
    )
    while time.monotonic() < deadline:
        output = bytes(serial.received[start:])
        if complete.search(output) is not None:
            print("PASS: K1 runtime RX worker dispatched C2H loopback",
                  file=sys.stderr)
            return output
        if b"K1 Wi-Fi: runtime RX read error=" in output or \
                b"K1 Wi-Fi: runtime RX parse error=" in output or \
                b"K1 Wi-Fi: runtime C2H dispatch error=" in output:
            break
        serial.read(max(0.0, min(0.2, deadline - time.monotonic())))

    output = bytes(serial.received[start:]).decode("utf-8", "replace")
    raise XmodemError(f"runtime RX worker did not dispatch C2H loopback:\n{output}")


def require_no_nsh_error(output: bytes, action: str) -> None:
    """Convert NSH/btsak errors into a single actionable smoke failure."""

    if b"ERROR:" in output or b"nsh: " in output:
        text = output.decode("utf-8", "replace")
        raise XmodemError(f"Bluetooth Host {action} failed:\n{text}")


def verify_bt_host_scan(serial: K1Xmodem, ifname: str,
                        scan_seconds: float) -> None:
    """Exercise the NuttX Host registration and non-destructive LE scan API."""

    if not ifname:
        raise XmodemError("Bluetooth Host interface name must not be empty")
    if scan_seconds <= 0:
        raise XmodemError("Bluetooth Host scan duration must be positive")

    ifconfig = nsh_command_retry_shell_error(serial, "ifconfig")
    if re.search(rb"^" + re.escape(ifname.encode("ascii")) + rb"(?:\s|$)",
                 ifconfig, re.MULTILINE) is None:
        text = ifconfig.decode("utf-8", "replace")
        raise XmodemError(
            f"Bluetooth Host netdev {ifname} was not registered:\n{text}"
        )

    started = nsh_command_retry_shell_error(serial, f"bt {ifname} scan start")
    require_no_nsh_error(started, "scan start")
    wait_for_serial(serial, scan_seconds)

    results = nsh_command_retry_shell_error(serial, f"bt {ifname} scan get")
    require_no_nsh_error(results, "scan get")
    if b"Scan result:" not in results:
        text = results.decode("utf-8", "replace")
        raise XmodemError(f"Bluetooth Host scan did not return results:\n{text}")

    stopped = nsh_command_retry_shell_error(serial, f"bt {ifname} scan stop")
    require_no_nsh_error(stopped, "scan stop")
    print(f"PASS: K1 Bluetooth Host scan completed on {ifname}",
          file=sys.stderr)


def verify_wlan0_scan(serial: K1Xmodem, ifname: str, timeout: float) -> None:
    """Exercise the wlan0 scan netdev through the wapi wireless ioctls."""

    if not ifname:
        raise XmodemError("wlan0 interface name must not be empty")

    ifconfig = nsh_command_retry_shell_error(serial, "ifconfig")
    if re.search(rb"^" + re.escape(ifname.encode("ascii")) + rb"(?:\s|$)",
                 ifconfig, re.MULTILINE) is None:
        text = ifconfig.decode("utf-8", "replace")
        raise XmodemError(
            f"wireless netdev {ifname} was not registered:\n{text}"
        )

    # SIOCSIWSCAN runs the whole 2.4 GHz 1-13 dwell budget before it returns,
    # so this one command owns the console for several seconds.  wapi then
    # reads the report back with SIOCGIWSCAN and prints one row per BSS.
    results = nsh_command_retry_shell_error(serial, f"wapi pscan {ifname}",
                                           timeout)
    if b"ERROR:" in results or b"nsh: " in results:
        text = results.decode("utf-8", "replace")
        raise XmodemError(f"wapi passive scan on {ifname} failed:\n{text}")

    # The driver names itself in this summary, so the marker stays "wlan0"
    # even when the interface was registered under another name.
    sweep = re.search(
        rb"wlan0 sweep ret=0 end=1 bss=([0-9]+) data-only=([0-9]+) "
        rb"dropped=([0-9]+)",
        results,
    )
    if sweep is None:
        text = results.decode("utf-8", "replace")
        raise XmodemError(
            "wlan0 SIOCSIWSCAN did not finish a firmware sweep with its "
            f"scan-end event:\n{text}"
        )

    if int(sweep.group(1)) == 0:
        text = results.decode("utf-8", "replace")
        raise XmodemError(
            "wlan0 sweep found no Beacon/Probe-Response BSS "
            f"(data-only={sweep.group(2).decode('ascii')} "
            f"dropped={sweep.group(3).decode('ascii')}):\n{text}"
        )

    if b"bssid / frequency / signal level / encode / ssid" not in results:
        text = results.decode("utf-8", "replace")
        raise XmodemError(
            f"wapi did not read the {ifname} scan report back:\n{text}"
        )

    # One row per reported BSS: BSSID, frequency, signal level, encode flags and
    # SSID.  The driver fills iw_freq in its channel form (e=0, m=channel),
    # which nuttx/include/nuttx/wireless/wireless.h documents as "0-1000 =
    # channel, > 1000 = frequency in Hz" and which the in-tree bcm43xxx driver
    # uses too.  wapi does not print that raw value: wapi_scan_event() in
    # apps/wireless/wapi/src/wireless.c maps e==0 with m in 1-13 to
    # 2407 + 5 * m MHz before "%g" prints it, so a 2.4 GHz row reaches us as
    # 2412-2472.  Accept either form and fold both back to a channel number.
    # This stays strict: wapi converts only channels it recognises, so an
    # unresolved channel (m=0) leaves the column at 0 and is rejected here.
    if b"*float*" in results:
        text = results.decode("utf-8", "replace")
        raise XmodemError(
            "wapi printed the scan report frequency column as \"*float*\", so "
            "the image lacks CONFIG_LIBC_FLOATINGPOINT and the channel each "
            f"BSS was heard on cannot be read back:\n{text}"
        )

    rows = re.findall(
        rb"^([0-9a-f]{2}(?::[0-9a-f]{2}){5})\t([^\t]*)\t", results,
        re.MULTILINE,
    )
    accepted = []
    for bssid, column in rows:
        if bssid == b"00:00:00:00:00:00":
            continue

        try:
            value = float(column)
        except ValueError:
            continue

        if not value.is_integer():
            continue

        value = int(value)
        if 1 <= value <= 13:
            channel = value
        elif 2412 <= value <= 2472 and (value - 2407) % 5 == 0:
            channel = (value - 2407) // 5
        else:
            continue

        accepted.append((bssid, channel))

    if not accepted:
        text = results.decode("utf-8", "replace")
        raise XmodemError(
            "wapi scan results carried no 2.4 GHz BSS row with a valid BSSID "
            "and a channel in 1-13 (either as a raw channel or as the "
            f"2412-2472 MHz wapi converts it to):\n{text}"
        )

    if len(accepted) != int(sweep.group(1)):
        text = results.decode("utf-8", "replace")
        raise XmodemError(
            f"wlan0 reported {sweep.group(1).decode('ascii')} BSS but wapi "
            f"decoded {len(accepted)} valid rows:\n{text}"
        )

    detail = ", ".join(
        f"{bssid.decode('ascii')} ch{channel}" for bssid, channel in accepted
    )
    print(f"PASS: K1 {ifname} passive scan reported {len(accepted)} BSS "
          f"(data-only={sweep.group(2).decode('ascii')} "
          f"dropped={sweep.group(3).decode('ascii')}): {detail}", file=sys.stderr)


def main() -> int:
    args = parse_args()
    if args.bt_scan_seconds <= 0:
        raise XmodemError("--bt-scan-seconds must be positive")
    if args.wlan0_scan_timeout <= 0:
        raise XmodemError("--wlan0-scan-timeout must be positive")
    wrapper = require_file(args.wrapper, "wrapper")
    payload = require_file(args.payload, "payload")
    args.log_dir.mkdir(parents=True, exist_ok=True)
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    log_path = args.log_dir / f"{payload.parent.name}-{timestamp}.log"
    compressed_payload = None
    device = resolve_serial_device(args.device)
    print(f"[serial] using {device}", file=sys.stderr)
    serial = K1Xmodem(
        device,
        args.baud,
        log_path,
        reconnect_on_reenumeration=True,
    )

    try:
        if args.gzip_payload:
            compressed_payload = make_gzip_payload(payload, args.log_dir)
        if args.manual_reset:
            # A USB-TTL adapter can retain the final U-Boot/Linux bytes from
            # a previous session.  Discard them before prompting so only the
            # next physical reset is eligible for the U-Boot stop window.
            discard_stale_serial(serial)
            print("[serial] press RST; waiting for and stopping U-Boot autoboot",
                  file=sys.stderr)
            # The serial listener is already open above.  Speak before each
            # operator action so a physical reset is never requested silently.
            if args.voice_prompt:
                announce_reset()
            halt_at_uboot_from_serial(serial, args.boot_timeout,
                                      allow_running_os=True)
        elif args.nsh_reboot:
            halt_at_uboot_from_nsh(serial, args.boot_timeout)
        elif args.uboot_ready:
            # Repeated CAN also leaves a stale U-Boot loadx session without
            # touching storage; at an ordinary prompt it is harmless input.
            serial.write(b"\x18" * 8 + b"\r")
            serial.wait_for_text(b"=>", 5.0)
        else:
            halt_at_uboot(serial, args.adb, args.boot_timeout)
        started = boot_wireless(serial, wrapper, payload, compressed_payload,
                                args.boot_timeout)
        required = [b"K1: entry", b"K1 Wi-Fi:"]
        bt_hci_device_markers = (
            b"K1 Bluetooth: H5 stack registered /dev/ttyHCI0",
            b"K1 Bluetooth: H5 raw HCI registered /dev/ttyHCI0",
        )
        if args.require_h5:
            required.append(b"K1 Bluetooth: H5 local version")
        if args.require_bt_hci_reset:
            required.append(b"K1 Bluetooth: H5 HCI reset complete")
        if args.require_wlan0_scan:
            required.append(b"K1 Wi-Fi: wlan0 scan device registered")
        if args.require_bt_host_scan:
            started += wait_for_bt_host_registration(serial)
            required.append(b"K1 Bluetooth: H5 host stack registered")
        elif (args.require_h5 or args.require_bt_hci_device) and \
                b"K1 Bluetooth: H5 raw HCI registered /dev/ttyHCI0" not in started:
            started += wait_for_bt_raw_registration(serial)
        missing = [marker.decode("ascii") for marker in required if marker not in started]
        if args.require_bt_hci_device and \
                not any(marker in started for marker in bt_hci_device_markers):
            missing.append("K1 Bluetooth H5 /dev/ttyHCI0 registration")
        first_vendor_write = re.search(
            rb"K1 Wi-Fi GPL: first vendor CMD53 error=0x0+\r?\n", started
        )
        first_vendor_value = re.search(
            rb"K1 Wi-Fi GPL: first vendor CMD53 value=(?:0x)?[0-9a-fA-F]+\r?\n",
            started,
        )
        if args.require_wifi_function:
            function_ready = re.search(
                rb"K1 Wi-Fi: F1 IOEN=(?:0x)?0*2 IORDY=(?:0x)?0*2", started
            )
            function_read = re.search(
                rb"K1 Wi-Fi: F1 local=(?:0x)?[0-9a-fA-F]+", started
            )
            if function_read is None:
                function_read = re.search(
                    rb"K1 Wi-Fi: F1 HISR CMD53=(?:0x)?[0-9a-fA-F]+\r?\n",
                    started,
                )

            if function_read is None and first_vendor_write is not None and \
                    first_vendor_value is not None:
                function_read = first_vendor_value

            if function_ready is None or function_read is None:
                missing.append("Wi-Fi Function 1 CMD53 transfer")
        if args.require_cmd53_write:
            write_result = re.search(
                rb"K1 Wi-Fi GPL: CMD53 write diagnostic error=0x0+\r?\n",
                started,
            )
            if write_result is None:
                missing.append("12-byte Function 1 CMD53 write")
        if args.require_first_vendor_cmd53:
            if first_vendor_write is None or first_vendor_value is None:
                missing.append("first vendor-shaped CMD53 write/read sequence")
        if args.require_dle_scc:
            dle_scc_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 DLE SCC init complete\r?\n",
                started,
            )
            if dle_scc_result is None:
                missing.append("RTL8852BS2 SDIO/SCC DLE initialization")
        if args.require_hci_flow_control:
            hci_fc_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 HCI flow-control init complete\r?\n",
                started,
            )
            if hci_fc_result is None:
                missing.append("RTL8852BS2 SDIO HCI flow-control initialization")
        if args.require_firmware_preboot:
            fwdl_preboot_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 FW preboot H2C path complete\r?\n",
                started,
            )
            if fwdl_preboot_result is None:
                missing.append("RTL8852BS2 firmware-download H2C preboot")
        if args.require_firmware_layout:
            fwdl_layout_result = re.search(
                rb"K1 Wi-Fi GPL: U2 NICCE layout complete\r?\n", started
            )
            if fwdl_layout_result is None:
                missing.append("RTL8852BS2 U2 NICCE firmware image layout")
        if args.require_firmware_mss_efuse:
            fwdl_mss_efuse_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 MSS eFuse diagnostic complete\r?\n",
                started,
            )
            if fwdl_mss_efuse_result is None:
                missing.append("RTL8852BS2 firmware MSS eFuse selector")
        if args.require_firmware_mss_legacy_signature:
            fwdl_mss_signature_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 MSS legacy signature "
                rb"diagnostic complete\r?\n",
                started,
            )
            if fwdl_mss_signature_result is None:
                missing.append("RTL8852BS2 legacy MSS signature selection")
        if args.require_firmware_full_download:
            fwdl_full_download_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 full FWDL ready status=0x0*7\r?\n",
                started,
            )
            if fwdl_full_download_result is None:
                missing.append("RTL8852BS2 full firmware download/WCPU ready")
        if args.require_firmware_runtime:
            fwdl_runtime_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 firmware runtime diagnostic "
                rb"complete\r?\n",
                started,
            )
            if fwdl_runtime_result is None:
                missing.append("RTL8852BS2 eFuse MAC/firmware runtime state")
        if args.require_runtime_transport:
            runtime_transport_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 runtime transport diagnostic "
                rb"complete\r?\n",
                started,
            )
            if runtime_transport_result is None:
                missing.append("RTL8852BS2 post-firmware SDIO transport state")
        if args.require_runtime_h2c_loopback:
            runtime_loopback_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 runtime H2C/C2H loopback "
                rb"complete\r?\n",
                started,
            )
            if runtime_loopback_result is None:
                missing.append("RTL8852BS2 runtime H2C/C2H SDIO loopback")
        if args.require_runtime_data_tx_descriptor:
            runtime_data_tx_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 runtime data TX descriptor "
                rb"complete\r?\n",
                started,
            )
            if runtime_data_tx_result is None:
                missing.append("RTL8852BS2 runtime data TX descriptor preflight")
        if args.require_runtime_mac_core:
            runtime_mac_core_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 runtime MAC core init "
                rb"complete\r?\n",
                started,
            )
            if runtime_mac_core_result is None:
                missing.append("RTL8852BS2 static runtime MAC-core initialization")
        if args.require_runtime_bb_rf:
            runtime_bb_rf_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 BB/RF release complete\r?\n",
                started,
            )
            if runtime_bb_rf_result is None:
                missing.append("RTL8852BS2 BB/RF release sequence")
        if args.require_runtime_phy_cr:
            runtime_phy_cr_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 PHY CR image complete\r?\n",
                started,
            )
            if runtime_phy_cr_result is None:
                missing.append("RTL8852BS2 static BB PHY CR image")
        if args.require_runtime_bb_reset:
            bb_reset_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 BB reset complete\r?\n",
                started,
            )
            if bb_reset_result is None:
                missing.append("RTL8852BS2 BB reset pulse")
            else:
                if re.search(rb"K1 Wi-Fi GPL: BB reset offload entries=",
                             started) is None:
                    missing.append("RTL8852BS2 BB reset offload summary")

                if re.search(rb"K1 Wi-Fi GPL: BB reset offload response",
                             started) is not None:
                    missing.append(
                        "RTL8852BS2 BB reset offload batch without a "
                        "successful CMD_OFLD C2H response")
        if args.require_scan_phy_counters:
            phy_before = re.search(
                rb"K1 Wi-Fi GPL: scan PHY counters before crc-ok=", started)
            phy_after = re.search(
                rb"K1 Wi-Fi GPL: scan PHY counters after crc-ok=", started)
            if phy_before is None:
                missing.append("RTL8852BS2 scan RMAC receive counter baseline")

            if phy_after is None:
                missing.append("RTL8852BS2 scan RMAC receive counter result")

            if re.search(rb"K1 Wi-Fi GPL: scan PHY counters error=",
                         started) is not None:
                missing.append(
                    "RTL8852BS2 scan RMAC receive counters without a read "
                    "error")

            # The stage line carries the selection word read back after the
            # clear channel index was written.  Its low six bits must be 0x1f,
            # otherwise the counter window never answered and a snapshot of
            # zeroes says nothing about the receiver.

            stage = re.search(
                rb"K1 Wi-Fi GPL: scan PHY stage after .*raw=0x([0-9a-f]+)",
                started)
            if stage is None:
                missing.append(
                    "RTL8852BS2 scan RMAC receive counter stage sample")
            elif (int(stage.group(1), 16) & 0x3f) != 0x1f:
                missing.append(
                    "RTL8852BS2 scan RMAC receive counter window that "
                    "returns the selected index")
        if args.require_scan_rf_readback:
            for phase in (b"before", b"after"):
                if re.search(rb"K1 Wi-Fi GPL: scan RF readback " + phase +
                             rb" path=0x0*0 mode=", started) is None:
                    missing.append(
                        "RTL8852BS2 scan radio read-back sample " +
                        phase.decode())

            if re.search(rb"K1 Wi-Fi GPL: scan RF readback error=",
                         started) is not None:
                missing.append(
                    "RTL8852BS2 scan radio read-back without a read error")

            # Every radio value the serial interface returns is worthless
            # unless the baseband window it is read through returns real
            # content, so the sample also reads the baseband registers the
            # PHY CR image is known to have set.  Require at least one and
            # require every one of them to match.
            #
            # Only the sample taken before the scan is held to the image.
            # Some of these registers are dynamic - 0x49c0 changed across the
            # scan on the board - and a register that changes is evidence the
            # window is live, not evidence it is broken, so the sample taken
            # after the scan is required to exist and then reported rather
            # than compared.

            # Only the first sample can be held to the image, and an image
            # that holds one sweep does not hold the next one: an image where
            # more than one sweep runs takes a "before" sample after a scan
            # has already happened, and 0x49c0 is one of the registers that
            # changes across a scan.  Those later samples fall under the same
            # rule as the post-scan sample - required to exist, reported
            # rather than compared - so the comparison window ends at the
            # first post-scan sample.

            first_after = re.search(
                rb"K1 Wi-Fi GPL: scan BB image after index=", started)
            image_window = (started if first_after is None
                            else started[:first_after.start()])

            image = re.findall(
                rb"K1 Wi-Fi GPL: scan BB image before index=0x[0-9a-f]+ "
                rb"address=0x[0-9a-f]+ expect=0x([0-9a-f]+) "
                rb"actual=0x([0-9a-f]+)", image_window)
            if not image:
                missing.append(
                    "RTL8852BS2 scan baseband image read-back sample")
            elif any(int(expect, 16) != int(actual, 16)
                     for expect, actual in image):
                missing.append(
                    "RTL8852BS2 scan baseband window that returns the "
                    "PHY CR image values")

            if re.search(rb"K1 Wi-Fi GPL: scan BB image after index=",
                         started) is None:
                missing.append(
                    "RTL8852BS2 scan baseband image read-back sample after")
        if args.require_rf_context:
            rf_context_result = re.search(
                rb"K1 Wi-Fi GPL: RF context read complete\r?\n",
                started,
            )
            if rf_context_result is None:
                missing.append("RTL8852BS2 board RF context report")
        if args.require_runtime_rf_cr:
            rf_cr_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 RF radio image complete\r?\n",
                started,
            )
            if rf_cr_result is None:
                missing.append("RTL8852BS2 RF radio A/B parameter image")
            else:
                for path_id in (b"A", b"B"):
                    if re.search(
                        rb"K1 Wi-Fi GPL: RF radio " + path_id +
                        rb" offload entries=", started) is None:
                        missing.append(
                            "RTL8852BS2 RF radio %s offload summary"
                            % path_id.decode("ascii"))

                if re.search(rb"K1 Wi-Fi GPL: RF radio [AB] offload response",
                             started) is not None:
                    missing.append(
                        "RTL8852BS2 RF radio offload batch without a "
                        "successful CMD_OFLD C2H response")

                if re.search(rb"K1 Wi-Fi GPL: RF CR guard refused",
                             started) is not None:
                    missing.append(
                        "RTL8852BS2 RF radio image accepted for this board "
                        "RFE/CV")

                # halrf_dm_init() closes the RF stage by handing the firmware
                # the board RFE type.  The vendor asks for no ack, so the
                # driver verifies the SDIO TX page instead and logs it here.
                if re.search(rb"K1 Wi-Fi GPL: RF init cfg H2C rfe=",
                             started) is None:
                    missing.append("RTL8852BS2 RF init config H2C")
        if args.require_runtime_control_plane:
            runtime_control_plane_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 runtime control-plane "
                rb"serialization complete\r?\n",
                started,
            )
            if runtime_control_plane_result is None:
                missing.append("RTL8852BS2 firmware role-control serialization")
        if args.require_runtime_address_cam:
            runtime_address_cam_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 runtime address CAM "
                rb"serialization complete\r?\n",
                started,
            )
            if runtime_address_cam_result is None:
                missing.append("RTL8852BS2 address/BSSID CAM serialization")
        if args.require_runtime_role_cam_done_ack:
            runtime_role_cam_done_ack_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 runtime role/CAM done-ack "
                rb"complete\r?\n",
                started,
            )
            if runtime_role_cam_done_ack_result is None:
                missing.append("RTL8852BS2 no-link role/CAM done acknowledgements")
        if args.require_runtime_scanofld_channel_done_ack:
            runtime_scanofld_channel_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 passive scan channel-list "
                rb"done-ack complete\r?\n",
                started,
            )
            if runtime_scanofld_channel_result is None:
                missing.append("RTL8852BS2 passive scan channel-list done acknowledgement")
        if args.require_runtime_scanofld_passive:
            runtime_scanofld_passive_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 passive scan-offload "
                rb"complete\r?\n",
                started,
            )
            if runtime_scanofld_passive_result is None:
                missing.append("RTL8852BS2 one-shot passive scan-offload")
        if args.require_runtime_scanofld_rx:
            # The summary line continues past "bss=" with channel/ssid-len/bssid,
            # so the match must stay inside one line instead of anchoring on the
            # newline, and "bss=" must be exactly 1 rather than a 1-prefixed
            # value such as 0x1a.
            runtime_scanofld_rx_result = re.search(
                rb"K1 Wi-Fi GPL: passive scan RX [^\r\n]*"
                rb" bss=(?:0x)?0*1(?![0-9a-fA-F])",
                started,
            )
            if runtime_scanofld_rx_result is None:
                missing.append("RTL8852BS2 passive Beacon/BSS RX")
            else:
                # A BSS is only accepted when it came from a Beacon or a Probe
                # Response carrying a non-zero BSSID; assert that on the same
                # line instead of trusting the bss flag alone.
                runtime_scanofld_mgmt_result = re.search(
                    rb"K1 Wi-Fi GPL: passive scan RX [^\r\n]*"
                    rb" (?:beacon|probe-rsp)=(?:0x)?0*[1-9a-fA-F]",
                    started,
                )
                if runtime_scanofld_mgmt_result is None:
                    missing.append(
                        "RTL8852BS2 passive Beacon/Probe-Response frame")

                runtime_scanofld_bssid_result = re.search(
                    rb"K1 Wi-Fi GPL: passive scan RX [^\r\n]*"
                    rb" bssid=(?:0x0*)*[1-9a-fA-F]",
                    started,
                )
                if runtime_scanofld_bssid_result is None:
                    missing.append("RTL8852BS2 passive scan BSSID")
        if args.require_runtime_scanofld_active:
            # A Probe Response is the only frame in this image that cannot
            # exist unless a Probe Request was radiated, so it is what proves
            # transmit.  It has to be read from the part of the log that
            # follows the packet offload and ends at the completion line: an
            # earlier passive sweep, and the later wapi sweep, both resubmit
            # the passive table and neither says anything about transmit.
            offload_result = re.search(
                rb"K1 Wi-Fi GPL: probe-request packet-offload done-ack "
                rb"return=0x0+(?![0-9a-fA-F])",
                started,
            )
            if offload_result is None:
                missing.append(
                    "RTL8852BS2 Probe Request firmware packet offload")
            else:
                active_end = re.search(
                    rb"K1 Wi-Fi GPL: RTL8852BS2 active scan probe response "
                    rb"complete\r?\n",
                    started,
                )
                if active_end is None:
                    missing.append("RTL8852BS2 active scan completion")
                    active = started[offload_result.end():]
                else:
                    active = started[offload_result.end():active_end.start()]

                # probe-id is chinfo probe_req_pkt_id.  It names the offloaded
                # packet and is accompanied by the tx_pkt bit, so this line is
                # what separates the channel table that transmits from the
                # passive one, whose probe-id stays at the original
                # PKT_OFLD_NOT_EXISTS_ID of 0xff.
                active_table_result = re.search(
                    rb"K1 Wi-Fi GPL: passive scan channel-list H2C queued "
                    rb"channels=1-13 sequence=5 "
                    rb"probe-id=0x0+(?![0-9a-fA-F])",
                    active,
                )
                if active_table_result is None:
                    missing.append(
                        "RTL8852BS2 active scan-offload channel table")

                probe_response_result = re.search(
                    rb"K1 Wi-Fi GPL: passive scan RX [^\r\n]*"
                    rb" probe-rsp=(?:0x)?0*[1-9a-fA-F]",
                    active,
                )
                if probe_response_result is None:
                    missing.append("RTL8852BS2 Probe Response RX")
        if args.require_runtime_auth:
            # The whole exchange is judged from the report line the diagnostic
            # prints, plus the completion line, because the report is what
            # separates the two ways this can fail: a request that was never
            # accepted by the transmit path, and one that was transmitted and
            # not answered.  rsp-self is counted in host software by comparing
            # A1 against the eFuse self MAC, so no register setting can make
            # another station's frame satisfy it.
            auth_target_result = None
            for candidate in re.finditer(
                    rb"K1 Wi-Fi GPL: auth target bssid=([0-9a-f]{12}) "
                    rb"channel=(?:0x)?0*[1-9a-fA-F]",
                    started):
                if candidate.group(1).strip(b"0"):
                    auth_target_result = candidate
                    break

            if auth_target_result is None:
                missing.append("RTL8852BS2 authentication target BSS")
                auth = started
            else:
                auth = started[auth_target_result.end():]

            auth_tx_result = re.search(
                rb"K1 Wi-Fi GPL: auth request tx channel=(?:0x)?0*"
                rb"[1-9a-fA-F][^\r\n]* status=0x0+(?![0-9a-fA-F])",
                auth,
            )
            if auth_tx_result is None:
                missing.append("RTL8852BS2 Authentication Request transmit")

            auth_response_result = re.search(
                rb"K1 Wi-Fi GPL: auth req=(?:0x)?0*[1-9a-fA-F][^\r\n]*"
                rb" rsp-self=(?:0x)?0*[1-9a-fA-F]",
                auth,
            )
            if auth_response_result is None:
                missing.append("RTL8852BS2 Authentication Response RX")

            auth_end_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 authentication response "
                rb"complete\r?\n",
                auth,
            )
            if auth_end_result is None:
                missing.append("RTL8852BS2 authentication completion")
        if args.require_runtime_join:
            # Two commands and then a repeat of the whole directed exchange.
            # The firmware return of each done acknowledgement is what says the
            # command was understood rather than merely queued, and the Beacon
            # count of the sweep that follows is what says programming a BSSID
            # and an infrastructure network type into the address CAM did not
            # quietly start filtering receive traffic.
            join_target_result = None
            for candidate in re.finditer(
                    rb"K1 Wi-Fi GPL: join target bssid=([0-9a-f]{12}) "
                    rb"channel=(?:0x)?0*[1-9a-fA-F]",
                    started):
                if candidate.group(1).strip(b"0"):
                    join_target_result = candidate
                    break

            if join_target_result is None:
                missing.append("RTL8852BS2 station join target BSS")
                join = started
            else:
                join = started[join_target_result.end():]

            join_info_result = re.search(
                rb"K1 Wi-Fi GPL: join info done-ack "
                rb"return=0x0+(?![0-9a-fA-F])",
                join,
            )
            if join_info_result is None:
                missing.append("RTL8852BS2 station join JOININFO done-ack")

            join_cam_result = re.search(
                rb"K1 Wi-Fi GPL: join CAM done-ack "
                rb"return=0x0+(?![0-9a-fA-F])",
                join,
            )
            if join_cam_result is None:
                missing.append(
                    "RTL8852BS2 station join address CAM done-ack")

            # The label keeps this line apart from the pre-join exchange's own
            # report, so --require-runtime-auth and this check cannot be
            # satisfied by the same line.
            join_auth_result = re.search(
                rb"K1 Wi-Fi GPL: join auth req=(?:0x)?0*[1-9a-fA-F][^\r\n]*"
                rb" rsp-self=(?:0x)?0*[1-9a-fA-F]",
                join,
            )
            if join_auth_result is None:
                missing.append(
                    "RTL8852BS2 Authentication Response RX after the join")

            join_rx_result = re.search(
                rb"K1 Wi-Fi GPL: join bss=(?:0x)?0*[1-9a-fA-F]",
                join,
            )
            if join_rx_result is None:
                missing.append("RTL8852BS2 Beacon RX after the join")

            join_end_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 station join complete\r?\n",
                join,
            )
            if join_end_result is None:
                missing.append("RTL8852BS2 station join completion")
        if args.require_runtime_assoc_response or args.require_runtime_assoc:
            # Everything is judged from the lines the association step prints,
            # sliced from its own target line so none of the earlier steps'
            # reports can satisfy these checks.  The target line also has to
            # describe a BSS this port is allowed to ask, which is one of
            # exactly two shapes: an open BSS, which gets no RSN element
            # (privacy=0x0 rsn-tx=0x0), or a BSS that requires confidentiality
            # and advertises suites the request builder can name, which gets one
            # (privacy=0x1 rsn-ccmp=0x1 rsn-psk=0x1 rsn-tx=0x1).  Privacy
            # without a transmitted RSN element, or an element sent to a BSS
            # that did not ask for one, would mean the target selection had been
            # bypassed rather than that the exchange worked -- the frame would
            # be refused by IEEE 802.11 clause 12.6.3 before this port's frame
            # path mattered.  rsn-tx is what the builder actually put in the
            # request, so no beacon field can stand in for it.
            assoc_target_result = None
            for candidate in re.finditer(
                    rb"K1 Wi-Fi GPL: assoc target bssid=([0-9a-f]{12}) "
                    rb"channel=(?:0x)?0*[1-9a-fA-F][^\r\n]*"
                    rb"(?: privacy=0x0+ [^\r\n]*rsn-tx=0x0+"
                    rb"| privacy=0x0*1 [^\r\n]*rsn-ccmp=0x0*1"
                    rb" rsn-psk=0x0*1 rsn-tx=0x0*1)"
                    rb"(?![0-9a-fA-F])",
                    started):
                if candidate.group(1).strip(b"0"):
                    assoc_target_result = candidate
                    break

            if assoc_target_result is None:
                missing.append(
                    "RTL8852BS2 association target BSS this port may ask")
                assoc = started
            else:
                assoc = started[assoc_target_result.end():]

            # The first attempt always uses the SSID the target advertised.  A
            # run whose only attempt was the sibling guess would be a different
            # experiment and is not accepted as this one.
            assoc_advertised_result = re.search(
                rb"K1 Wi-Fi GPL: assoc advertised begin "
                rb"ssid-source=advertised",
                assoc,
            )
            if assoc_advertised_result is None:
                missing.append(
                    "RTL8852BS2 association attempt with the advertised SSID")

            assoc_tx_result = re.search(
                rb"K1 Wi-Fi GPL: assoc request tx channel=(?:0x)?0*"
                rb"[1-9a-fA-F][^\r\n]* status=0x0+(?![0-9a-fA-F])",
                assoc,
            )
            if assoc_tx_result is None:
                missing.append("RTL8852BS2 Association Request transmit")

            # rsp on the exchange line is set only by an Association Response
            # whose A1 was compared against the eFuse self MAC in host
            # software, so no register setting can let another station's frame
            # satisfy it.
            assoc_response_result = re.search(
                rb"K1 Wi-Fi GPL: assoc exchange rsp=(?:0x)?0*[1-9a-fA-F]",
                assoc,
            )
            if assoc_response_result is None:
                missing.append("RTL8852BS2 Association Response RX")
        if args.require_runtime_assoc:
            # A granted association identifier, both re-sent connect commands
            # acknowledged with it, and a confirming sweep that still hears the
            # target.  The completion line is printed only for status 0 with a
            # non-zero identifier, so a refusal cannot satisfy this.
            assoc_info_result = re.search(
                rb"K1 Wi-Fi GPL: assoc info done-ack "
                rb"return=0x0+(?![0-9a-fA-F])",
                assoc,
            )
            if assoc_info_result is None:
                missing.append(
                    "RTL8852BS2 association JOININFO done-ack with the AID")

            assoc_cam_result = re.search(
                rb"K1 Wi-Fi GPL: assoc CAM done-ack "
                rb"return=0x0+(?![0-9a-fA-F])",
                assoc,
            )
            if assoc_cam_result is None:
                missing.append(
                    "RTL8852BS2 association address CAM done-ack with the AID")

            assoc_confirm_result = re.search(
                rb"K1 Wi-Fi GPL: assoc confirm bss=(?:0x)?0*[1-9a-fA-F]"
                rb"[^\r\n]* beacons=(?:0x)?0*[1-9a-fA-F]",
                assoc,
            )
            if assoc_confirm_result is None:
                missing.append(
                    "RTL8852BS2 Beacon RX after the association AID update")

            assoc_end_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 station association complete\r?\n",
                assoc,
            )
            if assoc_end_result is None:
                missing.append("RTL8852BS2 station association completion")
        if args.require_h2c_tx_resource:
            h2c_tx_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 H2C TX resource diagnostic "
                rb"complete\r?\n",
                started,
            )
            if h2c_tx_result is None:
                missing.append("RTL8852BS2 H2C TX resource/descriptor diagnostic")
        if args.require_firmware_header_packet:
            fw_header_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 FW header packet complete\r?\n",
                started,
            )
            if fw_header_result is None:
                missing.append("RTL8852BS2 firmware static-header H2C packet")
        if args.require_firmware_section_packet:
            fw_section_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 FW section0 packet complete\r?\n",
                started,
            )
            if fw_section_result is None:
                missing.append("RTL8852BS2 first firmware section packet")
        if args.require_firmware_section0_tail_packet:
            fw_section_tail_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 FW section0 tail packet complete\r?\n",
                started,
            )
            if fw_section_tail_result is None:
                missing.append("RTL8852BS2 28-byte post-first-packet probe")
        if args.require_firmware_section0_second_packet:
            fw_section_second_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 FW section0 second packet "
                rb"complete\r?\n",
                started,
            )
            if fw_section_second_result is None:
                missing.append("RTL8852BS2 second firmware section-zero packet")
        if args.require_firmware_section0_third_packet:
            fw_section_third_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 FW section0 third packet "
                rb"complete\r?\n",
                started,
            )
            if fw_section_third_result is None:
                missing.append("RTL8852BS2 third firmware section-zero packet")
        if args.require_firmware_section0_fourth_packet:
            fw_section_fourth_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 FW section0 fourth packet "
                rb"complete\r?\n",
                started,
            )
            if fw_section_fourth_result is None:
                missing.append("RTL8852BS2 fourth firmware section-zero packet")
        if args.require_sdio_pre_init:
            sdio_pre_init_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 SDIO pre-init complete\r?\n",
                started,
            )
            if sdio_pre_init_result is None:
                missing.append("RTL8852BS2 SDIO pre-initialization")
        if args.require_bringup_success:
            bringup_failure = re.search(
                rb"(?:ERROR: )?K1 RTL8852BS2 bring-up failed:", started
            )
            if bringup_failure is not None:
                missing.append("successful K1 RTL8852BS2 bring-up")
        if args.require_cmd53_hisr_read:
            hisr_result = re.search(
                rb"K1 Wi-Fi: F1 HISR CMD53=(?:0x)?[0-9a-fA-F]+\r?\n",
                started,
            )
            if hisr_result is None:
                missing.append("4-byte Function 1 HISR CMD53 read")
        if args.require_wlan0_scan and \
                b"K1 Wi-Fi: wlan0 register error=" in started:
            missing.append("successful wlan0 scan device registration")
        if missing:
            raise XmodemError("NuttX started without: " + ", ".join(missing))
        if args.require_runtime_rx_worker:
            runtime_rx_complete = re.compile(
                rb"K1 Wi-Fi: runtime RX transfer=(?:0x)?0*30 "
                rb"frames=(?:0x)?0*1 data=(?:0x)?0* C2H=(?:0x)?0*1 "
                rb"dispatched=(?:0x)?0*1"
            )
            if runtime_rx_complete.search(started) is None:
                wait_for_runtime_rx_worker(serial)
            else:
                print("PASS: K1 runtime RX worker dispatched C2H loopback",
                      file=sys.stderr)
        if args.verify_bt_hci_open:
            verify_bt_hci_open(serial)
        if args.require_bt_host_scan:
            verify_bt_host_scan(serial, args.bt_ifname, args.bt_scan_seconds)
        if args.require_wlan0_scan:
            verify_wlan0_scan(serial, args.wlan0_ifname,
                              args.wlan0_scan_timeout)
        print("PASS: K1 wireless RAM image reached NSH", file=sys.stderr)
        return 0
    finally:
        serial.close()
        if compressed_payload is not None:
            compressed_payload.unlink(missing_ok=True)
        print(f"Serial log: {log_path}", file=sys.stderr)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (XmodemError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
