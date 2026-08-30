#!/usr/bin/env python3
"""Generate a local RTL8852BS H5 patch include from vendor firmware files.

The resulting file is a locally generated C include.  Do not commit it: the
firmware package supplied on the MUSE Pi Pro does not state redistribution
terms for the binary blobs.
"""

# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct
import sys


FIRMWARE_SIGNATURE = b"RTBTCore"
CONFIG_SIGNATURE = b"\x55\xab\x23\x87"
SECTION_SNIPPETS = 1
SECTION_DUMMY = 2
SECTION_SECURITY = 3
CONFIG_BAUD_RATE_OFFSET = 0x000C
VENDOR_BAUD_RATES = {
    0x0252C014: 115200,
    0x05F75004: 921600,
    0x00005004: 1000000,
    0x04928002: 1500000,
    0x01128002: 1500000,
    0x00005002: 2000000,
    0x0000B001: 2500000,
    0x04928001: 3000000,
    0x052A6001: 3500000,
    0x00005001: 4000000,
}


class ExtractError(RuntimeError):
    """The supplied Realtek files do not match the supported epatch format."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", type=pathlib.Path, required=True)
    parser.add_argument("--config", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument(
        "--eco",
        type=int,
        default=1,
        help="controller ROM ECO value returned by HCI 0xfc6d (default: 1)",
    )
    parser.add_argument(
        "--key-id",
        type=int,
        default=0,
        help="security project key returned by HCI 0xfc61 (default: 0)",
    )
    return parser.parse_args()


def read_u16(data: bytes, offset: int, limit: int) -> int:
    if offset + 2 > limit:
        raise ExtractError(f"truncated 16-bit field at offset {offset}")
    return struct.unpack_from("<H", data, offset)[0]


def read_u32(data: bytes, offset: int, limit: int) -> int:
    if offset + 4 > limit:
        raise ExtractError(f"truncated 32-bit field at offset {offset}")
    return struct.unpack_from("<I", data, offset)[0]


def parse_config(config: bytes) -> tuple[int, bool]:
    if len(config) < 6 or config[:4] != CONFIG_SIGNATURE:
        raise ExtractError("invalid Realtek config signature")

    declared_length = read_u16(config, 4, len(config))
    if len(config) != 6 + declared_length:
        raise ExtractError(
            f"config size {len(config)} does not match declared {declared_length}"
        )

    offset = 6
    vendor_baud: int | None = None
    hardware_flow_control = False
    while offset < len(config):
        if offset + 3 > len(config):
            raise ExtractError(f"truncated config entry at offset {offset}")

        entry_offset = read_u16(config, offset, len(config))
        entry_length = config[offset + 2]
        data_offset = offset + 3
        entry_end = data_offset + entry_length
        if entry_end > len(config):
            raise ExtractError(f"config entry at offset {offset} exceeds config")

        if entry_offset == CONFIG_BAUD_RATE_OFFSET:
            if entry_length < 4:
                raise ExtractError("RTL8852BS config baud entry is truncated")

            vendor_baud = read_u32(config, data_offset, entry_end)
            if entry_length > 12:
                hardware_flow_control = (config[data_offset + 12] & 0x04) != 0

        offset = entry_end

    if vendor_baud is None:
        raise ExtractError("RTL8852BS config does not contain a UART baud entry")
    if vendor_baud not in VENDOR_BAUD_RATES:
        raise ExtractError(f"unsupported Realtek vendor baud 0x{vendor_baud:08x}")

    return vendor_baud, hardware_flow_control


def extract_patch(firmware: bytes, eco: int, key_id: int) -> bytes:
    if firmware[:8] != FIRMWARE_SIGNATURE:
        raise ExtractError("unsupported firmware signature (expected RTBTCore)")
    if len(firmware) < 20:
        raise ExtractError("truncated RTBTCore header")
    if not 0 <= eco <= 0xFE or not 0 <= key_id <= 0xFF:
        raise ExtractError("ECO and key id must fit in one byte")

    sections = read_u32(firmware, 16, len(firmware))
    offset = 20
    selected: list[tuple[int, bytes]] = []
    target_eco = eco + 1

    for section_number in range(sections):
        opcode = read_u32(firmware, offset, len(firmware))
        section_length = read_u32(firmware, offset + 4, len(firmware))
        section_data = offset + 8
        section_end = section_data + section_length
        if section_end > len(firmware):
            raise ExtractError(f"section {section_number} exceeds firmware size")

        if opcode in (SECTION_SNIPPETS, SECTION_DUMMY, SECTION_SECURITY):
            entries = read_u16(firmware, section_data, section_end)
            entry_offset = section_data + 4
            for entry_number in range(entries):
                if entry_offset + 8 > section_end:
                    raise ExtractError(
                        f"section {section_number} entry {entry_number} is truncated"
                    )

                entry_eco = firmware[entry_offset]
                priority = firmware[entry_offset + 1]
                entry_key = firmware[entry_offset + 2]
                payload_length = read_u32(firmware, entry_offset + 4, section_end)
                payload_offset = entry_offset + 8
                payload_end = payload_offset + payload_length
                if payload_end > section_end:
                    raise ExtractError(
                        f"section {section_number} entry {entry_number} exceeds section"
                    )

                use_entry = entry_eco == target_eco
                if opcode == SECTION_SECURITY:
                    use_entry = use_entry and key_id != 0 and entry_key == key_id
                elif opcode == SECTION_DUMMY:
                    use_entry = use_entry and key_id == 0

                if use_entry:
                    selected.append((priority, firmware[payload_offset:payload_end]))

                entry_offset = payload_end

        offset = section_end

    if not selected:
        raise ExtractError(
            f"no epatch payload matches ROM ECO {eco} and key id {key_id}"
        )

    selected.sort(key=lambda item: item[0])
    return b"".join(payload for _, payload in selected)


def format_include(
    payload: bytes,
    firmware_sha256: str,
    config_sha256: str,
    vendor_baud: int,
    hardware_flow_control: bool,
) -> str:
    lines = [
        "/* Generated locally by tools/extract_rtl8852bs_bt_patch.py.",
        " * The input firmware is not licensed for repository redistribution.",
        f" * firmware sha256: {firmware_sha256}",
        f" * config sha256: {config_sha256}",
        f" * generated patch and config length: {len(payload)} bytes",
        " */",
        f"#define K1_BT_RTL8852BS_CONFIG_VENDOR_BAUD 0x{vendor_baud:08x}u",
        f"#define K1_BT_RTL8852BS_CONFIG_UART_BAUD {VENDOR_BAUD_RATES[vendor_baud]}u",
        "#define K1_BT_RTL8852BS_CONFIG_HARDWARE_FLOW_CONTROL "
        f"{int(hardware_flow_control)}u",
    ]

    for offset in range(0, len(payload), 12):
        chunk = payload[offset:offset + 12]
        lines.append("  " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")

    return "\n".join(lines) + "\n"


def main() -> int:
    args = parse_args()
    try:
        firmware = args.firmware.read_bytes()
        config = args.config.read_bytes()
        vendor_baud, hardware_flow_control = parse_config(config)
        patch = extract_patch(firmware, args.eco, args.key_id)
    except (OSError, ExtractError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    merged = patch + config
    output = format_include(
        merged,
        hashlib.sha256(firmware).hexdigest(),
        hashlib.sha256(config).hexdigest(),
        vendor_baud,
        hardware_flow_control,
    )

    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="ascii")
    except OSError as error:
        print(f"ERROR: cannot write {args.output}: {error}", file=sys.stderr)
        return 1

    print(
        f"generated {args.output}: {len(patch)} patch bytes + "
        f"{len(config)} config bytes = {len(merged)} bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
