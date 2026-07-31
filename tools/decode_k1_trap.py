#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#
# Parse K1 early exception blocks and symbolize sepc addresses.

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys


CAUSES = {
    0: "instruction address misaligned",
    1: "instruction access fault",
    2: "illegal instruction",
    3: "breakpoint",
    4: "load address misaligned",
    5: "load access fault",
    6: "store/AMO address misaligned",
    7: "store/AMO access fault",
    8: "environment call from U-mode",
    9: "environment call from S-mode",
    12: "instruction page fault",
    13: "load page fault",
    15: "store/AMO page fault",
}

FIELD = re.compile(
    r"^\s*(scause|sepc|stval|sstatus|satp|sp)=(0x[0-9a-fA-F]+)\s*$"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Decode K1 exception log")
    parser.add_argument("--log", required=True, help="captured serial log")
    parser.add_argument("--elf", required=True, help="matching NuttX ELF")
    parser.add_argument("--addr2line", required=True, help="RISC-V addr2line")
    parser.add_argument("--output", help="write report to this path")
    return parser.parse_args()


def parse_blocks(text: str) -> list[dict[str, int]]:
    blocks: list[dict[str, int]] = []
    current: dict[str, int] | None = None

    for line in text.replace("\r", "").splitlines():
        if line.strip() == "K1 EXCEPTION":
            if current:
                blocks.append(current)
            current = {}
            continue

        if current is None:
            continue

        match = FIELD.match(line)
        if match:
            current[match.group(1)] = int(match.group(2), 16)

    if current:
        blocks.append(current)
    return blocks


def symbolize(tool: pathlib.Path, elf: pathlib.Path, address: int) -> str:
    result = subprocess.run(
        [str(tool), "-e", str(elf), "-f", "-C", f"0x{address:x}"],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return f"addr2line failed: {result.stderr.strip()}"
    return " | ".join(line.strip() for line in result.stdout.splitlines())


def format_report(
    blocks: list[dict[str, int]],
    elf: pathlib.Path,
    tool: pathlib.Path,
) -> str:
    lines = ["# K1 trap decode", "", f"ELF: `{elf}`", ""]
    if not blocks:
        lines.append("No `K1 EXCEPTION` block found.")
        return "\n".join(lines) + "\n"

    for index, block in enumerate(blocks, start=1):
        cause = block.get("scause")
        lines.extend([f"## Exception {index}", ""])
        if cause is not None:
            interrupt = bool(cause >> 63)
            code = cause & ((1 << 63) - 1)
            name = "interrupt" if interrupt else CAUSES.get(code, "unknown")
            lines.append(f"- scause: `0x{cause:016x}` ({name}, code {code})")

        for field in ("sepc", "stval", "sstatus", "satp", "sp"):
            if field in block:
                lines.append(f"- {field}: `0x{block[field]:016x}`")

        if "sepc" in block:
            symbol = symbolize(tool, elf, block["sepc"])
            lines.append(f"- symbol: `{symbol}`")
        lines.append("")

    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    log = pathlib.Path(args.log).resolve()
    elf = pathlib.Path(args.elf).resolve()
    tool = pathlib.Path(args.addr2line).resolve()

    for path, label in ((log, "log"), (elf, "ELF"), (tool, "addr2line")):
        if not path.exists():
            print(f"ERROR: {label} not found: {path}", file=sys.stderr)
            return 1

    report = format_report(
        parse_blocks(log.read_text(encoding="utf-8", errors="replace")),
        elf,
        tool,
    )
    if args.output:
        output = pathlib.Path(args.output).resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(report, encoding="utf-8")
        print(output)
    else:
        sys.stdout.write(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
