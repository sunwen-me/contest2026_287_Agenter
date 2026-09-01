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
    parser.add_argument(
        "--replay", type=pathlib.Path, default=None,
        help=("read a captured serial log instead of booting a board, and "
              "apply every verdict below to it -- no device is opened and "
              "nothing is transmitted.  This exists because run 67 was "
              "reported FAIL by a stale expectation in this script while the "
              "board's own selftest passed: a verdict change can now be "
              "checked against captured logs before it costs a board cycle"),
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
        "--require-runtime-tx-security", action="store_true",
        help=("fail unless the RTL8852BS2 protected-transmit descriptor "
              "security dword and CCMP header builder check out"),
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
        "--require-runtime-port-init", action="store_true",
        help=("fail unless the band 0 / port 0 infrastructure subset of "
              "mac_port_init() runs on that association, every field it "
              "programs reads back, the port's network type and function "
              "enable end up set, and the port's own TSF is running when the "
              "vendor's TSF-based delay asks"),
    )
    parser.add_argument(
        "--require-runtime-resident", action="store_true",
        help=("fail unless the associated channel is held with no sweep "
              "running: a Beacon from the target and its Probe Response back "
              "inside one bounded receive window, every sampled channel "
              "register unchanged across it, the receive filter restored, and "
              "the target's traffic indication map read for this station's "
              "AID, and any of this port's own uplink frames the access "
              "point sent back out counted, and the frames addressed to "
              "this station counted in software beside the descriptor "
              "bit, with the error-packet filter widened and the "
              "receive MAC stage counters read across the window"),
    )
    parser.add_argument(
        "--require-runtime-data-secure-tx", action="store_true",
        help=("fail unless the protected data frame this host transmits "
              "checks out against its offline model and the data queue "
              "accepts one on the associated channel with a pairwise key "
              "installed"),
    )
    parser.add_argument(
        "--require-runtime-arp-probe", action="store_true",
        help=("fail unless the observer that learns a station from decrypted "
              "group traffic and recognises an ARP reply addressed to this "
              "host agrees with its offline model, and the resident window "
              "reports what its protected ARP request did"),
    )
    parser.add_argument(
        "--require-runtime-ccmp-selftest", action="store_true",
        help=("fail unless the software CCMP the loopback readback measures "
              "the hardware against reproduces RFC 3610 packet vector one "
              "and, for a frame shaped like the one the readback submits, "
              "the authenticated data, the nonce, the ciphertext and the "
              "integrity code that the published sample key produces"),
    )
    parser.add_argument(
        "--require-runtime-loopback-readback", action="store_true",
        help=("fail unless the reader that decides whether a frame came back "
              "out of MAC loopback as ciphertext or as the plaintext that was "
              "submitted agrees with its offline model, and the resident "
              "window reports what its own looped frame carried, both for the "
              "frame it sends to the access point and for the from-DS frame "
              "it addresses to itself"),
    )
    parser.add_argument(
        "--require-runtime-wpa-msg1", action="store_true",
        help=("fail unless a pairwise master key is derived from a configured "
              "passphrase and the access point sends a first EAPOL-Key frame "
              "addressed to the eFuse self MAC that this host answers with a "
              "second message"),
    )
    parser.add_argument(
        "--require-runtime-wpa-mic", action="store_true",
        help=("fail unless the access point's third EAPOL-Key message arrives "
              "and its HMAC-SHA1-128 integrity code verifies against the "
              "pairwise transient key this host derived, which no register "
              "setting can produce and only the same pre-shared key can"),
    )
    parser.add_argument(
        "--require-runtime-wpa", action="store_true",
        help=("fail unless that verified third message is acknowledged with a "
              "fourth and the WPA2-PSK four-way handshake completes"),
    )
    parser.add_argument(
        "--require-runtime-wpa-keys", action="store_true",
        help=("fail unless the security engine comes up, the group key inside "
              "the verified third message is unwrapped, and the firmware "
              "acknowledges all four commands that install the pairwise and "
              "group keys into the address CAM and the security CAM"),
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
    abort_sent = False
    next_wait_notice = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        output = serial.received[start:]
        if b"=>" in output:
            # The stop stream runs until this prompt is read, so part of it can
            # still be in flight -- and the host can be behind the board, so
            # what is still in flight is measured in hundreds of milliseconds
            # rather than in bytes.  Drain the remainder and kill the line
            # until the prompt is genuinely empty; otherwise the first command
            # would inherit a leading stop byte and U-Boot would reject it.
            for _ in range(3):
                serial.read(0.4)
                mark = len(serial.received)
                serial.write_text(b"\x15\r")
                prompt_at = serial.wait_for_text(b"=>", 5.0, mark)
                serial.read(0.3)
                if not bytes(serial.received[prompt_at:]).strip():
                    return

            raise XmodemError("U-Boot prompt kept receiving stop bytes")

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
            abort_sent = False
            continue

        now = time.monotonic()
        if now >= next_wait_notice:
            print("[serial] still waiting for a physical RST", file=sys.stderr)
            next_wait_notice = now + 10.0

        # This board's autoboot delay is zero: U-Boot leaves it only when a
        # byte is already pending the instant it looks, and it announces no
        # window to aim at.  One byte timed off the main banner is enough only
        # while the host reads the console in step with the board, which a
        # USB-TTL re-enumeration burst breaks -- the host can fall several
        # hundred milliseconds behind, so a byte it sends "before" the autoboot
        # line has in truth arrived after it.  So every marker that means a
        # reset is under way starts a continuous stop stream instead, and it is
        # the stream, not the timing, that covers the window.
        if not saw_spl_banner and b"U-Boot SPL 2022" in output:
            saw_spl_banner = True
            if abort_deadline == 0.0:
                abort_deadline = now + 20.0
            print("[serial] K1 SPL banner detected; sending the stop stream",
                  file=sys.stderr)

        if not saw_uboot_banner and b"U-Boot 2022" in output:
            saw_uboot_banner = True
            if abort_deadline == 0.0:
                abort_deadline = now + 20.0
            print("[serial] K1 U-Boot banner detected; waiting for stop window",
                  file=sys.stderr)

        # A USB-TTL adapter can re-enumerate between the U-Boot banner and the
        # console setup.  The reopened descriptor then sees the console marker
        # but not the banner, and that marker still precedes the autoboot point.
        if abort_deadline == 0.0 and b"In:    serial" in output:
            saw_uboot_banner = True
            abort_deadline = now + 20.0
            print("[serial] K1 U-Boot console detected after re-enumeration",
                  file=sys.stderr)

        # Once U-Boot runs its boot command, console input can no longer stop
        # it.  With a physical reset that is not fatal: the next RST press
        # opens a fresh window, so discard this attempt and keep listening
        # rather than making the operator restart the tool.
        if b"Try to boot" in output or b"Starting kernel" in output:
            if not allow_running_os:
                raise XmodemError("K1 U-Boot stop window expired")

            print("[serial] missed the autoboot window; press RST again",
                  file=sys.stderr)
            start = len(serial.received)
            saw_uboot_banner = False
            saw_spl_banner = False
            abort_deadline = 0.0
            abort_sent = False
            continue

        if abort_deadline != 0.0:
            if now >= abort_deadline:
                # No boot attempt and no prompt followed the marker either, so
                # it was a stale one.  Stop the stream and wait for a reset.
                saw_uboot_banner = False
                saw_spl_banner = False
                abort_deadline = 0.0
                abort_sent = False
                continue

            try:
                serial.write_text(b"s")
            except (OSError, XmodemError):
                # The USB-TTL node can disappear mid-reset; the next polling
                # pass reopens it through the stable by-id link.
                pass

            if not abort_sent:
                abort_sent = True
                print("[serial] K1 U-Boot stop stream started",
                      file=sys.stderr)

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


def boot_from_board(args: argparse.Namespace, serial: K1Xmodem,
                    wrapper: pathlib.Path,
                    payload: pathlib.Path) -> tuple:
    """Stop U-Boot, load the RAM image and return everything NuttX printed.

    Split out of main() so the verdicts below can also be applied to a
    captured log with --replay.  Nothing here is reachable in replay mode:
    no device is opened, no reset is requested and no byte is transmitted.
    """

    compressed_payload = None
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
    return started, compressed_payload


def main() -> int:
    args = parse_args()
    if args.bt_scan_seconds <= 0:
        raise XmodemError("--bt-scan-seconds must be positive")
    if args.wlan0_scan_timeout <= 0:
        raise XmodemError("--wlan0-scan-timeout must be positive")
    replay = None
    if args.replay is not None:
        replay = require_file(args.replay, "replay log")
    wrapper = None if replay is not None else require_file(
        args.wrapper, "wrapper")
    payload = None if replay is not None else require_file(
        args.payload, "payload")
    compressed_payload = None
    if replay is not None:
        # No device, no reset, no transmission: the log is the whole input.
        # Every verdict below reads the captured bytes, so the ones that
        # depend on asking the board a question at the end are skipped and
        # said to be skipped rather than silently counted as passing.
        log_path = replay
        serial = None
        print(f"[replay] applying verdicts to {replay} without a board",
              file=sys.stderr)
    else:
        args.log_dir.mkdir(parents=True, exist_ok=True)
        timestamp = dt.datetime.now(
            dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        log_path = args.log_dir / f"{payload.parent.name}-{timestamp}.log"
        device = resolve_serial_device(args.device)
        print(f"[serial] using {device}", file=sys.stderr)
        serial = K1Xmodem(
            device,
            args.baud,
            log_path,
            reconnect_on_reenumeration=True,
        )

    try:
        if replay is not None:
            started = replay.read_bytes()
            print(f"[replay] {len(started)} bytes captured",
                  file=sys.stderr)
        else:
            started, compressed_payload = boot_from_board(
                args, serial, wrapper, payload)
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
            if serial is not None:
                started += wait_for_bt_host_registration(serial)
            required.append(b"K1 Bluetooth: H5 host stack registered")
        elif (args.require_h5 or args.require_bt_hci_device) and \
                b"K1 Bluetooth: H5 raw HCI registered /dev/ttyHCI0" not in started:
            if serial is not None:
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
        if args.require_runtime_tx_security:
            runtime_tx_security_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 runtime TX security fields "
                rb"complete\r?\n",
                started,
            )
            if runtime_tx_security_result is None:
                missing.append("RTL8852BS2 runtime TX security fields")
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
        if (args.require_runtime_assoc_response or args.require_runtime_assoc
                or args.require_runtime_port_init):
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
        if args.require_runtime_port_init:
            # mac_port_init()'s band 0 / port 0 subset, judged from the lines it
            # prints inside the association slice.  The begin line has to report
            # a port that was still disabled and the station parameters, so a
            # second call that skipped the sequence cannot satisfy this.
            port_begin_result = re.search(
                rb"K1 Wi-Fi GPL: port init begin stat=0x0+ "
                rb"net-type=0x0*2(?![0-9a-fA-F]) "
                rb"bcn-intv=0x0*64(?![0-9a-fA-F])",
                assoc,
            )
            if port_begin_result is None:
                missing.append(
                    "RTL8852BS2 port 0 mac_port_init() start from a disabled "
                    "port")

            # The two fields that make the port a station of this BSS, each
            # required to have been written rather than found already set.  On
            # this firmware both start clear, which is what makes the step
            # responsible for them; a build where they no longer do would be a
            # different claim and has to be re-read rather than pass quietly.
            port_net_type_result = re.search(
                rb"K1 Wi-Fi GPL: port init net-type was=0x0+ "
                rb"set=0x0*800(?![0-9a-fA-F]) written",
                assoc,
            )
            if port_net_type_result is None:
                missing.append("RTL8852BS2 port 0 network type set to INFRA")

            port_func_en_result = re.search(
                rb"K1 Wi-Fi GPL: port init func-en was=0x0+ "
                rb"set=0x0*4(?![0-9a-fA-F]) written",
                assoc,
            )
            if port_func_en_result is None:
                missing.append("RTL8852BS2 port 0 function enable set")

            # dly_port_us() between the enable and the beacon early time: the
            # port's own timer has to advance, which is a property of the
            # hardware and not of any value this host wrote.
            port_tsf_result = re.search(
                rb"K1 Wi-Fi GPL: port init tsf-delay running=0x0*1"
                rb"(?![0-9a-fA-F]) error=0x0+(?![0-9a-fA-F])",
                assoc,
            )
            if port_tsf_result is None:
                missing.append("RTL8852BS2 port 0 TSF running at the port delay")

            # The result line carries the port state the sequence reached and
            # the status every read-back verification folded into.
            port_result_result = re.search(
                rb"K1 Wi-Fi GPL: port init result stat=0x0*3(?![0-9a-fA-F])"
                rb"[^\r\n]* tsf=0x0*1(?![0-9a-fA-F])"
                rb"[^\r\n]* status=0x0+(?![0-9a-fA-F])",
                assoc,
            )
            if port_result_result is None:
                missing.append(
                    "RTL8852BS2 port 0 mac_port_init() field read-back")

            port_end_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 port init complete\r?\n",
                assoc,
            )
            if port_end_result is None:
                missing.append("RTL8852BS2 port 0 initialisation completion")
        if (args.require_runtime_wpa_msg1 or args.require_runtime_wpa_mic
                or args.require_runtime_wpa or args.require_runtime_wpa_keys):
            # Everything is judged from the handshake's own lines, sliced from
            # the line that reports a pairwise master key was derived, so no
            # earlier step can satisfy these checks and an image built with no
            # passphrase cannot either: that line carries status=0x0 only when
            # PBKDF2 ran over a configured passphrase and the target's own SSID.
            wpa_pmk_result = re.search(
                rb"K1 Wi-Fi GPL: wpa pmk ssid-len=(?:0x)?0*[1-9a-fA-F]"
                rb"[0-9a-fA-F]* status=0x0+(?![0-9a-fA-F])",
                started,
            )
            if wpa_pmk_result is None:
                missing.append(
                    "RTL8852BS2 WPA2-PSK pairwise master key from a "
                    "configured passphrase")
                wpa = started
            else:
                wpa = started[wpa_pmk_result.end():]

            # The exchange line of the decided attempt carries the handshake
            # outcome, and every field in it is set by host software that
            # compared the frame's own addresses against the eFuse self MAC.
            wpa_exchange_result = re.search(
                rb"K1 Wi-Fi GPL: assoc exchange rsp=(?:0x)?0*[1-9a-fA-F]"
                rb"[^\r\n]* wpa=0x0*1 msg1=0x0*1 msg2=0x0*1 msg3=(0x[0-9a-f]+)"
                rb" mic=(0x[0-9a-f]+) msg4=(0x[0-9a-f]+)",
                wpa,
            )
            if wpa_exchange_result is None:
                missing.append(
                    "RTL8852BS2 EAPOL-Key message 1 RX and message 2 transmit")

            # The second message is proof of nothing on its own -- this host
            # composes it -- but its transmit status separates a frame the
            # hardware accepted from one the builder refused.
            wpa_msg2_result = re.search(
                rb"K1 Wi-Fi GPL: wpa msg2 tx bytes=(?:0x)?0*[1-9a-fA-F]"
                rb"[^\r\n]* status=0x0+(?![0-9a-fA-F])",
                wpa,
            )
            if wpa_msg2_result is None:
                missing.append("RTL8852BS2 EAPOL-Key message 2 transmit")
        if (args.require_runtime_wpa_mic or args.require_runtime_wpa
                or args.require_runtime_wpa_keys):
            # The load-bearing check of the whole increment.  mic=0x1 is set
            # only when this host recomputed HMAC-SHA1-128 over the received
            # frame under the key confirmation key it derived itself and the
            # result equalled the access point's, which the access point could
            # only have produced from the same pre-shared key.
            wpa_mic_result = re.search(
                rb"K1 Wi-Fi GPL: assoc exchange rsp=(?:0x)?0*[1-9a-fA-F]"
                rb"[^\r\n]* msg3=0x0*1 mic=0x0*1",
                wpa,
            )
            if wpa_mic_result is None:
                missing.append(
                    "RTL8852BS2 EAPOL-Key message 3 integrity code verified")

            # A run that verified a code and also counted a failure is not a
            # clean verification and is not accepted as one.
            wpa_failure_result = re.search(
                rb"K1 Wi-Fi GPL: assoc advertised eapol=[^\r\n]* "
                rb"mic-fail=0x0+(?![0-9a-fA-F])",
                wpa,
            )
            if wpa_failure_result is None:
                missing.append(
                    "RTL8852BS2 handshake with no integrity code failure")
        if args.require_runtime_wpa or args.require_runtime_wpa_keys:
            wpa_msg4_result = re.search(
                rb"K1 Wi-Fi GPL: wpa msg4 tx bytes=(?:0x)?0*[1-9a-fA-F]"
                rb"[^\r\n]* status=0x0+(?![0-9a-fA-F])",
                wpa,
            )
            if wpa_msg4_result is None:
                missing.append("RTL8852BS2 EAPOL-Key message 4 transmit")

            wpa_end_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 station WPA2 four-way handshake "
                rb"complete\r?\n",
                wpa,
            )
            if wpa_end_result is None:
                missing.append(
                    "RTL8852BS2 WPA2-PSK four-way handshake completion")
        if args.require_runtime_wpa_keys:
            # The security engine is judged from its own readback: the routine
            # reports status=0x0 only after reading both registers back and
            # finding the cipher clocks, the six direction enables and the
            # ICV/MIC append set with partial transmit mode clear.
            sec_eng_result = re.search(
                rb"K1 Wi-Fi GPL: sec-eng init status=0x0+(?![0-9a-fA-F])",
                started,
            )
            if sec_eng_result is None:
                missing.append("RTL8852BS2 security engine bring-up")

            # The group key is evidence in its own right: RFC 3394 puts eight
            # 0xa6 bytes in front of the plaintext and the unwrap refuses
            # anything else, so unwrap=0x0 means the access point wrapped with
            # the same key encryption key this host derived.  A recovered key
            # also has to be a length a CCMP-128 entry holds.
            wpa_gtk_result = re.search(
                rb"K1 Wi-Fi GPL: assoc advertised eapol=[^\r\n]* unwrap=0x0+"
                rb" kde=0x0+ gtk=0x0*1 gtk-len=0x0*10 ",
                wpa,
            )
            if wpa_gtk_result is None:
                missing.append(
                    "RTL8852BS2 group key unwrapped from the third message")

            # Each of the four commands separately, because each can be refused
            # on its own: the slot the key occupies and the entry that holds it,
            # for the pairwise key and then the group key.
            for label, description in (
                (b"TK CAM", "pairwise key address CAM slot"),
                (b"TK SEC", "pairwise key security CAM entry"),
                (b"GTK CAM", "group key address CAM slot"),
                (b"GTK SEC", "group key security CAM entry"),
            ):
                command_result = re.search(
                    rb"K1 Wi-Fi GPL: " + label
                    + rb" done-ack return=0x0+(?![0-9a-fA-F])",
                    wpa,
                )
                if command_result is None:
                    missing.append(
                        "RTL8852BS2 firmware acknowledgement of the "
                        + description)

            # The report line is the host's own view of the same four commands
            # plus the addressing they used: security entry mode 2, the pairwise
            # slot 0 and the group slot 2 live, entry 0 and entry 1.
            wpa_keys_result = re.search(
                rb"K1 Wi-Fi GPL: station keys sec-mode=0x0*2 sec-valid=0x0*5"
                rb" tk-ent=0x0+ tk-keyid=0x0+ tk-cam=0x0+ tk-sec=0x0+"
                rb" tk=0x0*1 gtk-ent=0x0*1 gtk-keyid=0x[0-9a-fA-F]+"
                rb" gtk-cam=0x0+ gtk-sec=0x0+ gtk=0x0*1",
                wpa,
            )
            if wpa_keys_result is None:
                missing.append(
                    "RTL8852BS2 pairwise and group key CAM installation")

            wpa_keys_end_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 station WPA2 keys installed\r?\n",
                wpa,
            )
            if wpa_keys_end_result is None:
                missing.append("RTL8852BS2 WPA2-PSK key installation")
        if args.require_runtime_resident:
            # The resident window's own lines, sliced from the line it prints
            # on entry so nothing a sweep received can satisfy these checks.
            # That line exists only inside the window, which runs after the
            # association step has already reported its result.
            resident_begin_result = re.search(
                rb"K1 Wi-Fi GPL: resident window enter channel="
                rb"(?:0x)?0*[1-9a-fA-F][0-9a-fA-F]*",
                started,
            )
            if resident_begin_result is None:
                missing.append("RTL8852BS2 resident window start")
                resident = started
            else:
                resident = started[resident_begin_result.end():]

            # The park step that runs before the window, and the window's own
            # read-back of what it did.  Run 36 polled a whole window on
            # channel 13 against an access point on channel 1, because the
            # sweep that confirms the association walks 1 to 13 unparked and
            # ends wherever it ends; the park step is what puts the radio back
            # and this is what says it landed.  The completion marker is
            # searched before the window's slice because the step runs before
            # the window's first line.
            resident_park_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 station resident park "
                rb"complete\r?\n",
                started,
            )
            if resident_park_result is None:
                missing.append(
                    "RTL8852BS2 radio parked on the associated channel")

            # The two numbers are compared rather than matched: a regular
            # expression cannot say "equal to the other capture", and the
            # channel this has to equal is whatever the association ran on.
            resident_parked_result = re.search(
                rb"K1 Wi-Fi GPL: resident window parked="
                rb"(?:0x)?0*([0-9a-fA-F]+) bb-ch=(?:0x)?[0-9a-fA-F]+"
                rb" target=(?:0x)?0*([0-9a-fA-F]+)",
                resident,
            )
            if (resident_parked_result is None or
                    int(resident_parked_result.group(1), 16) !=
                    int(resident_parked_result.group(2), 16)):
                missing.append(
                    "RTL8852BS2 resident window on the associated channel")

            # A Beacon from the access point this run associated with, received
            # with no sweep running.  Beacons from other access points are
            # counted in the same line and deliberately not accepted here: they
            # would prove the receiver works without proving anything about the
            # channel this host is supposed to be holding.
            resident_beacon_result = re.search(
                rb"K1 Wi-Fi GPL: resident window channel=[^\r\n]*"
                rb" bcn-target=(?:0x)?0*[1-9a-fA-F]",
                resident,
            )
            if resident_beacon_result is None:
                missing.append(
                    "RTL8852BS2 Beacon from the associated access point with "
                    "no sweep running")

            # What the access point's Beacons say is waiting for this station.
            # The line is required to exist and no value in it is required:
            # both readings are conclusions.  A set AID bit means the answer to
            # this window's probes exists and was never collected, which is the
            # one explanation a silent window cannot otherwise be told apart
            # from an access point that produced no answer at all.  A clear bit
            # is the negative of that one explanation and nothing more: an
            # answer for a station the access point believes awake goes out
            # immediately and never appears in a map.
            resident_tim_result = re.search(
                rb"K1 Wi-Fi GPL: resident window tim aid=(?:0x)?"
                rb"([0-9a-fA-F]+) seen=(?:0x)?([0-9a-fA-F]+)"
                rb" aid-set=(?:0x)?([0-9a-fA-F]+)"
                rb" bcast-set=(?:0x)?([0-9a-fA-F]+)"
                rb" absent=(?:0x)?([0-9a-fA-F]+)"
                rb" short=(?:0x)?([0-9a-fA-F]+)"
                rb" out-of-range=(?:0x)?([0-9a-fA-F]+)"
                rb" dtim-count=(?:0x)?([0-9a-fA-F]+)"
                rb" dtim-period=(?:0x)?([0-9a-fA-F]+)"
                rb" ctl=(?:0x)?([0-9a-fA-F]+)"
                rb" bmap-len=(?:0x)?([0-9a-fA-F]+)"
                rb" len=(?:0x)?([0-9a-fA-F]+)"
                rb" head=([0-9a-fA-F]*)",
                resident,
            )
            if resident_tim_result is None:
                missing.append(
                    "RTL8852BS2 traffic indication map from the associated "
                    "access point")
            else:
                (tim_aid, tim_seen, tim_aid_set, tim_bcast_set, tim_absent,
                 tim_short, tim_range, tim_dtim_count, tim_dtim_period,
                 tim_control, tim_bitmap, tim_frame) = (
                     int(group, 16)
                     for group in resident_tim_result.groups()[:12])
                tim_head = resident_tim_result.group(13).decode("ascii")

                print(
                    "[serial] traffic indication map: aid={0} seen={1} "
                    "aid-set={2} bcast-set={3} absent={4} short={5} "
                    "out-of-range={6} dtim={7}/{8} ctl={9:#04x} "
                    "bitmap={10} frame={11} head={12}"
                    .format(tim_aid, tim_seen, tim_aid_set, tim_bcast_set,
                            tim_absent, tim_short, tim_range, tim_dtim_count,
                            tim_dtim_period, tim_control, tim_bitmap,
                            tim_frame, tim_head or "none"),
                    file=sys.stderr)
                if tim_aid == 0:
                    print(
                        "[serial] this station holds no association "
                        "identifier, so no bit in the map belongs to it: "
                        "the reading is void, not negative",
                        file=sys.stderr)
                elif tim_seen == 0:
                    print(
                        "[serial] no Beacon from the access point carried a "
                        "map ({0} of them had no element list this reader "
                        "could walk, first was {1} bytes), so this window "
                        "says nothing about buffered traffic"
                        .format(tim_absent, tim_frame),
                        file=sys.stderr)
                elif tim_aid_set:
                    print(
                        "[serial] the access point is holding a frame for "
                        "this station in {0} of {1} maps: the answer to the "
                        "window's probes exists and was never collected, so "
                        "what is missing is on this side -- the access point "
                        "believes this station is asleep"
                        .format(tim_aid_set, tim_seen),
                        file=sys.stderr)
                else:
                    print(
                        "[serial] no map in {0} Beacons had this station's "
                        "bit set -- {1} of them left out the octet it lives "
                        "in, which clause 9.4.2.6 defines as a clear bit -- "
                        "so the access point is holding nothing for it: an "
                        "answer waiting in its buffer for a station it thinks "
                        "is asleep is ruled out, and an answer that was never "
                        "produced and one sent straight out and not received "
                        "are still to be told apart"
                        .format(tim_seen, tim_range),
                        file=sys.stderr)

            # And whether any of this port's own uplink bytes came back off
            # the air.  A frame the access point transmitted whose payload
            # source is this host is the access point flooding a
            # group-addressed frame this port sent up back out to the basic
            # service set, which cannot happen unless the uplink frame was
            # received, decrypted and forwarded.  The line is required to
            # exist and no value in it is required: a nonzero count is a
            # statement about the uplink, and a zero is not its opposite,
            # because an access point that does not flood back to the set it
            # received from reads zero as well.
            resident_echo_result = re.search(
                rb"K1 Wi-Fi GPL: resident window echo frames=(?:0x)?"
                rb"([0-9a-fA-F]+) group=(?:0x)?([0-9a-fA-F]+)"
                rb" data=(?:0x)?([0-9a-fA-F]+)"
                rb" fc=(?:0x)?([0-9a-fA-F]+)"
                rb" len=(?:0x)?([0-9a-fA-F]+)"
                rb" head=([0-9a-fA-F]*)",
                resident,
            )
            if resident_echo_result is None:
                missing.append(
                    "RTL8852BS2 count of this port's own uplink frames the "
                    "access point sent back out")
            else:
                echo_frames = int(resident_echo_result.group(1), 16)
                echo_group = int(resident_echo_result.group(2), 16)
                echo_data = int(resident_echo_result.group(3), 16)
                echo_fc = int(resident_echo_result.group(4), 16)
                echo_len = int(resident_echo_result.group(5), 16)
                echo_head = resident_echo_result.group(6).decode()
                print(
                    "[serial] uplink echo: frames={0} group={1} data={2} "
                    "fc=0x{3:04x} len={4} head={5}"
                    .format(echo_frames, echo_group, echo_data, echo_fc,
                            echo_len, echo_head or "(none)"),
                    file=sys.stderr)
                if echo_frames:
                    print(
                        "[serial] the access point transmitted {0} frame(s) "
                        "carrying this station's own address as the source of "
                        "the payload inside them: the uplink was received, "
                        "decrypted and forwarded"
                        .format(echo_frames),
                        file=sys.stderr)
                else:
                    print(
                        "[serial] no frame the access point transmitted "
                        "carried this station's own address as the source of "
                        "the payload, so nothing here says the uplink got "
                        "through; an access point that does not flood a "
                        "group-addressed frame back to the set it came from "
                        "reads the same, so this is not the opposite "
                        "statement",
                        file=sys.stderr)

            # The round trip taken as a rate.  run 63 received the first ARP
            # reply this port has ever had, and one reply cannot say whether
            # the exchange repeats: three explanations for the four silent runs
            # before it were left standing, and one of them is transmit retry
            # luck, which only a repeat count measures.  Six attempts against
            # one latched host, three addressed to it and three broadcast, so
            # a rate is read and the delivery form is compared at the same
            # time.  The line is required to exist and no value in it is
            # required.
            resident_trip_result = re.search(
                rb"K1 Wi-Fi GPL: resident window arp trip attempts=(?:0x)?"
                rb"([0-9a-fA-F]+) replies=(?:0x)?([0-9a-fA-F]+)"
                rb" mask=(?:0x)?([0-9a-fA-F]+)"
                rb" uni=(?:0x)?([0-9a-fA-F]+)"
                rb" uni-ack=(?:0x)?([0-9a-fA-F]+)"
                rb" bcast=(?:0x)?([0-9a-fA-F]+)"
                rb" bcast-ack=(?:0x)?([0-9a-fA-F]+)"
                rb" peer=([0-9a-fA-F]*) ip=(?:0x)?([0-9a-fA-F]+)",
                resident,
            )
            if resident_trip_result is None:
                missing.append(
                    "RTL8852BS2 repeated protected ARP round trip against one "
                    "host")
            else:
                trip_attempts = int(resident_trip_result.group(1), 16)
                trip_replies = int(resident_trip_result.group(2), 16)
                trip_mask = int(resident_trip_result.group(3), 16)
                trip_uni = int(resident_trip_result.group(4), 16)
                trip_uni_ack = int(resident_trip_result.group(5), 16)
                trip_bcast = int(resident_trip_result.group(6), 16)
                trip_bcast_ack = int(resident_trip_result.group(7), 16)
                trip_peer = resident_trip_result.group(8).decode()
                trip_ip = int(resident_trip_result.group(9), 16)
                print(
                    "[serial] protected ARP round trip: {0}/{1} answered "
                    "(mask=0x{2:02x}), addressed {3}/{4}, broadcast {5}/{6}, "
                    "host {7} at {8}"
                    .format(trip_replies, trip_attempts, trip_mask,
                            trip_uni_ack, trip_uni, trip_bcast_ack,
                            trip_bcast, trip_peer or "(none)",
                            ".".join(str((trip_ip >> shift) & 0xff)
                                     for shift in (24, 16, 8, 0))),
                    file=sys.stderr)
                if trip_attempts and trip_replies >= trip_attempts:
                    print(
                        "[serial] every request the queue took was answered, "
                        "so the protected unicast round trip is not a one-off "
                        "and transmit retry luck does not explain the runs "
                        "that saw none",
                        file=sys.stderr)
                elif trip_replies:
                    print(
                        "[serial] {0} of {1} requests were answered, so the "
                        "round trip works and is lossy; a rate this far below "
                        "one is what transmit retry luck would look like, and "
                        "the earlier silent runs sent one or two requests"
                        .format(trip_replies, trip_attempts),
                        file=sys.stderr)
                elif trip_attempts:
                    print(
                        "[serial] none of the {0} requests was answered, so "
                        "whatever produced run 63's reply was not the request "
                        "itself; the host that answered then is not "
                        "necessarily the one asked here"
                        .format(trip_attempts),
                        file=sys.stderr)
                else:
                    print(
                        "[serial] no request was transmitted, so the window "
                        "learned no host to ask and this line says nothing "
                        "about the round trip",
                        file=sys.stderr)

            # The DHCP exchange as the server answered it.  The offer line
            # is the reservation, and it is existence-only because a run in
            # which no server answers is a reading about the network rather
            # than about this port.
            resident_offer_result = re.search(
                rb"K1 Wi-Fi GPL: resident window dhcp offer ip=(?:0x)?"
                rb"([0-9a-fA-F]+) server-id=(?:0x)?([0-9a-fA-F]+)"
                rb" src=(?:0x)?([0-9a-fA-F]+)"
                rb" mask=(?:0x)?([0-9a-fA-F]+)"
                rb" router=(?:0x)?([0-9a-fA-F]+)"
                rb" lease=(?:0x)?([0-9a-fA-F]+)"
                rb" offers=(?:0x)?([0-9a-fA-F]+)"
                rb" attempt=(?:0x)?([0-9a-fA-F]+)"
                rb" xid=(?:0x)?([0-9a-fA-F]+)"
                rb" mac=([0-9a-fA-F]*)",
                resident,
            )
            if resident_offer_result is None:
                missing.append(
                    "RTL8852BS2 DHCP offer parsed out of a protected reply")
            else:
                offer_mac = resident_offer_result.group(10).decode()
                (offer_ip, offer_server, offer_src, offer_mask, offer_router,
                 offer_lease, offer_count, offer_attempt,
                 offer_xid) = (int(group, 16) for group
                               in resident_offer_result.groups()[:9])

                def quad(value):
                    return "{0}.{1}.{2}.{3}".format(
                        (value >> 24) & 0xff, (value >> 16) & 0xff,
                        (value >> 8) & 0xff, value & 0xff)

                if offer_ip:
                    print(
                        "[serial] DHCP offer: {0} for this station, from "
                        "server {1} (datagram source {2}), mask {3}, router "
                        "{4}, lease 0x{5:x}s, {6} offer(s), Discover attempt "
                        "{7} (xid 0x{8:08x}), sent inside the BSS by {9}"
                        .format(quad(offer_ip), quad(offer_server),
                                quad(offer_src), quad(offer_mask),
                                quad(offer_router), offer_lease, offer_count,
                                offer_attempt, offer_xid,
                                offer_mac or "(unknown)"),
                        file=sys.stderr)
                else:
                    print(
                        "[serial] no DHCP offer arrived in this window, so "
                        "the acceptance below had nothing to accept",
                        file=sys.stderr)

            # And the acceptance.  sent says whether the window asked, ack
            # and nak how the server closed the exchange.
            resident_request_result = re.search(
                rb"K1 Wi-Fi GPL: resident window dhcp request sent=(?:0x)?"
                rb"([0-9a-fA-F]+) status=(?:0x)?([0-9a-fA-F]+)"
                rb" sn=(?:0x)?([0-9a-fA-F]+)"
                rb" bytes=(?:0x)?([0-9a-fA-F]+)"
                rb" reply=(?:0x)?([0-9a-fA-F]+)"
                rb" ack=(?:0x)?([0-9a-fA-F]+)"
                rb" ack-ip=(?:0x)?([0-9a-fA-F]+)"
                rb" nak=(?:0x)?([0-9a-fA-F]+)",
                resident,
            )
            if resident_request_result is None:
                missing.append(
                    "RTL8852BS2 protected DHCP Request accepting the offer")
            else:
                (request_sent, request_status, request_sn, request_bytes,
                 dhcp_replies, dhcp_acks, dhcp_ack_ip,
                 dhcp_naks) = (int(group, 16) for group
                               in resident_request_result.groups())
                print(
                    "[serial] DHCP Request: {0} sent (sn 0x{1:x}, {2} bytes, "
                    "status {3}), {4} replies seen, {5} ack, {6} nak"
                    .format(request_sent, request_sn, request_bytes,
                            request_status or "ok", dhcp_replies, dhcp_acks,
                            dhcp_naks),
                    file=sys.stderr)
                if dhcp_acks:
                    print(
                        "[serial] the server acknowledged the Request, so "
                        "0x{0:08x} is this station's address to use: the DHCP "
                        "handshake completed over the protected link and a "
                        "non-zero sender address is now available for ARP and "
                        "ICMP".format(dhcp_ack_ip),
                        file=sys.stderr)
                elif dhcp_naks:
                    print(
                        "[serial] the server refused the Request, so the "
                        "offered address was no longer free by the time it "
                        "was accepted; the exchange has to restart from a "
                        "Discover rather than be retried",
                        file=sys.stderr)
                elif request_sent:
                    print(
                        "[serial] the Request was queued but nothing came "
                        "back inside the window, so either the answer landed "
                        "after the window closed or the Request did not reach "
                        "the server -- the transmit report says which",
                        file=sys.stderr)
                else:
                    print(
                        "[serial] no Request was transmitted, so this line "
                        "says nothing about the acceptance",
                        file=sys.stderr)

            # The same host asked again with the acknowledged address in
            # the sender protocol field.  Existence-only: a window that
            # never got an address has nothing to claim with.
            resident_claim_result = re.search(
                rb"K1 Wi-Fi GPL: resident window arp claim attempts=(?:0x)?"
                rb"([0-9a-fA-F]+) acks=(?:0x)?([0-9a-fA-F]+)"
                rb" mask=(?:0x)?([0-9a-fA-F]+)"
                rb" spa=(?:0x)?([0-9a-fA-F]+)"
                rb" status=(?:0x)?([0-9a-fA-F]+)"
                rb" peer=([0-9a-fA-F]*) ip=(?:0x)?([0-9a-fA-F]+)",
                resident,
            )
            if resident_claim_result is None:
                missing.append(
                    "RTL8852BS2 ARP round from an acknowledged address")
            else:
                claim_peer = resident_claim_result.group(6).decode()
                (claim_attempts, claim_acks, claim_mask, claim_spa,
                 claim_status, claim_ip) = (
                     int(group, 16) for group in
                     resident_claim_result.groups()[:5]
                     + resident_claim_result.groups()[6:])

                def spa_quad(value):
                    return "{0}.{1}.{2}.{3}".format(
                        (value >> 24) & 0xff, (value >> 16) & 0xff,
                        (value >> 8) & 0xff, value & 0xff)

                print(
                    "[serial] claimed ARP round: {0}/{1} answered "
                    "(mask=0x{2:02x}), from {3} to {4} at {5}, status {6}"
                    .format(claim_acks, claim_attempts, claim_mask,
                            spa_quad(claim_spa), claim_peer or "(none)",
                            spa_quad(claim_ip), claim_status or "ok"),
                    file=sys.stderr)
                if claim_acks:
                    print(
                        "[serial] the host answered a request that came from "
                        "this station's own address after ignoring six that "
                        "came from nobody, so the zero sender protocol "
                        "address was the reason the probe round was silent "
                        "and the reply came back over the unicast downlink",
                        file=sys.stderr)
                elif claim_attempts:
                    print(
                        "[serial] the acknowledged sender address changed "
                        "nothing: the same host, key, window and peer went "
                        "unanswered in both forms, so what is left is the "
                        "direction the answer has to take -- a frame "
                        "addressed to this station, which this window only "
                        "ever received as broadcast",
                        file=sys.stderr)
                else:
                    print(
                        "[serial] no claimed request was transmitted, so no "
                        "address was acknowledged inside the window and this "
                        "line says nothing about the round trip",
                        file=sys.stderr)

            # Whether anything asked for this station's own address, and
            # whether it was answered.  run 66 heard thirty-two ARP requests
            # in one window and answered none of them, at least one of which
            # was asking who holds the address the server had just
            # acknowledged -- and RFC 826 makes that answer the precondition
            # for any peer sending this station a unicast datagram at all.
            resident_serve_result = re.search(
                rb"K1 Wi-Fi GPL: resident window arp serve requests=(?:0x)?"
                rb"([0-9a-fA-F]+) sent=(?:0x)?([0-9a-fA-F]+)"
                rb" bytes=(?:0x)?([0-9a-fA-F]+)"
                rb" peer-ip=(?:0x)?([0-9a-fA-F]+)"
                rb" status=(?:0x)?([0-9a-fA-F]+)"
                rb" mac=([0-9a-fA-F]*)",
                resident,
            )
            if resident_serve_result is None:
                missing.append(
                    "RTL8852BS2 ARP answers for this station's own address")
            else:
                serve_mac = resident_serve_result.group(6).decode()
                (serve_requests, serve_sent, serve_bytes, serve_peer_ip,
                 serve_status) = (
                     int(group, 16) for group in
                     resident_serve_result.groups()[:5])

                def serve_quad(value):
                    return "{0}.{1}.{2}.{3}".format(
                        (value >> 24) & 0xff, (value >> 16) & 0xff,
                        (value >> 8) & 0xff, value & 0xff)

                print(
                    "[serial] ARP requests for this station: {0} asked, {1} "
                    "answered ({2} bytes), last asker {3} at {4}, status {5}"
                    .format(serve_requests, serve_sent, serve_bytes,
                            serve_mac or "(none)",
                            serve_quad(serve_peer_ip), serve_status or "ok"),
                    file=sys.stderr)
                if serve_sent:
                    print(
                        "[serial] this port answered an ARP request for its "
                        "own address for the first time, so a peer that had "
                        "no entry for this station can now address a unicast "
                        "datagram to it",
                        file=sys.stderr)
                elif serve_requests:
                    print(
                        "[serial] somebody asked for this station's address "
                        "and no answer went out, so the peer's neighbour "
                        "entry stays empty and nothing it wants to send here "
                        "can leave it -- the status above says whether the "
                        "frame was built or the queue refused it",
                        file=sys.stderr)
                else:
                    print(
                        "[serial] nobody asked for this station's address "
                        "inside the window, so this line says nothing about "
                        "whether the answer works",
                        file=sys.stderr)

            # And the first round trip above ARP.  An ARP reply proves two
            # stations reach each other and that the pairwise key works both
            # ways; it says nothing about the address itself, because ARP
            # carries it as an opaque field and any host owning it answers.
            # An echo reply cannot be produced without a peer accepting a
            # datagram addressed to this station and routing one back, so it
            # is the first evidence the acknowledged address is usable.  The
            # attempts alternate between two hosts and the echo sequence
            # number comes back inside the reply, so the mask says which of
            # the two answered: even bits the DHCP server, odd bits the host
            # that answered the claimed ARP round.
            resident_echo_result = re.search(
                rb"K1 Wi-Fi GPL: resident window icmp echo attempts=(?:0x)?"
                rb"([0-9a-fA-F]+) mask=(?:0x)?([0-9a-fA-F]+)"
                rb" replies=(?:0x)?([0-9a-fA-F]+)"
                rb" bad=(?:0x)?([0-9a-fA-F]+)"
                rb" target=(?:0x)?([0-9a-fA-F]+)"
                rb" alt=(?:0x)?([0-9a-fA-F]+)"
                rb" src=(?:0x)?([0-9a-fA-F]+)"
                rb" id=(?:0x)?([0-9a-fA-F]+)"
                rb" seq=(?:0x)?([0-9a-fA-F]+)"
                rb" sn=(?:0x)?([0-9a-fA-F]+)"
                rb" bytes=(?:0x)?([0-9a-fA-F]+)"
                rb" status=(?:0x)?([0-9a-fA-F]+)"
                rb" a3=([0-9a-fA-F]*)",
                resident,
            )
            if resident_echo_result is None:
                missing.append(
                    "RTL8852BS2 ICMP echo round from the acknowledged address")
            else:
                echo_a3 = resident_echo_result.group(13).decode()
                (echo_attempts, echo_mask, echo_replies, echo_bad,
                 echo_target, echo_alt, echo_src, echo_id, echo_seq,
                 echo_sn, echo_bytes, echo_status) = (
                     int(group, 16) for group in
                     resident_echo_result.groups()[:12])

                def echo_quad(value):
                    return "{0}.{1}.{2}.{3}".format(
                        (value >> 24) & 0xff, (value >> 16) & 0xff,
                        (value >> 8) & 0xff, value & 0xff)

                echo_even = echo_mask & 0x55555555
                echo_odd = echo_mask & 0xaaaaaaaa

                # The two candidates are the pair the last attempt used, and
                # they are equal whenever only one host was ever known.  The
                # parity of the mask separates the attempts either way, but it
                # separates the hosts only when the pair is really two.
                echo_two_hosts = echo_target != echo_alt
                print(
                    "[serial] ICMP echo round: {0}/{1} answered "
                    "(mask=0x{2:02x}, {3} damaged), {4} and {5} asked, last "
                    "attempt addressed to {6}, {7} bytes, sn=0x{8:x}, "
                    "status {9}"
                    .format(echo_replies, echo_attempts, echo_mask, echo_bad,
                            echo_quad(echo_target), echo_quad(echo_alt),
                            echo_a3 or "(none)", echo_bytes, echo_sn,
                            echo_status or "ok"),
                    file=sys.stderr)
                if echo_replies:
                    print(
                        "[serial] an echo reply came back from {0} carrying "
                        "identifier 0x{1:04x} and sequence {2}, so this port "
                        "completed its first IP round trip: a peer accepted a "
                        "datagram addressed from the address the server "
                        "acknowledged and routed one back to it"
                        .format(echo_quad(echo_src), echo_id, echo_seq),
                        file=sys.stderr)
                    if not echo_two_hosts:
                        print(
                            "[serial] both candidates are the same host "
                            "({0}), so the parity of the mask says how many "
                            "attempts were answered and nothing about which "
                            "host: the one outcome two targets exist to "
                            "separate -- a peer that drops echoes -- is "
                            "untested in this window"
                            .format(echo_quad(echo_target)),
                            file=sys.stderr)
                    elif echo_even and echo_odd:
                        print(
                            "[serial] both hosts answered, so the round trip "
                            "is not a property of one peer",
                            file=sys.stderr)
                    elif echo_even:
                        print(
                            "[serial] only the DHCP server answered, which is "
                            "the expected asymmetry: a router answers echoes "
                            "and an arbitrary station often drops them",
                            file=sys.stderr)
                    elif echo_odd:
                        print(
                            "[serial] only the host that answered the claimed "
                            "ARP round answered, so the server that handed "
                            "out the address does not answer echoes",
                            file=sys.stderr)
                elif echo_bad:
                    print(
                        "[serial] every answer carried this port's identifier "
                        "and failed its own checksum, so the round trip "
                        "happened and the receive path handed up a damaged "
                        "payload -- that is a receive-side finding, not a "
                        "silent peer",
                        file=sys.stderr)
                elif echo_attempts:
                    print(
                        "[serial] neither host answered the echo, so what is "
                        "left is either the datagram this port builds -- the "
                        "two checksums, the header, the address it claims -- "
                        "or an access point that does not forward an IPv4 "
                        "unicast between stations, and the ARP rounds above "
                        "already rule the key and the peer out",
                        file=sys.stderr)
                else:
                    print(
                        "[serial] no echo request was transmitted, so either "
                        "no address was acknowledged or no host was known to "
                        "ask, and this line says nothing about the round trip",
                        file=sys.stderr)

            # And the direction 4a left open: whether a ping addressed to
            # this station is answered.  Everything above is this port
            # asking -- the ARP responder included, because it answers a
            # question about an address rather than a datagram sent to it.
            # This line is the first place the port owes a reply to an IP
            # datagram somebody else chose to send, and it is the one part of
            # the port a person can check without reading a log: ping the
            # acknowledged address from any host on the subnet during the
            # window.
            #
            # bad and long are refusals, not failures.  RFC 792 requires the
            # request's identifier, sequence and data back unchanged, so a
            # request whose own checksum failed is not answered and one
            # carrying more data than the reply can hold is not answered
            # short -- either answer would report a round trip that did not
            # happen, which is worse for the person reading the ping output
            # than silence.
            resident_icmp_serve_result = re.search(
                rb"K1 Wi-Fi GPL: resident window icmp serve requests=(?:0x)?"
                rb"([0-9a-fA-F]+) sent=(?:0x)?([0-9a-fA-F]+)"
                rb" bad=(?:0x)?([0-9a-fA-F]+)"
                rb" long=(?:0x)?([0-9a-fA-F]+)"
                rb" bytes=(?:0x)?([0-9a-fA-F]+)"
                rb" peer-ip=(?:0x)?([0-9a-fA-F]+)"
                rb" id=(?:0x)?([0-9a-fA-F]+)"
                rb" seq=(?:0x)?([0-9a-fA-F]+)"
                rb" data=(?:0x)?([0-9a-fA-F]+)"
                rb" status=(?:0x)?([0-9a-fA-F]+)"
                rb" mac=([0-9a-fA-F]*)",
                resident,
            )
            if resident_icmp_serve_result is None:
                missing.append(
                    "RTL8852BS2 ICMP echo answers for this station's own "
                    "address")
            else:
                icmp_serve_mac = resident_icmp_serve_result.group(11).decode()
                (icmp_serve_requests, icmp_serve_sent, icmp_serve_bad,
                 icmp_serve_long, icmp_serve_bytes, icmp_serve_peer_ip,
                 icmp_serve_id, icmp_serve_seq, icmp_serve_data,
                 icmp_serve_status) = (
                     int(group, 16) for group in
                     resident_icmp_serve_result.groups()[:10])

                def icmp_serve_quad(value):
                    return "{0}.{1}.{2}.{3}".format(
                        (value >> 24) & 0xff, (value >> 16) & 0xff,
                        (value >> 8) & 0xff, value & 0xff)

                print(
                    "[serial] pings addressed to this station: {0} asked, {1} "
                    "answered ({2} bytes), {3} refused for a checksum, {4} "
                    "for a length, last asker {5} at {6} "
                    "(id=0x{7:04x} seq={8} data={9}), status {10}"
                    .format(icmp_serve_requests, icmp_serve_sent,
                            icmp_serve_bytes, icmp_serve_bad, icmp_serve_long,
                            icmp_serve_mac or "(none)",
                            icmp_serve_quad(icmp_serve_peer_ip),
                            icmp_serve_id, icmp_serve_seq, icmp_serve_data,
                            icmp_serve_status or "ok"),
                    file=sys.stderr)
                if icmp_serve_sent:
                    print(
                        "[serial] this port answered a ping for its own "
                        "address, so the acknowledged address is reachable "
                        "from outside and not merely usable from inside: a "
                        "host that chose to send a datagram here got one back",
                        file=sys.stderr)
                elif icmp_serve_requests:
                    print(
                        "[serial] a ping arrived for this station and no "
                        "answer went out, so the address is reachable inbound "
                        "and the reply is what failed -- the status above "
                        "says whether the frame was built or the queue "
                        "refused it",
                        file=sys.stderr)
                elif icmp_serve_bad or icmp_serve_long:
                    print(
                        "[serial] every ping for this station was refused "
                        "before an answer was owed, so nothing here is a "
                        "transmit finding: a broken checksum is a "
                        "receive-path statement and an over-long request is "
                        "the asking host's choice of payload size",
                        file=sys.stderr)
                else:
                    print(
                        "[serial] nobody pinged this station inside the "
                        "window, so this line says nothing about whether the "
                        "answer works -- run it again with a ping to the "
                        "acknowledged address from a host on the same subnet",
                        file=sys.stderr)

            # Whether the receive filter was widened for the length of the
            # window, so a frame whose payload the security engine could not
            # verify is handed up instead of being dropped inside the receive
            # MAC.  status is the errno the write-back check returned, and a
            # nonzero one means every count below was taken under the narrow
            # filter every earlier window ran.
            resident_err_filter_result = re.search(
                rb"K1 Wi-Fi GPL: resident window rx-err-filter before="
                rb"(?:0x)?([0-9a-fA-F]+) after=(?:0x)?([0-9a-fA-F]+)"
                rb" status=(?:0x)?([0-9a-fA-F]+)",
                resident,
            )
            if resident_err_filter_result is None:
                missing.append(
                    "RTL8852BS2 resident window error-packet receive filter")
            else:
                err_before = int(resident_err_filter_result.group(1), 16)
                err_after = int(resident_err_filter_result.group(2), 16)
                err_status = int(resident_err_filter_result.group(3), 16)
                print(
                    "[serial] resident receive filter: before=0x{0:08x} "
                    "after=0x{1:08x} status={2}"
                    .format(err_before, err_after, err_status),
                    file=sys.stderr)
                if err_status:
                    print(
                        "[serial] the error-packet admission bit could not be "
                        "set, so a frame whose payload the security engine "
                        "rejected was still dropped inside the receive MAC "
                        "and nothing below rules that out",
                        file=sys.stderr)

            # The same question the receive descriptor's own A1_MATCH bit
            # answers, asked in software.  self counts frames whose first
            # address is this station's MAC by comparison; bssid and other are
            # the two remaining unicast destinations, and they are here so
            # that a self of zero is read next to how much unicast traffic the
            # window heard from anybody.  hw-a1 repeats the descriptor bit:
            # the two disagreeing is a statement about the filter this window
            # runs under -- sniffer mode with the unicast address-CAM match
            # bit clear -- and not about the access point.  The line is
            # required to exist and no value in it is required.
            resident_a1_result = re.search(
                rb"K1 Wi-Fi GPL: resident window data a1 self=(?:0x)?"
                rb"([0-9a-fA-F]+) bssid=(?:0x)?([0-9a-fA-F]+)"
                rb" other=(?:0x)?([0-9a-fA-F]+)"
                rb" self-prot=(?:0x)?([0-9a-fA-F]+)"
                rb" self-dec=(?:0x)?([0-9a-fA-F]+)"
                rb" hw-a1=(?:0x)?([0-9a-fA-F]+)",
                resident,
            )
            if resident_a1_result is None:
                missing.append(
                    "RTL8852BS2 software count of received frames addressed "
                    "to this station")
            else:
                a1_self = int(resident_a1_result.group(1), 16)
                a1_bssid = int(resident_a1_result.group(2), 16)
                a1_other = int(resident_a1_result.group(3), 16)
                a1_self_prot = int(resident_a1_result.group(4), 16)
                a1_self_dec = int(resident_a1_result.group(5), 16)
                a1_hw = int(resident_a1_result.group(6), 16)
                print(
                    "[serial] unicast destinations heard: self={0} "
                    "(protected={1} decrypted={2}) to-bssid={3} "
                    "to-others={4} descriptor-a1-match={5}"
                    .format(a1_self, a1_self_prot, a1_self_dec, a1_bssid,
                            a1_other, a1_hw),
                    file=sys.stderr)
                if a1_self and not a1_hw:
                    print(
                        "[serial] {0} frame(s) carrying this station's own "
                        "address arrived while the descriptor's match bit "
                        "stayed clear: the downlink exists, and the bit every "
                        "earlier window read it through does not report it"
                        .format(a1_self),
                        file=sys.stderr)
                elif a1_self:
                    print(
                        "[serial] {0} frame(s) addressed to this station "
                        "arrived and the descriptor agrees, so the downlink "
                        "exists and what is left is what became of the "
                        "payload"
                        .format(a1_self),
                        file=sys.stderr)
                elif a1_bssid or a1_other:
                    print(
                        "[serial] no frame in the window carried this "
                        "station's address while {0} unicast frame(s) to "
                        "other destinations did, so the receiver was taking "
                        "unicast traffic in and none of it was addressed here"
                        .format(a1_bssid + a1_other),
                        file=sys.stderr)
                else:
                    print(
                        "[serial] the window heard no unicast frame at all, "
                        "so nothing here separates an access point that sent "
                        "none from a receiver that admitted none",
                        file=sys.stderr)

            # And the first of those frames, because a count of one says less
            # than the frame it counted.  flags is bit 0 hardware decrypted,
            # bit 1 handed to software, bit 2 integrity check failed, bit 3
            # CRC failed, bit 4 the descriptor's own A1_MATCH and bit 5 a long
            # descriptor.  A length of zero is the no-such-frame case and the
            # remaining fields are then empty by construction.
            resident_a1_first_result = re.search(
                rb"K1 Wi-Fi GPL: resident window data a1 first fc=(?:0x)?"
                rb"([0-9a-fA-F]+) len=(?:0x)?([0-9a-fA-F]+)"
                rb" flags=(?:0x)?([0-9a-fA-F]+)"
                rb" sec=(?:0x)?([0-9a-fA-F]+)"
                rb" a2=([0-9a-fA-F]*) a3=([0-9a-fA-F]*)"
                rb" head=([0-9a-fA-F]*)",
                resident,
            )
            if resident_a1_first_result is None:
                missing.append(
                    "RTL8852BS2 first received frame addressed to this "
                    "station")
            elif int(resident_a1_first_result.group(2), 16):
                a1_first_fc = int(resident_a1_first_result.group(1), 16)
                a1_first_len = int(resident_a1_first_result.group(2), 16)
                a1_first_flags = int(resident_a1_first_result.group(3), 16)
                a1_first_sec = int(resident_a1_first_result.group(4), 16)
                print(
                    "[serial] first frame addressed to this station: "
                    "fc=0x{0:04x} len={1} flags=0x{2:02x} sec=0x{3:02x} "
                    "a2={4} a3={5} head={6}"
                    .format(a1_first_fc, a1_first_len, a1_first_flags,
                            a1_first_sec,
                            resident_a1_first_result.group(5).decode() or
                            "(none)",
                            resident_a1_first_result.group(6).decode() or
                            "(none)",
                            resident_a1_first_result.group(7).decode() or
                            "(none)"),
                    file=sys.stderr)

            # Where the receive MAC lost a frame if it lost one at all.  The
            # stage counters are sampled on entry and again at the end, and
            # the exit line carries the deltas across the window.  A filter
            # drop delta of zero in a window that reports nothing addressed to
            # this station is a window in which the filter dropped nothing
            # either, which moves the missing answer off this host.
            resident_stage_result = re.search(
                rb"K1 Wi-Fi GPL: scan PHY stage resident-exit "
                rb"recca=[^\r\n]* delta-recca=(?:0x)?([0-9a-fA-F]+)"
                rb" delta-rxdma=(?:0x)?([0-9a-fA-F]+)"
                rb" delta-pktfltr-drp=(?:0x)?([0-9a-fA-F]+)",
                resident,
            )
            if resident_stage_result is None:
                missing.append(
                    "RTL8852BS2 receive MAC stage counters across the "
                    "resident window")
            else:
                stage_recca = int(resident_stage_result.group(1), 16)
                stage_rxdma = int(resident_stage_result.group(2), 16)
                stage_drop = int(resident_stage_result.group(3), 16)
                print(
                    "[serial] receive MAC across the window: recca+{0} "
                    "rxdma+{1} filter-drop+{2}"
                    .format(stage_recca, stage_rxdma, stage_drop),
                    file=sys.stderr)
                if stage_drop:
                    print(
                        "[serial] the receive filter rejected {0} frame(s) "
                        "inside the window, which is where a frame addressed "
                        "to this station would have gone if one was sent"
                        .format(stage_drop),
                        file=sys.stderr)
                else:
                    print(
                        "[serial] the receive filter rejected nothing inside "
                        "the window, so a frame addressed to this station was "
                        "not dropped by it",
                        file=sys.stderr)

            # The transmit half.  The status of the Probe Request separates a
            # frame the hardware accepted from one the builder or the queue
            # refused, and only a Probe Response carrying this host's own
            # address counts as its answer.
            resident_probe_tx_result = re.search(
                rb"K1 Wi-Fi GPL: resident probe tx sn=[^\r\n]*"
                rb" status=0x0+(?![0-9a-fA-F])",
                resident,
            )
            if resident_probe_tx_result is None:
                missing.append(
                    "RTL8852BS2 directed Probe Request transmit outside a "
                    "dwell")

            resident_probe_rsp_result = re.search(
                rb"K1 Wi-Fi GPL: resident window channel=[^\r\n]*"
                rb" probe-rsp-self=(?:0x)?0*[1-9a-fA-F]",
                resident,
            )
            if resident_probe_rsp_result is None:
                missing.append(
                    "RTL8852BS2 Probe Response to this host outside a dwell")

            # What makes the two above evidence about the receive and transmit
            # paths rather than about the radio: every sampled channel and
            # bandwidth register held still, the receive loop hit no error, and
            # the three receive filters were restored and read back.
            resident_stable_result = re.search(
                rb"K1 Wi-Fi GPL: resident window channel=[^\r\n]*"
                rb" ch-stable=0x0*1(?![0-9a-fA-F]) rx=0x0+(?![0-9a-fA-F])"
                rb" filter=0x0+(?![0-9a-fA-F])",
                resident,
            )
            if resident_stable_result is None:
                missing.append(
                    "RTL8852BS2 channel registers unchanged across the "
                    "resident window")

            resident_end_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 station resident window "
                rb"complete\r?\n",
                resident,
            )
            if resident_end_result is None:
                missing.append("RTL8852BS2 resident window completion")
        if args.require_runtime_data_secure_tx:
            # The memory-only half first: the DHCP Discover body, both
            # IPv4/UDP checksums, the data-queue descriptor and the reply
            # matcher, all against values a python model produced offline.  A
            # failure here says the frame would have been wrong on the air.
            data_secure_model_result = re.search(
                rb"K1 Wi-Fi GPL: RTL8852BS2 runtime protected data TX "
                rb"complete\r?\n",
                started,
            )
            if data_secure_model_result is None:
                missing.append("RTL8852BS2 protected data TX frame model")

            # The on-air half, sliced from the resident window's entry line so
            # nothing printed before the association can satisfy it.  status
            # is what the queue returned for the last attempt, and a zero
            # there with the frame built from a live pairwise key is a frame
            # the hardware took to encrypt itself.
            data_secure_begin_result = re.search(
                rb"K1 Wi-Fi GPL: resident window enter channel="
                rb"(?:0x)?0*[1-9a-fA-F][0-9a-fA-F]*",
                started,
            )
            if data_secure_begin_result is None:
                missing.append(
                    "RTL8852BS2 resident window start before a protected "
                    "data transmit")
                data_secure = started
            else:
                data_secure = started[data_secure_begin_result.end():]

            data_secure_tx_result = re.search(
                rb"K1 Wi-Fi GPL: resident data tx sn=[^\r\n]*"
                rb" status=0x0+(?![0-9a-fA-F])",
                data_secure,
            )
            if data_secure_tx_result is None:
                missing.append(
                    "RTL8852BS2 protected data frame accepted by a data queue")

            # And the window's own read-back of it, which is what says the
            # frame counted above was transmitted with a key installed rather
            # than skipped.  The DHCP reply on the same line is reported by
            # the firmware and deliberately not required here: an access point
            # with no DHCP server behind it would decrypt this frame perfectly
            # well and still never answer.
            data_secure_report_result = re.search(
                rb"K1 Wi-Fi GPL: resident window data tx "
                rb"sent=(?:0x)?0*[1-9a-fA-F][0-9a-fA-F]*"
                rb"[^\r\n]* status=0x0+(?![0-9a-fA-F])"
                rb" tk=(?:0x)?0*1(?![0-9a-fA-F])",
                data_secure,
            )
            if data_secure_report_result is None:
                missing.append(
                    "RTL8852BS2 resident window protected data transmit "
                    "summary")

            # Whether the MAC transmitted the frame the queue accepted is a
            # third question, and neither line above can answer it: status is
            # the queue's answer and sent counts writes.  The counters that do
            # answer it are only read if this dump ran, so the dump is required
            # to exist.  What it says is printed rather than required, for the
            # same reason the DHCP reply is: a zero transmitted-MPDU delta is
            # the reading this instrument was added to be able to see, not a
            # reason to fail the run that produced it.
            data_secure_state_result = re.search(
                rb"K1 Wi-Fi GPL: TX state data-after "
                rb"[^\r\n]* delta-mactx-mpdu=(?:0x)?[0-9a-fA-F]+"
                rb"[^\r\n]*\r?\n",
                data_secure,
            )
            if data_secure_state_result is None:
                missing.append(
                    "RTL8852BS2 transmit-counter sample across a protected "
                    "data write")

            data_secure_counters = re.search(
                rb"K1 Wi-Fi GPL: resident window data tx [^\r\n]*"
                rb" mpdu=(?:0x)?([0-9a-fA-F]+)"
                rb" cck=(?:0x)?([0-9a-fA-F]+)"
                rb" block=(?:0x)?([0-9a-fA-F]+)",
                data_secure,
            )
            if data_secure_counters is not None:
                print(
                    "[serial] protected data transmit counters: mpdu="
                    f"{int(data_secure_counters.group(1), 16)} cck="
                    f"{int(data_secure_counters.group(2), 16)} block="
                    f"{int(data_secure_counters.group(3), 16)}",
                    file=sys.stderr)

            # And the fourth question, which is the one the counters still
            # cannot answer: whether anything acknowledged the frame the MAC
            # emitted.  The hardware answers it in a transmit report, and the
            # summary prints what the window collected unconditionally, so the
            # fields being present is what says this instrument ran.  Their
            # values are printed and not required for the same reason as the
            # counters above: a report that says the retries ran out is the
            # reading this was added to be able to see.
            data_secure_txrpt_result = re.search(
                rb"K1 Wi-Fi GPL: resident window data tx [^\r\n]*"
                rb" txrpt=(?:0x)?([0-9a-fA-F]+)"
                rb" txrpt-self=(?:0x)?([0-9a-fA-F]+)"
                rb" txrpt-ok=(?:0x)?([0-9a-fA-F]+)"
                rb" txrpt-fail=(?:0x)?([0-9a-fA-F]+)"
                rb" txrpt-dat=(?:0x)?([0-9a-fA-F]+)"
                rb" txrpt-dat-ok=(?:0x)?([0-9a-fA-F]+)"
                rb" txrpt-dat-fail=(?:0x)?([0-9a-fA-F]+)"
                rb" txrpt-short=(?:0x)?([0-9a-fA-F]+)"
                rb" c2h=(?:0x)?([0-9a-fA-F]+)"
                rb" ccxrpt=(?:0x)?([0-9a-fA-F]+)"
                rb" ccxrpt-short=(?:0x)?([0-9a-fA-F]+)"
                rb" ccxrpt-tag=(?:0x)?([0-9a-fA-F]+)"
                rb" ccxrpt-tag-ok=(?:0x)?([0-9a-fA-F]+)"
                rb" ccxrpt-tag-fail=(?:0x)?([0-9a-fA-F]+)",
                data_secure,
            )
            if data_secure_txrpt_result is None:
                missing.append(
                    "RTL8852BS2 transmit reports collected across a "
                    "protected data write")
            else:
                values = [int(group, 16)
                          for group in data_secure_txrpt_result.groups()]
                print(
                    "[serial] transmit reports: total={0} self={1} ok={2} "
                    "fail={3} data={4} data-ok={5} data-fail={6} "
                    "short={7}".format(*values),
                    file=sys.stderr)
                # And the same report as the firmware delivers it.  The
                # descriptor now carries the special-report request and a
                # four-bit tag, and because the report path stays pointed at
                # the firmware processor the answer comes back as a C2H, not
                # as a receive packet.  c2h is every firmware message the
                # window saw, so a zero ccxrpt with a non-zero c2h means the
                # firmware was talking and did not report, while both zero
                # means the drain saw no firmware message at all.
                print(
                    "[serial] firmware transmit reports: c2h={8} "
                    "ccxrpt={9} short={10} tagged={11} tag-ok={12} "
                    "tag-fail={13}".format(*values),
                    file=sys.stderr)

            # The A/B the window runs on the one descriptor field whose value
            # this port cannot derive from the vendor tree: the first attempt
            # carries the vendor's header-with-LLC length, twenty half-bytes
            # over the MAC, LLC/SNAP and cipher headers, and the second
            # carries mainline's twelve over the MAC header alone.  Each has
            # its own transaction identifier and its own transmit-report tag,
            # so offer-attempt names the form the access point answered and
            # tag-seen says which forms the hardware reported on at all.
            #
            # Reported and never required.  A window that is answered on its
            # first attempt never transmits the second, so requiring both
            # would fail exactly the run that succeeded, and a window that is
            # answered on neither is the reading this experiment exists to
            # produce.
            data_secure_ab = re.search(
                rb"K1 Wi-Fi GPL: resident window data tx [^\r\n]*"
                rb" hdr-llc=(?:0x)?([0-9a-fA-F]+)"
                rb"[^\r\n]* xid=(?:0x)?([0-9a-fA-F]+)"
                rb"[^\r\n]* xid2=(?:0x)?([0-9a-fA-F]+)"
                rb" offer-attempt=(?:0x)?([0-9a-fA-F]+)"
                rb" ccxrpt-tag-seen=(?:0x)?([0-9a-fA-F]+)",
                data_secure,
            )
            if data_secure_ab is not None:
                hdr_llc, xid, xid_alt, attempt, tag_seen = (
                    int(group, 16) for group in data_secure_ab.groups())
                answered = {
                    0: "neither form was answered",
                    1: "the vendor's twenty half-bytes were answered",
                    2: "mainline's twelve half-bytes were answered",
                }.get(attempt, f"attempt {attempt} was answered")
                print(
                    "[serial] header-with-LLC A/B: last-written="
                    f"{hdr_llc} half-bytes xid=0x{xid:08x} "
                    f"xid2=0x{xid_alt:08x} reported-tags=0x{tag_seen:x}: "
                    f"{answered}",
                    file=sys.stderr)

            # The decoded reports themselves.  The first one belongs to a
            # management frame the access point answered inside the same
            # window, so a transmit state that is not zero there says the
            # field map is wrong rather than that a transmission failed, and
            # that distinction is worth seeing before the data report is read.
            #
            # The dwell report is searched over the whole boot rather than the
            # resident window, because run 47 collected zero reports in the
            # resident window while the scan dwells that transmitted
            # management frames collected 30, 6, 28 and 1 of them.  Until that
            # asymmetry is explained the dwell report is the only place the
            # field map can be read against real hardware output, so it is
            # printed whenever the image produced one.
            for phase in (b"dwell", b"resident-first", b"resident-data",
                          b"resident-c2h", b"resident-c2h-tag"):
                decoded = re.search(
                    rb"K1 Wi-Fi GPL: txrpt " + phase +
                    rb" (sel=[^\r\n]*)",
                    started if phase == b"dwell" else data_secure,
                )
                if decoded is not None:
                    print(
                        "[serial] transmit report "
                        f"{phase.decode()}: "
                        f"{decoded.group(1).decode('ascii', 'replace')}",
                        file=sys.stderr)
        if args.require_runtime_arp_probe:
            # A protected ARP request asks a different question than the DHCP
            # Discover does, and it is the question left after the descriptor
            # was cleared field by field: whether the hardware really encrypted
            # the frame it transmitted.  Twenty-eight bytes, no checksum
            # anywhere in them, no server needed -- any host's kernel answers
            # one -- and the answer comes back unicast to this host, which is
            # also the first exercise of the pairwise receive path.
            #
            # The observer that reads those answers is required to agree with
            # its offline model first.  It runs before the window's clock and
            # its numbers are fixed, so unlike everything else in the window
            # this one requirement cannot be affected by the air.
            #
            # Increment 4a took the model from six payloads to ten and added
            # an eleventh stage that checks the echo request this port builds.
            # 4b takes it to twelve payloads and fifteen stages: a ping
            # addressed to this station, the same ping with its own checksum
            # broken, one carrying more data than a reply can return, and the
            # reply this port builds for the first of the three.  The totals
            # below are the twelve-payload totals, and the echo-serve fields
            # are what the three new payloads did -- one request accepted, one
            # refused for its checksum, one refused for its length, with the
            # accepted one's identifier, sequence and data length unchanged by
            # either refusal.  stage=0 status=0 is the fifteenth stage's
            # verdict as well: it asserts internally and a mismatch there
            # lands here as a nonzero status.
            arp_model_result = re.search(
                rb"K1 Wi-Fi GPL: resident network selftest net=(?:0x)?0*c"
                rb" arp=(?:0x)?0*4 ipv4=(?:0x)?0*7 other=(?:0x)?0*1"
                rb" other-type=(?:0x)?0*86dd arp-req=(?:0x)?0*2"
                rb" replies=(?:0x)?0*1 reply-ip=(?:0x)?0*c0a80109"
                rb" peer-ip=(?:0x)?0*c0a80101 peer-tpa=(?:0x)?0*c0a8017b"
                rb" ip-peer-ip=(?:0x)?0*c0a80105 serve=(?:0x)?0*1"
                rb" serve-ip=(?:0x)?0*c0a8010a echo=(?:0x)?0*1"
                rb" echo-bad=(?:0x)?0*1 echo-mask=(?:0x)?0*4"
                rb" echo-seq=(?:0x)?0*2 echo-serve=(?:0x)?0*1"
                rb" echo-serve-id=(?:0x)?0*1234"
                rb" echo-serve-seq=(?:0x)?0*7"
                rb" echo-serve-data=(?:0x)?0*28"
                rb" echo-serve-bad=(?:0x)?0*1"
                rb" echo-serve-long=(?:0x)?0*1 stage=0x0*"
                rb"(?![0-9a-fA-F]) status=0x0+(?![0-9a-fA-F])",
                started,
            )
            if arp_model_result is None:
                missing.append(
                    "RTL8852BS2 decrypted-payload observer model")

            arp_begin_result = re.search(
                rb"K1 Wi-Fi GPL: resident window enter channel="
                rb"(?:0x)?0*[1-9a-fA-F][0-9a-fA-F]*",
                started,
            )
            arp_window = (started if arp_begin_result is None
                          else started[arp_begin_result.end():])

            # And the window's own summary, required to be present and not to
            # carry any particular value.  status is EADDRNOTAVAIL when the
            # window heard no network payload to learn a station from, which is
            # a statement about the traffic on that network and not about this
            # port, and replies is zero on exactly the run this experiment
            # exists to produce.
            arp_summary_result = re.search(
                rb"K1 Wi-Fi GPL: resident window arp "
                rb"sent=(?:0x)?([0-9a-fA-F]+)"
                rb" bytes=(?:0x)?([0-9a-fA-F]+)"
                rb"[^\r\n]* tpa=(?:0x)?([0-9a-fA-F]+)"
                rb" status=(?:0x)?([0-9a-fA-F]+)"
                rb" replies=(?:0x)?([0-9a-fA-F]+)"
                rb" reply-ip=(?:0x)?([0-9a-fA-F]+)"
                rb" net=(?:0x)?([0-9a-fA-F]+)"
                rb" arp=(?:0x)?([0-9a-fA-F]+)"
                rb" ipv4=(?:0x)?([0-9a-fA-F]+)"
                rb" other=(?:0x)?([0-9a-fA-F]+)"
                rb" other-type=(?:0x)?([0-9a-fA-F]+)"
                rb" arp-req=(?:0x)?([0-9a-fA-F]+)"
                rb" peer-ip=(?:0x)?([0-9a-fA-F]+)"
                rb" peer-tpa=(?:0x)?([0-9a-fA-F]+)"
                rb" ip-peer-ip=(?:0x)?([0-9a-fA-F]+)",
                arp_window,
            )
            if arp_summary_result is None:
                missing.append(
                    "RTL8852BS2 resident window protected ARP request "
                    "summary")
            else:
                (sent, byte_count, tpa, status, replies, reply_ip, net,
                 arp_frames, ipv4_frames, other_frames, other_type,
                 requests, peer_ip, peer_tpa,
                 ipv4_peer_ip) = (int(group, 16)
                                  for group in arp_summary_result.groups())

                def dotted(value):
                    return "{0}.{1}.{2}.{3}".format(
                        (value >> 24) & 0xff, (value >> 16) & 0xff,
                        (value >> 8) & 0xff, value & 0xff)

                print(
                    "[serial] decrypted payloads: {0} with an LLC/SNAP "
                    "header, {1} ARP ({2} requests), {3} IPv4, {4} other"
                    "{5}".format(
                        net, arp_frames, requests, ipv4_frames, other_frames,
                        f" (first ethertype 0x{other_type:04x})"
                        if other_type else ""),
                    file=sys.stderr)
                if peer_ip:
                    print(
                        "[serial] learned station {0} asking about {1}".format(
                            dotted(peer_ip), dotted(peer_tpa)),
                        file=sys.stderr)
                elif ipv4_peer_ip:
                    print(
                        "[serial] learned station {0} from an IPv4 "
                        "datagram".format(dotted(ipv4_peer_ip)),
                        file=sys.stderr)
                else:
                    print(
                        "[serial] no station learned: the window heard no "
                        "decrypted network payload to aim a request at",
                        file=sys.stderr)

                verdict = (
                    f"{replies} answered, from {dotted(reply_ip)}"
                    if replies else
                    "no answer; the encryption and the transmit path are "
                    "both excluded, so read the claim round below")
                print(
                    "[serial] protected ARP request: sent={0} bytes={1} "
                    "target={2} status={3}: {4}".format(
                        sent, byte_count, dotted(tpa), status, verdict),
                    file=sys.stderr)
        if args.require_runtime_ccmp_selftest:
            # Increment 3s compares the hardware's ciphertext against
            # ciphertext this component computes itself, and that comparison
            # is only evidence when the software side of it is known to be
            # right.  So the software side is put in front of two published
            # vectors first: RFC 3610 packet vector one for the algorithm, and
            # a frame of the shape the readback submits, under the FIPS 197
            # sample key, for the two pieces the RFC vector cannot reach - the
            # authenticated data built out of an 802.11 header and the nonce
            # built out of a cipher header.
            #
            # Unlike the verdict it helps produce, this one is asserted: a
            # cipher that disagrees with a published vector is broken, not a
            # finding.  The 0x prefix is mandatory in the last two for the
            # same reason as below.
            ccmp_selftest_result = re.search(
                rb"K1 Wi-Fi GPL: resident ccmp selftest"
                rb" rfc=(?:0x)?0*1 rfc-mic=(?:0x)?0*1 aad=(?:0x)?0*1"
                rb" nonce=(?:0x)?0*1 frame=(?:0x)?0*1 frame-mic=(?:0x)?0*1"
                rb" stage=0x0+(?![0-9a-fA-F])"
                rb" status=0x0+(?![0-9a-fA-F])",
                started,
            )
            if ccmp_selftest_result is None:
                missing.append(
                    "RTL8852BS2 software CCMP known answer selftest")
            else:
                print(
                    "[serial] software CCMP selftest: RFC 3610 vector one and "
                    "the frame-shaped vector both reproduce, so the "
                    "authenticated data, the nonce, the counter mode and the "
                    "integrity code are all right",
                    file=sys.stderr)
        if args.require_runtime_loopback_readback:
            # The last question the air-side readings left open: the access
            # point acknowledges a frame before it consults any key, so its
            # silence cannot separate a frame it decrypted and dropped from a
            # frame that was never encrypted.  MAC loopback can, because the
            # loopback tap is a CMAC register while the security engine is a
            # DMAC block: a frame that comes back around has already passed
            # the engine, so its payload says what the engine did.
            #
            # The reader is required to agree with its offline model first, and
            # this is the only hard requirement here.  The verdict itself is
            # deliberately not required to take any value: plaintext coming
            # back is the finding this increment exists to produce, and a
            # requirement that refused it would turn the finding into a failed
            # run.
            #
            # The 0x prefix is mandatory in the last two: with it optional,
            # `0+` can match the zero of "0x" and stop, so any nonzero value
            # would satisfy the assertion.
            loopback_model_result = re.search(
                rb"K1 Wi-Fi GPL: resident loopback selftest"
                rb" plain=(?:0x)?0*3 cipher=(?:0x)?0*1 header=(?:0x)?0*2"
                rb" rxdec=(?:0x)?0*4 short=(?:0x)?0*5 diff=(?:0x)?0*24"
                rb" first=0x0+(?![0-9a-fA-F]) llc=(?:0x)?0*8"
                rb" pn=(?:0x)?0*1 stage=0x0+(?![0-9a-fA-F])"
                rb" status=0x0+(?![0-9a-fA-F])",
                started,
            )
            if loopback_model_result is None:
                missing.append(
                    "RTL8852BS2 loopback payload reader model")

            # And the readback's own line, required to be present and not to
            # carry any particular value.
            loopback_result = re.search(
                rb"K1 Wi-Fi GPL: resident loopback readback "
                rb"verdict=(?:0x)?([0-9a-fA-F]+)"
                rb" looped=(?:0x)?([0-9a-fA-F]+)"
                rb"[^\r\n]* self=(?:0x)?([0-9a-fA-F]+)"
                rb" frame=(?:0x)?([0-9a-fA-F]+)"
                rb" len=(?:0x)?([0-9a-fA-F]+)"
                rb"[^\r\n]* llc=(?:0x)?([0-9a-fA-F]+)"
                rb" diff-bytes=(?:0x)?([0-9a-fA-F]+)"
                rb" diff-first=(?:0x)?([0-9a-fA-F]+)"
                rb" sw-ready=(?:0x)?([0-9a-fA-F]+)"
                rb" sw-diff=(?:0x)?([0-9a-fA-F]+)"
                rb" sw-first=(?:0x)?([0-9a-fA-F]+)"
                rb" sw-mic=(?:0x)?([0-9a-fA-F]+)"
                rb" sw-mic-diff=(?:0x)?([0-9a-fA-F]+)"
                rb" sw-stage=(?:0x)?([0-9a-fA-F]+)"
                rb" pn=(?:0x)?([0-9a-fA-F]+)"
                rb" prot=(?:0x)?([0-9a-fA-F]+)"
                rb" hw-dec=(?:0x)?([0-9a-fA-F]+)"
                rb"[^\r\n]* status=(?:0x)?([0-9a-fA-F]+)",
                started,
            )
            if loopback_result is None:
                missing.append(
                    "RTL8852BS2 resident window MAC loopback readback")
            else:
                (verdict_code, looped, self_frames, frame_valid, frame_length,
                 llc_at, diff_bytes, diff_first, sw_ready, sw_diff, sw_first,
                 sw_mic, sw_mic_diff, sw_stage, pn_match, protected_frame,
                 hw_dec, loopback_status) = (
                     int(group, 16) for group in loopback_result.groups())

                verdicts = {
                    0x0: "nothing classified",
                    0x1: "ciphertext, so the transmit path does encrypt",
                    0x2: "an integrity code was appended but the body is "
                         "unchanged",
                    0x3: "the plaintext that was submitted, so the transmit "
                         "path is not encrypting",
                    0x4: "the return path decrypted it, so the payload says "
                         "nothing about the transmit path",
                    0x5: "too short to read either offset",
                }
                print(
                    "[serial] MAC loopback readback: looped={0} self={1} "
                    "frame={2} len={3} status={4}".format(
                        looped, self_frames, frame_valid, frame_length,
                        loopback_status),
                    file=sys.stderr)
                print(
                    "[serial] looped payload: llc-at={0} diff-bytes={1} "
                    "diff-first={2} pn-kept={3} protected={4} hw-dec={5}"
                    .format(
                        "nowhere" if llc_at == 0xff else llc_at, diff_bytes,
                        "none" if diff_first == 0xff else diff_first,
                        pn_match, protected_frame, hw_dec),
                    file=sys.stderr)
                print(
                    "[serial] loopback verdict {0}: {1}".format(
                        verdict_code,
                        verdicts.get(verdict_code, "unknown")),
                    file=sys.stderr)

                # And increment 3s's reading, which is reported and not
                # asserted.  "The hardware encrypted" was 3r's answer;
                # "the hardware encrypted correctly" is this one, and it is
                # the difference between an access point that silently fails
                # the integrity check and an access point that decrypts the
                # frame and declines to forward it.
                if not sw_ready:
                    print(
                        "[serial] software CCMP comparison did not run: "
                        "sw-stage={0}".format(sw_stage),
                        file=sys.stderr)
                else:
                    print(
                        "[serial] software CCMP: cipher-diff={0} first={1} "
                        "mic-match={2} mic-diff={3}".format(
                            sw_diff,
                            "none" if sw_first == 0xff else sw_first,
                            sw_mic, sw_mic_diff),
                        file=sys.stderr)
                    if sw_diff == 0 and sw_mic:
                        print(
                            "[serial] software CCMP verdict: the ciphertext "
                            "and the integrity code the hardware produced are "
                            "exactly what the derived temporal key produces, "
                            "so the encryption is right and the access "
                            "point's forwarding is the next place to look",
                            file=sys.stderr)
                    else:
                        print(
                            "[serial] software CCMP verdict: the hardware's "
                            "output differs from what the derived temporal "
                            "key produces, so the key bytes going into the "
                            "security CAM, or the engine's nonce and "
                            "authenticated data, are the next place to look",
                            file=sys.stderr)

            # And increment 3t's second frame, which is the same frame with
            # address 1 changed to this station's own MAC.  Required to be
            # present, because the window either sent it or stopped before it,
            # and reported rather than asserted, because a from-DS frame that
            # never comes back is one of the two readings it was sent to take.
            downlink_result = re.search(
                rb"K1 Wi-Fi GPL: resident loopback downlink "
                rb"sent=(?:0x)?([0-9a-fA-F]+)"
                rb"[^\r\n]* seen=(?:0x)?([0-9a-fA-F]+)"
                rb" frame=(?:0x)?([0-9a-fA-F]+)"
                rb" len=(?:0x)?([0-9a-fA-F]+)"
                rb" fc=(?:0x)?([0-9a-fA-F]+)"
                rb"[^\r\n]* llc=(?:0x)?([0-9a-fA-F]+)"
                rb" diff-bytes=(?:0x)?([0-9a-fA-F]+)"
                rb"[^\r\n]* verdict=(?:0x)?([0-9a-fA-F]+)"
                rb" pn=(?:0x)?([0-9a-fA-F]+)"
                rb" prot=(?:0x)?([0-9a-fA-F]+)"
                rb" hw-dec=(?:0x)?([0-9a-fA-F]+)"
                rb" sw-dec=(?:0x)?([0-9a-fA-F]+)"
                rb" a1-match=(?:0x)?([0-9a-fA-F]+)"
                rb" icv=(?:0x)?([0-9a-fA-F]+)"
                rb" sec-type=(?:0x)?([0-9a-fA-F]+)"
                rb" sec-cam=(?:0x)?([0-9a-fA-F]+)"
                rb"[^\r\n]* stage=(?:0x)?([0-9a-fA-F]+)"
                rb" status=(?:0x)?([0-9a-fA-F]+)",
                started,
            )
            if downlink_result is None:
                missing.append(
                    "RTL8852BS2 resident window from-DS loopback probe")
            else:
                (dl_sent, dl_seen, dl_frame, dl_length, dl_fc, dl_llc_at,
                 dl_diff_bytes, dl_verdict, dl_pn, dl_prot, dl_hw_dec,
                 dl_sw_dec, dl_a1_match, dl_icv, dl_sec_type, dl_sec_cam,
                 dl_stage, dl_status) = (
                     int(group, 16) for group in downlink_result.groups())

                print(
                    "[serial] from-DS loopback probe: sent={0} seen={1} "
                    "frame={2} len={3} fc={4:#06x} stage={5} status={6}"
                    .format(dl_sent, dl_seen, dl_frame, dl_length, dl_fc,
                            dl_stage, dl_status),
                    file=sys.stderr)
                if not dl_sent:
                    print(
                        "[serial] the from-DS frame was never submitted, so "
                        "this window says nothing about the receive path: "
                        "status={0}".format(dl_status),
                        file=sys.stderr)
                elif not dl_frame:
                    print(
                        "[serial] the from-DS frame did not come back around "
                        "the MAC at all, which is itself a receive-path "
                        "reading: the frame the access point would send is "
                        "the frame this port does not see",
                        file=sys.stderr)
                else:
                    print(
                        "[serial] from-DS reception: a1-match={0} hw-dec={1} "
                        "sw-dec={2} icv={3} sec-type={4} sec-cam={5} "
                        "llc-at={6} diff-bytes={7} pn-kept={8} verdict={9}"
                        .format(
                            dl_a1_match, dl_hw_dec, dl_sw_dec, dl_icv,
                            dl_sec_type, dl_sec_cam,
                            "nowhere" if dl_llc_at == 0xff else dl_llc_at,
                            dl_diff_bytes, dl_pn, dl_verdict),
                        file=sys.stderr)
                    if not dl_a1_match:
                        print(
                            "[serial] from-DS verdict: address 1 was this "
                            "station's own MAC and the receive path did not "
                            "recognise it, so the address CAM's self address "
                            "is where to look -- and that alone explains a "
                            "port whose probes go out and are never answered",
                            file=sys.stderr)
                    elif not dl_hw_dec or dl_icv:
                        print(
                            "[serial] from-DS verdict: address 1 was "
                            "recognised but the security engine did not "
                            "decrypt the frame, so the pairwise key is "
                            "reachable on transmit and not on receive: the "
                            "address CAM's key mapping is where to look",
                            file=sys.stderr)
                    elif dl_sec_cam != 0:
                        print(
                            "[serial] from-DS verdict: the frame was "
                            "decrypted with key table entry {0} rather than "
                            "the pairwise entry, so a unicast frame is being "
                            "given the group key".format(dl_sec_cam),
                            file=sys.stderr)
                    else:
                        print(
                            "[serial] from-DS verdict: the receive path "
                            "recognised a frame addressed to this station and "
                            "the security engine decrypted it with the "
                            "pairwise key, so this port can receive what the "
                            "access point would send and the access point's "
                            "forwarding is what remains",
                            file=sys.stderr)
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
                if serial is None:
                    raise XmodemError(
                        "captured log has no runtime RX worker dispatch and "
                        "a replay cannot ask the board for one")

                wait_for_runtime_rx_worker(serial)
            else:
                print("PASS: K1 runtime RX worker dispatched C2H loopback",
                      file=sys.stderr)
        if serial is None:
            # These three ask the board a question rather than read the log,
            # so a replay cannot run them.  Say which ones were skipped: a
            # verdict that was never applied must not read as one that passed.
            skipped = [name for flag, name in (
                (args.verify_bt_hci_open, "/dev/ttyHCI0 open"),
                (args.require_bt_host_scan, "Bluetooth host scan"),
                (args.require_wlan0_scan, "wlan0 scan"))
                if flag]
            if skipped:
                print("[replay] not applied (needs a board): "
                      + ", ".join(skipped), file=sys.stderr)

            print("PASS: every log-only verdict holds for this capture",
                  file=sys.stderr)
            return 0

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
        if serial is not None:
            serial.close()
        if compressed_payload is not None:
            compressed_payload.unlink(missing_ok=True)
        print("{0}: {1}".format(
            "Replayed log" if replay is not None else "Serial log", log_path),
            file=sys.stderr)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (XmodemError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
