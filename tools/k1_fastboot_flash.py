#!/usr/bin/env python3
"""Run a guarded K1 Fastboot flash and reboot verification cycle.

Manifest validation and device inspection are read-only. A write requires
both --execute and the exact confirmation token, and the manifest must name a
single /dev child and a recovery image.
"""

# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


CONFIRMATION = "K1-FASTBOOT-WRITE"
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
TARGET_RE = re.compile(r"^[A-Za-z0-9._-]+$")


class ToolError(RuntimeError):
    """A user-correctable preflight or command failure."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checked_file(manifest: dict, path_key: str, hash_key: str) -> Path:
    value = manifest.get(path_key)
    expected = manifest.get(hash_key)
    if not isinstance(value, str) or not value:
        raise ToolError(f"{path_key} must be a non-empty path")
    path = Path(value)
    if not path.is_absolute() or not path.is_file():
        raise ToolError(f"{path_key} must be an existing absolute file: {path}")
    if not isinstance(expected, str) or not SHA256_RE.fullmatch(expected):
        raise ToolError(f"{hash_key} must be a 64-character SHA-256 value")
    actual = sha256_file(path)
    if actual.lower() != expected.lower():
        raise ToolError(f"{hash_key} mismatch: expected {expected}, got {actual}")
    return path


def load_manifest(path: Path) -> tuple[dict, Path]:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ToolError(f"cannot read manifest: {exc}") from exc

    if not isinstance(manifest, dict):
        raise ToolError("manifest root must be a JSON object")
    if manifest.get("board") != "muse_pi_pro":
        raise ToolError("board must be muse_pi_pro")
    if manifest.get("protocol") != "fastboot":
        raise ToolError("protocol must be fastboot for this tool")
    target = manifest.get("target_partition")
    if not isinstance(target, str) or not TARGET_RE.fullmatch(target):
        raise ToolError(
            "target_partition must be one /dev child name without slash"
        )
    if manifest.get("write_enabled") is not True:
        raise ToolError("write_enabled must be true in the explicit write manifest")

    image = checked_file(manifest, "image", "image_sha256")
    recovery = checked_file(manifest, "recovery_image", "recovery_sha256")
    if image == recovery:
        raise ToolError("image and recovery_image must be different files")

    expected = manifest.get("expected_getvar")
    if not isinstance(expected, dict) or not expected:
        raise ToolError("expected_getvar must contain at least one exact value")
    for key, value in expected.items():
        if (not isinstance(key, str) or
                not re.fullmatch(r"[A-Za-z0-9_.-]+", key) or
                not isinstance(value, str) or not value):
            raise ToolError("expected_getvar keys and values must be non-empty strings")

    markers = manifest.get("serial_markers")
    if not isinstance(markers, list) or not markers or not all(
            isinstance(marker, str) and marker for marker in markers):
        raise ToolError("serial_markers must contain at least one non-empty string")

    return manifest, image


def command_result(command: list[str], timeout: float = 60) -> tuple[int, str]:
    try:
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ToolError(
            f"command failed to start or timed out: {' '.join(command)}: {exc}"
        ) from exc

    output = (result.stdout or "") + (result.stderr or "")
    return result.returncode, output


def require_success(command: list[str], timeout: float = 60) -> str:
    code, output = command_result(command, timeout)
    if code != 0:
        raise ToolError(
            f"command returned {code}: {' '.join(command)}\n{output.strip()}"
        )
    if "FAILED" in output or "FAIL" in output:
        raise ToolError(f"fastboot reported failure: {' '.join(command)}\n{output.strip()}")
    return output


def fastboot_command(binary: str, serial: str | None, *args: str) -> list[str]:
    command = [binary]
    if serial:
        command += ["-s", serial]
    command += list(args)
    return command


def discover(binary: str, serial: str | None) -> str:
    output = require_success(fastboot_command(binary, serial, "devices"))
    devices = []
    for line in output.splitlines():
        fields = line.split()
        if fields and fields[0] not in {"<waiting>", "no", "????????????"}:
            devices.append(fields[0])
    if serial:
        if serial not in devices:
            raise ToolError(f"requested Fastboot serial is not present: {serial}")
        return serial
    if len(devices) != 1:
        raise ToolError(f"expected exactly one Fastboot device, found {len(devices)}")
    return devices[0]


def getvar(binary: str, serial: str, name: str) -> tuple[str | None, str]:
    output = require_success(fastboot_command(binary, serial, "getvar", name))
    match = re.search(rf"(?:^|\n){re.escape(name)}:\s*([^\r\n]*)", output)
    return (match.group(1).strip() if match else None), output


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Guarded K1 Fastboot flash, reboot, and serial verification"
    )
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--fastboot", default=None, help="fastboot executable")
    parser.add_argument("--serial", help="Fastboot device serial when more than one is attached")
    parser.add_argument("--probe", action="store_true", help="probe the device without writing")
    parser.add_argument("--execute", action="store_true", help="allow the flash operation")
    parser.add_argument("--confirm", help="must equal K1-FASTBOOT-WRITE for --execute")
    parser.add_argument("--serial-device", help="USB-TTL device for post-reboot capture")
    parser.add_argument("--serial-log", type=Path, help="post-reboot serial log path")
    parser.add_argument("--serial-duration", type=float, default=45)
    parser.add_argument(
        "--no-reboot", action="store_true", help="leave the device in Fastboot after flash"
    )
    parser.add_argument("--evidence", type=Path, help="JSON evidence output path")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    evidence = args.evidence or Path("out/k1-fastboot") / (
        dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + ".json"
    )
    evidence = evidence.resolve()
    record: dict = {
        "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "manifest": str(args.manifest.resolve()),
        "write_attempted": False,
        "commands": [],
    }
    capture = None

    try:
        manifest, image = load_manifest(args.manifest.resolve())
        record.update({
            "board": manifest["board"],
            "protocol": manifest["protocol"],
            "target_partition": manifest["target_partition"],
            "image": str(image),
            "image_sha256": sha256_file(image),
            "recovery_image": manifest["recovery_image"],
            "recovery_sha256": manifest["recovery_sha256"].lower(),
        })

        binary = args.fastboot or shutil.which("fastboot")
        if not binary:
            raise ToolError("fastboot executable not found; install Android platform-tools")
        if not os.access(binary, os.X_OK):
            raise ToolError(f"fastboot is not executable: {binary}")

        if not args.execute and not args.probe:
            print("PASS: manifest hashes and safety fields validated")
            print(
                "REFUSED: add --probe for read-only device checks or "
                "--execute --confirm K1-FASTBOOT-WRITE to write"
            )
            return 2

        serial = discover(binary, args.serial)
        record["fastboot_serial"] = serial
        print(f"PASS: one Fastboot device: {serial}")

        for name, expected in manifest["expected_getvar"].items():
            actual, output = getvar(binary, serial, name)
            record["commands"].append({"operation": f"getvar {name}", "output": output})
            if actual != expected:
                raise ToolError(f"getvar {name} mismatch: expected {expected!r}, got {actual!r}")
            print(f"PASS: getvar {name}={actual}")

        max_download, output = getvar(binary, serial, "max-download-size")
        record["commands"].append({
            "operation": "getvar max-download-size",
            "output": output,
        })
        if max_download is None:
            raise ToolError("Fastboot did not report max-download-size")
        try:
            max_download_bytes = int(max_download, 0)
        except ValueError as exc:
            raise ToolError(
                f"invalid Fastboot max-download-size: {max_download!r}"
            ) from exc
        if image.stat().st_size > max_download_bytes:
            raise ToolError(
                f"image is {image.stat().st_size} bytes, above Fastboot limit "
                f"{max_download_bytes}"
            )
        print(f"PASS: image fits max-download-size={max_download_bytes}")

        if not args.execute:
            print("READY: probe completed; no write command was run")
            return 0

        if args.confirm != CONFIRMATION:
            raise ToolError(f"--execute requires --confirm {CONFIRMATION}")
        if not args.serial_device:
            raise ToolError("--execute requires --serial-device for reboot verification")
        if args.serial_duration <= 0:
            raise ToolError("--serial-duration must be positive")

        serial_log = (args.serial_log or evidence.with_suffix(".serial.log")).resolve()
        serial_log.parent.mkdir(parents=True, exist_ok=True)
        capture_script = Path(__file__).with_name("capture_k1_serial.py")
        capture = subprocess.Popen([
            sys.executable, str(capture_script),
            "--device", args.serial_device,
            "--output", str(serial_log),
            "--duration", str(args.serial_duration),
        ], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)

        target = manifest["target_partition"]
        flash_command = fastboot_command(binary, serial, "flash", target, str(image))
        record["write_attempted"] = True
        record["flash_command"] = flash_command
        output = require_success(flash_command, timeout=max(120, args.serial_duration))
        record["flash_output"] = output
        print(f"PASS: flash {target} completed")

        if not args.no_reboot:
            reboot_command = fastboot_command(binary, serial, "reboot")
            record["reboot_command"] = reboot_command
            record["reboot_output"] = require_success(reboot_command)
            print("PASS: reboot requested")
            try:
                capture.wait(timeout=args.serial_duration + 10)
            except subprocess.TimeoutExpired:
                capture.terminate()
                capture.wait(timeout=5)
                raise ToolError("serial capture timed out after reboot")
            serial_text = serial_log.read_text(encoding="utf-8", errors="replace")
            missing = [marker for marker in manifest["serial_markers"] if marker not in serial_text]
            if missing:
                raise ToolError(f"post-reboot serial markers missing: {missing}")
            record["serial_log"] = str(serial_log)
            record["serial_markers"] = manifest["serial_markers"]
            print("PASS: post-reboot serial markers found")
        else:
            capture.terminate()
            capture.wait(timeout=5)
            print("WARNING: device left in Fastboot; no reboot verification was run")

        print("COMPLETE: Fastboot write and verification cycle passed")
        return 0
    except ToolError as exc:
        record["error"] = str(exc)
        print(f"REFUSED/FAILED: {exc}", file=sys.stderr)
        return 2
    finally:
        if capture is not None and capture.poll() is None:
            capture.terminate()
            capture.wait(timeout=5)
        record["ended_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
        evidence.parent.mkdir(parents=True, exist_ok=True)
        evidence.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        print(f"Evidence: {evidence}", file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
