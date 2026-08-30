#!/usr/bin/env python3
"""Normalize RTL8852 SDIO CMD52 traces before the first CMD53 request.

The Linux MMC tracepoint records the raw CMD52 argument, but the Realtek
driver uses Function 1 addresses 0x1040--0x1048 as an indirect-register
window.  This tool converts those window transactions into logical register
operations so a NuttX bootstrap can be compared with a normal Linux boot.
"""

# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from collections import Counter
from dataclasses import dataclass, field


CMD_PATTERN = re.compile(r"cmd_opcode=(?P<opcode>52|53) cmd_arg=0x(?P<arg>[0-9a-fA-F]+)")
POWER_STEP_PATTERN = re.compile(
    r"\{\s*(0x[0-9a-fA-F]+)\s*,\s*0x[0-9a-fA-F]+\s*,\s*"
    r"0x[0-9a-fA-F]+\s*,\s*K1_RTL8852BS_POWER_"
)

INDIRECT_ADDR = 0x1040
INDIRECT_DATA = 0x1044
INDIRECT_CTRL = 0x1048
INDIRECT_READY = 0x80


@dataclass
class Operation:
    """One direct or normalized Function 1 operation."""

    request: int
    kind: str
    function: int
    address: int
    value: int | None = None
    width: int = 1
    detail: str = ""

    def render(self) -> str:
        direction = "W" if self.kind.endswith("write") else "R"
        if self.kind.startswith("indirect"):
            width = self.width * 8
            value = "" if self.value is None else f" value=0x{self.value:0{self.width * 2}x}"
            return (
                f"{self.request:04d} INDIR {direction}{width} "
                f"addr=0x{self.address:08x}{value}{self.detail}"
            )

        value = "" if self.value is None else f" value=0x{self.value:02x}"
        return (
            f"{self.request:04d} CMD52 {direction} f{self.function} "
            f"addr=0x{self.address:05x}{value}{self.detail}"
        )


@dataclass
class IndirectWindow:
    """The CMD52-programmed address and data halves of the Realtek window."""

    address: list[int] = field(default_factory=lambda: [0, 0, 0, 0])
    data: list[int] = field(default_factory=lambda: [0, 0, 0, 0])
    written_data: set[int] = field(default_factory=set)
    awaiting_result: bool = False

    def target(self) -> int:
        value = sum(byte << (index * 8) for index, byte in enumerate(self.address))
        return value & ~(INDIRECT_READY << 24)

    def data_value(self) -> int:
        return sum(byte << (index * 8) for index, byte in enumerate(self.data))

    def reset_data(self) -> None:
        self.written_data.clear()


def decode_cmd52(argument: int) -> tuple[bool, int, int, int]:
    """Return CMD52 write flag, function, 17-bit address and byte value."""

    return (
        bool(argument & (1 << 31)),
        (argument >> 28) & 0x7,
        (argument >> 9) & 0x1ffff,
        argument & 0xff,
    )


def parse_trace(trace: pathlib.Path) -> tuple[list[Operation], int, int | None]:
    """Parse CMD52 lines until the first CMD53 and normalize indirect access."""

    operations: list[Operation] = []
    window = IndirectWindow()
    cmd52_count = 0
    first_cmd53 = None

    for line in trace.read_text(encoding="utf-8", errors="replace").splitlines():
        match = CMD_PATTERN.search(line)
        if match is None:
            continue

        opcode = int(match.group("opcode"))
        argument = int(match.group("arg"), 16)
        if opcode == 53:
            first_cmd53 = argument
            break

        cmd52_count += 1
        write, function, address, value = decode_cmd52(argument)

        if function != 1:
            operations.append(
                Operation(cmd52_count, "direct-write" if write else "direct-read",
                          function, address, value if write else None)
            )
            continue

        if write and INDIRECT_ADDR <= address < INDIRECT_ADDR + 4:
            window.awaiting_result = False
            window.address[address - INDIRECT_ADDR] = value
            continue

        if write and INDIRECT_DATA <= address < INDIRECT_DATA + 4:
            window.awaiting_result = False
            data_index = address - INDIRECT_DATA
            window.data[data_index] = value
            window.written_data.add(data_index)
            continue

        if write and address == INDIRECT_CTRL:
            if value & 0x08:
                operation = Operation(cmd52_count, "indirect-read", function,
                                      window.target(), width=1 << (value & 0x3))
                operations.append(operation)
            elif value & 0x04:
                width = 1 << (value & 0x3)
                mask = (1 << (width * 8)) - 1
                operations.append(
                    Operation(cmd52_count, "indirect-write", function,
                              window.target(), window.data_value() & mask,
                              width)
                )
                window.reset_data()
            else:
                operations.append(
                    Operation(cmd52_count, "direct-write", function, address,
                              value, detail=f" ctrl=0x{value:02x}")
                )
            window.awaiting_result = True
            continue

        if not write and address == INDIRECT_ADDR + 3 and window.awaiting_result:
            continue

        if not write and INDIRECT_DATA <= address < INDIRECT_DATA + 4 and \
           window.awaiting_result:
            continue

        window.awaiting_result = False

        operations.append(
            Operation(cmd52_count, "direct-write" if write else "direct-read",
                      function, address, value if write else None)
        )

    return operations, cmd52_count, first_cmd53


def load_power_table_addresses(source: pathlib.Path) -> set[int]:
    """Extract addresses of the existing GPL power-on table without C parsing."""

    return {int(match.group(1), 16)
            for match in POWER_STEP_PATTERN.finditer(
                source.read_text(encoding="utf-8", errors="replace"))}


def print_summary(operations: list[Operation], cmd52_count: int,
                  first_cmd53: int | None, power_table: set[int] | None) -> None:
    indirect = [operation for operation in operations
                if operation.kind.startswith("indirect")]
    targets = Counter(operation.address for operation in indirect)

    print(f"CMD52 before first CMD53: {cmd52_count}")
    if first_cmd53 is None:
        print("First CMD53: not found")
    else:
        print(f"First CMD53: 0x{first_cmd53:08x}")

    print(f"Normalized operations: {len(operations)}")
    print(f"Indirect operations: {len(indirect)} "
          f"(reads={sum(op.kind == 'indirect-read' for op in indirect)}, "
          f"writes={sum(op.kind == 'indirect-write' for op in indirect)})")
    print("Most-accessed indirect targets:")
    for address, count in targets.most_common(12):
        print(f"  0x{address:08x}: {count}")

    if power_table is not None:
        trace_targets = set(targets)
        overlap = sorted(trace_targets & power_table)
        missing = sorted(trace_targets - power_table)
        print(f"GPL power-table addresses: {len(power_table)}")
        print("Trace targets also in GPL power table: " +
              (", ".join(f"0x{address:04x}" for address in overlap) or "none"))
        print("Trace indirect targets outside GPL power table: " +
              (", ".join(f"0x{address:08x}" for address in missing[:24]) or "none"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=pathlib.Path,
                        help="Linux mmc_request_start trace")
    parser.add_argument("--gpl-source", type=pathlib.Path,
                        help="existing k1_rtl8852bs_gpl.c for table comparison")
    parser.add_argument("--operations", action="store_true",
                        help="print normalized operations in trace order")
    parser.add_argument("--tail", type=int, default=0,
                        help="only print the final N normalized operations")
    args = parser.parse_args()

    if not args.trace.is_file():
        parser.error(f"trace is not a file: {args.trace}")
    if args.tail < 0:
        parser.error("--tail must not be negative")
    if args.gpl_source is not None and not args.gpl_source.is_file():
        parser.error(f"GPL source is not a file: {args.gpl_source}")

    operations, cmd52_count, first_cmd53 = parse_trace(args.trace)
    power_table = (load_power_table_addresses(args.gpl_source)
                   if args.gpl_source is not None else None)
    print_summary(operations, cmd52_count, first_cmd53, power_table)

    if args.operations:
        selected = operations[-args.tail:] if args.tail else operations
        print("Operations:")
        for operation in selected:
            print(operation.render())

    return 0


if __name__ == "__main__":
    sys.exit(main())
