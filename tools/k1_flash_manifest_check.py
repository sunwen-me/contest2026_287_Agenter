#!/usr/bin/env python3
"""Validate a K1 flash/recovery manifest without touching a target device."""

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
ALLOWED_PROTOCOLS = {"fdl", "fastboot"}


def add_error(errors, message):
    errors.append(message)


def check_file(manifest, key, hash_key, errors):
    value = manifest.get(key)
    expected = manifest.get(hash_key)
    if not isinstance(value, str) or not value:
        add_error(errors, f"{key} must be a non-empty path")
        return
    path = Path(value)
    if not path.is_absolute():
        add_error(errors, f"{key} must be absolute: {value}")
    if not path.is_file():
        add_error(errors, f"{key} does not name a regular file: {path}")
        return
    if not isinstance(expected, str) or not SHA256_RE.fullmatch(expected):
        add_error(errors, f"{hash_key} must be a 64-character SHA-256 hex string")
        return

    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    actual = digest.hexdigest()
    if actual.lower() != expected.lower():
        add_error(errors, f"{hash_key} mismatch: expected {expected}, got {actual}")
    else:
        print(f"PASS: {key} sha256 {actual}")


def main():
    parser = argparse.ArgumentParser(
        description="Validate K1 flash/recovery metadata; never performs a write."
    )
    parser.add_argument("manifest", type=Path)
    parser.add_argument(
        "--allow-write-manifest",
        action="store_true",
        help="validate a write-enabled manifest without performing a write",
    )
    args = parser.parse_args()

    errors = []
    try:
        with args.manifest.open("r", encoding="utf-8") as stream:
            manifest = json.load(stream)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: cannot read manifest: {exc}", file=sys.stderr)
        return 2

    if not isinstance(manifest, dict):
        print("ERROR: manifest root must be a JSON object", file=sys.stderr)
        return 2

    if manifest.get("board") != "muse_pi_pro":
        add_error(errors, "board must be muse_pi_pro")

    protocol = manifest.get("protocol")
    if protocol not in ALLOWED_PROTOCOLS:
        add_error(errors, "protocol must be exactly fdl or fastboot")

    if not isinstance(manifest.get("target_partition"), str) or not manifest[
        "target_partition"
    ].strip():
        add_error(errors, "target_partition must be non-empty")

    write_enabled = manifest.get("write_enabled")
    if write_enabled is not False and not (
        args.allow_write_manifest and write_enabled is True
    ):
        add_error(
            errors,
            "write_enabled must be false, or use --allow-write-manifest for true",
        )

    check_file(manifest, "image", "image_sha256", errors)
    check_file(manifest, "recovery_image", "recovery_sha256", errors)

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        print("REFUSED: no fastboot, FDL, MMC, erase, or write command was run", file=sys.stderr)
        return 2

    print(f"PASS: board {manifest['board']}")
    print(f"PASS: protocol {manifest['protocol']}")
    print(f"PASS: target partition {manifest['target_partition']}")
    print(f"PASS: write_enabled {str(write_enabled).lower()}")
    if write_enabled is True:
        print("PASS: write manifest validated; no write command was run")
    print("READY: manual protocol, partition, and recovery confirmation still required")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
