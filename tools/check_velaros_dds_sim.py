#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#
"""Run the VelaROS ROS 2 core and Fast DDS simulator acceptance through a PTY."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import sys
import time


PROMPT = re.compile(rb"(?:^|[\r\n])[^\r\n>]*>\s*$", re.MULTILINE)
ANSI_ESCAPE = re.compile(rb"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))")


class AcceptanceError(RuntimeError):
    """A runtime acceptance condition was not met."""


class PtyConsole:
    def __init__(self, command: list[str], cwd: Path, log_path: Path) -> None:
        master_fd, slave_fd = os.openpty()
        self.master_fd = master_fd
        self.buffer = bytearray()
        self.log_file = log_path.open("wb")
        self.process = subprocess.Popen(
            command,
            cwd=cwd,
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            start_new_session=True,
            close_fds=True,
        )
        os.close(slave_fd)
        os.set_blocking(master_fd, False)

    def close(self) -> None:
        try:
            self.log_file.close()
        finally:
            try:
                os.close(self.master_fd)
            except OSError:
                pass

    def read_once(self, timeout: float) -> bool:
        readable, _, _ = select.select([self.master_fd], [], [], timeout)
        if not readable:
            return False
        try:
            chunk = os.read(self.master_fd, 65536)
        except (BlockingIOError, OSError):
            return False
        if not chunk:
            return False
        self.buffer.extend(chunk)
        self.log_file.write(chunk)
        self.log_file.flush()
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        return True

    def drain_for(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and self.process.poll() is None:
            self.read_once(min(0.2, max(0.0, deadline - time.monotonic())))

    def wait_until(self, description: str, predicate, timeout: float, start: int = 0) -> bytes:
        deadline = time.monotonic() + timeout
        while True:
            segment = bytes(self.buffer[start:])
            if predicate(ANSI_ESCAPE.sub(b"", segment)):
                return segment
            return_code = self.process.poll()
            if return_code is not None:
                raise AcceptanceError(
                    f"emulator exited with status {return_code} while waiting for {description}"
                )
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AcceptanceError(f"timed out waiting for {description}")
            self.read_once(min(0.5, remaining))

    def send(self, command: bytes) -> int:
        start = len(self.buffer)
        os.write(self.master_fd, command)
        return start

    def stop_emulator(self) -> None:
        if self.process.poll() is not None:
            return
        os.write(self.master_fd, b"\x01x")
        deadline = time.monotonic() + 10
        while self.process.poll() is None and time.monotonic() < deadline:
            self.read_once(0.2)
        if self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGTERM)
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=5)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build optionally, boot openvela, check the ROS 2 core slice, "
            "exchange three DDS samples, and verify clean exit."
        )
    )
    parser.add_argument(
        "--build",
        action="store_true",
        help="run the reproducible DDS simulator build before booting",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=120,
        help="boot timeout in seconds (default: 120)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="simulator output directory",
    )
    parser.add_argument(
        "--log",
        type=Path,
        help="runtime transcript path",
    )
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def assert_no_host_ros_contamination(firmware: Path, output_dir: Path) -> None:
    """Reject host ROS paths or shared libraries in target build metadata/ELF."""

    forbidden = (
        b"/opt/ros",
        b"libros-lyrical",
        b"librmw_dds_common.so",
        b"librmw_fastrtps_cpp.so",
    )
    targets = (firmware, output_dir / "build.ninja")
    for target in targets:
        if not target.is_file():
            if target == firmware:
                raise AcceptanceError(f"firmware ELF is missing: {firmware}")
            continue
        data = target.read_bytes()
        matches = [needle.decode() for needle in forbidden if needle in data]
        if matches:
            raise AcceptanceError(
                f"host ROS contamination in {target}: {', '.join(matches)}"
            )


def main() -> int:
    args = parse_args()
    script_dir = Path(__file__).resolve().parent
    contest_root = script_dir.parent
    workspace_root = Path(os.environ.get("OPENVELA_ROOT", contest_root.parent)).resolve()
    output_dir = (
        args.output.resolve()
        if args.output
        else workspace_root
        / "cmake_out"
        / "contest2026_287_Agenter_goldfish-arm64-v8a-ap-fastdds"
    )
    log_path = (
        args.log.resolve()
        if args.log
        else workspace_root / "cmake_out" / "velaros-dds-sim-runtime.log"
    )

    if args.timeout <= 0:
        raise AcceptanceError("--timeout must be positive")
    if args.build:
        subprocess.run([str(script_dir / "build_velaros_dds_sim.sh")], check=True)

    firmware = output_dir / "nuttx"
    emulator = workspace_root / "emulator.sh"
    if not firmware.is_file():
        raise AcceptanceError(f"firmware ELF is missing: {firmware}")
    if not os.access(emulator, os.X_OK):
        raise AcceptanceError(f"emulator launcher is missing or not executable: {emulator}")

    assert_no_host_ros_contamination(firmware, output_dir)
    print("host ROS contamination check: PASS")

    log_path.parent.mkdir(parents=True, exist_ok=True)
    for lock_name in ("hardware-qemu.ini.lock", "multiinstance.lock"):
        lock_path = output_dir / lock_name
        if lock_path.exists():
            lock_path.unlink()

    command = [
        str(emulator),
        str(output_dir),
        "-no-window",
        "-no-audio",
    ]
    console = PtyConsole(command, workspace_root, log_path)
    passed = False
    try:
        boot_start = len(console.buffer)
        console.wait_until(
            "NuttShell banner",
            lambda data: b"NuttShell (NSH)" in data,
            args.timeout,
            boot_start,
        )
        console.wait_until(
            "NuttShell prompt",
            lambda data: PROMPT.search(data) is not None,
            30,
            boot_start,
        )

        smoke_start = console.send(b"velaros_core_smoke\n")
        console.wait_until(
            "ROS 2 Lyrical core smoke test",
            lambda data: (
                b"VelaROS ROS 2 Lyrical core smoke: PASS" in data
                and b"rcutils allocator: PASS" in data
                and b"rmw node-name validation: PASS" in data
                and b"Fast DDS dynamic typesupport identifier: PASS" in data
                and PROMPT.search(data) is not None
            ),
            30,
            smoke_start,
        )

        graph_smoke_start = console.send(b"velaros_rmw_dds_smoke\n")
        console.wait_until(
            "rmw_dds_common generated typesupport and graph smoke test",
            lambda data: (
                b"rmw_dds_common generated Fast RTPS typesupport: PASS" in data
                and b"rmw_dds_common graph cache update: PASS" in data
                and b"VelaROS rmw_dds_common smoke: PASS" in data
                and PROMPT.search(data) is not None
            ),
            30,
            graph_smoke_start,
        )

        rmw_smoke_start = console.send(b"velaros_rmw_fastrtps_smoke\n")
        console.wait_until(
            "rmw_fastrtps context, node, graph, and cleanup lifecycle smoke test",
            lambda data: (
                b"rmw_fastrtps context init: PASS" in data
                and b"rmw_fastrtps Fast DDS node create: PASS" in data
                and b"rmw_fastrtps ROS graph query: PASS" in data
                and b"VelaROS rmw_fastrtps lifecycle smoke: PASS" in data
                and PROMPT.search(data) is not None
            ),
            45,
            rmw_smoke_start,
        )

        rcl_smoke_start = console.send(b"velaros_rcl_smoke\n")
        console.wait_until(
            "rcl minimal context, node, and cleanup lifecycle smoke test",
            lambda data: (
                b"rcl context init: PASS" in data
                and b"rcl Fast DDS node create: PASS" in data
                and b"rcl node fini: PASS" in data
                and b"rcl shutdown: PASS" in data
                and b"rcl context fini: PASS" in data
                and b"VelaROS rcl minimal lifecycle smoke: PASS" in data
                and PROMPT.search(data) is not None
            ),
            45,
            rcl_smoke_start,
        )

        subscriber_start = console.send(
            b"DDSHelloWorldExample subscriber --samples 3 &\n"
        )
        console.wait_until(
            "subscriber startup",
            lambda data: b"Subscriber running for 3 samples" in data,
            30,
            subscriber_start,
        )

        publisher_start = console.send(
            b"DDSHelloWorldExample publisher --samples 3 &\n"
        )

        def exchange_complete(data: bytes) -> bool:
            return (
                data.count(b" SENT") >= 3
                and data.count(b" RECEIVED") >= 3
                and b"Publisher matched." in data
                and b"Subscriber matched." in data
                and b"Publisher unmatched." in data
            )

        exchange = console.wait_until(
            "three sent and three received samples plus publisher cleanup",
            exchange_complete,
            60,
            publisher_start,
        )
        clean_exchange = ANSI_ESCAPE.sub(b"", exchange)
        sent_count = clean_exchange.count(b" SENT")
        received_count = clean_exchange.count(b" RECEIVED")

        console.drain_for(3)
        ps_start = console.send(b"ps\n")
        ps_output = console.wait_until(
            "post-test process listing",
            lambda data: PROMPT.search(data) is not None,
            15,
            ps_start,
        )
        clean_ps = ANSI_ESCAPE.sub(b"", ps_output)
        leftovers = re.findall(
            rb"^.*(?:DDSHelloWorldExample|Publisher|Subscriber).*$",
            clean_ps,
            re.MULTILINE,
        )
        if leftovers:
            decoded = "\n".join(line.decode(errors="replace") for line in leftovers)
            raise AcceptanceError(f"DDS tasks or threads remain after cleanup:\n{decoded}")

        passed = True
        print("\nVelaROS ROS 2 core + DDS simulator acceptance: PASS")
        print(f"ELF SHA256: {sha256(firmware)}")
        print(
            "ROS 2 core: rcutils allocator PASS / rmw node-name validation PASS "
            "/ Fast DDS dynamic typesupport PASS"
        )
        print(
            "rmw_dds_common: generated Fast RTPS typesupport PASS "
            "/ graph cache update PASS"
        )
        print(
            "rmw_fastrtps_cpp: context init PASS / Fast DDS node create PASS "
            "/ ROS graph query PASS / lifecycle cleanup PASS"
        )
        print(
            "rcl: context init PASS / Fast DDS node create PASS "
            "/ node, context, and participant cleanup PASS"
        )
        print(f"samples: {sent_count} SENT / {received_count} RECEIVED")
        print("cleanup: no Publisher, Subscriber, or DDSHelloWorldExample task remains")
        print(f"log: {log_path}")
        return 0
    finally:
        try:
            console.stop_emulator()
        finally:
            console.close()
        if not passed:
            print(f"\nruntime transcript retained at {log_path}", file=sys.stderr)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AcceptanceError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
