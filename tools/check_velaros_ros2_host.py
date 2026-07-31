#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#
"""Verify bidirectional ROS 2 Lyrical communication with the openvela simulator."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import time

from check_velaros_dds_sim import AcceptanceError, PROMPT, PtyConsole


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def emulator_console_command(port: int, command: str) -> None:
    token_path = Path.home() / ".emulator_console_auth_token"
    token = token_path.read_text(encoding="utf-8").strip()
    with socket.create_connection(("127.0.0.1", port), timeout=5) as connection:
        connection.settimeout(5)
        greeting = connection.recv(4096)
        if b"Authentication required" in greeting:
            connection.sendall(f"auth {token}\n".encode())
            response = connection.recv(4096)
            if not response.rstrip().endswith(b"OK"):
                raise AcceptanceError(
                    f"emulator console authentication failed: {response!r}"
                )
        connection.sendall(f"{command}\n".encode())
        response = connection.recv(4096)
        if not response.rstrip().endswith(b"OK"):
            raise AcceptanceError(
                f"emulator console command failed ({command}): {response!r}"
            )
        connection.sendall(b"quit\n")


class HostNode:
    def __init__(
        self, script: Path, profile: Path, mode: str, log_path: Path
    ) -> None:
        command = [
            "/bin/bash",
            "--noprofile",
            "--norc",
            "-c",
            (
                "source /opt/ros/lyrical/setup.bash && "
                "unset ROS_STATIC_PEERS && exec python3 \"$@\""
            ),
            "--",
            str(script),
            mode,
            "--count",
            "3",
            "--timeout",
            "45",
        ]
        environment = os.environ.copy()
        environment.update(
            {
                "FASTDDS_DEFAULT_PROFILES_FILE": str(profile),
                "RMW_FASTRTPS_USE_QOS_FROM_XML": "1",
                "RMW_IMPLEMENTATION": "rmw_fastrtps_cpp",
                "ROS_AUTOMATIC_DISCOVERY_RANGE": "SYSTEM_DEFAULT",
                "ROS_DOMAIN_ID": "0",
            }
        )
        self.process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=environment,
        )
        assert self.process.stdout is not None
        os.set_blocking(self.process.stdout.fileno(), False)
        self.buffer = bytearray()
        self.log = log_path.open("ab")

    def wait_for(self, marker: bytes, timeout: float) -> bytes:
        assert self.process.stdout is not None
        deadline = time.monotonic() + timeout
        while marker not in self.buffer:
            return_code = self.process.poll()
            if return_code is not None:
                raise AcceptanceError(
                    f"host node exited with {return_code} before {marker!r}:\n"
                    f"{self.buffer.decode(errors='replace')}"
                )
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AcceptanceError(f"host node timed out before {marker!r}")
            readable, _, _ = select.select(
                [self.process.stdout.fileno()], [], [], min(0.5, remaining)
            )
            if readable:
                chunk = os.read(self.process.stdout.fileno(), 65536)
                if chunk:
                    self.buffer.extend(chunk)
                    self.log.write(chunk)
                    self.log.flush()
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.buffer.flush()
        return bytes(self.buffer)

    def finish(self, timeout: float = 10) -> None:
        try:
            return_code = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            self.process.terminate()
            self.process.wait(timeout=5)
            raise AcceptanceError("host node did not exit cleanly") from error
        finally:
            self.log.close()
        if return_code != 0:
            raise AcceptanceError(f"host node exited with status {return_code}")

    def terminate(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        if not self.log.closed:
            self.log.close()


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    workspace_root = script_dir.parent.parent
    output_dir = (
        workspace_root
        / "cmake_out"
        / "contest2026_287_Agenter_goldfish-arm64-v8a-ap-fastdds"
    )
    firmware = output_dir / "nuttx"
    emulator = workspace_root / "emulator.sh"
    profile = script_dir / "velaros_host_fastdds.xml"
    host_node_script = script_dir / "velaros_host_node.py"
    runtime_log = workspace_root / "cmake_out" / "velaros-ros2-host-runtime.log"
    runtime_log.parent.mkdir(parents=True, exist_ok=True)
    runtime_log.write_bytes(b"")

    for required in (firmware, emulator, profile, host_node_script):
        if not required.exists():
            raise AcceptanceError(f"required path is missing: {required}")
    for lock_name in ("hardware-qemu.ini.lock", "multiinstance.lock"):
        lock_path = output_dir / lock_name
        if lock_path.exists():
            lock_path.unlink()

    console = PtyConsole(
        [str(emulator), str(output_dir), "-no-window", "-no-audio"],
        workspace_root,
        runtime_log,
    )
    host: HostNode | None = None
    passed = False
    try:
        boot_start = len(console.buffer)
        console.wait_until(
            "emulator console port",
            lambda data: b"control console listening on port 5554" in data,
            120,
            boot_start,
        )
        console.wait_until(
            "NuttShell prompt",
            lambda data: b"NuttShell (NSH)" in data and PROMPT.search(data),
            60,
            boot_start,
        )
        emulator_console_command(5554, "redir add udp:17410:7410")
        emulator_console_command(5554, "redir add udp:17411:7411")
        print("QEMU UDP redirects: 17410->7410, 17411->7411")

        host = HostNode(host_node_script, profile, "listener", runtime_log)
        host.wait_for(b"HOST listener ready:", 20)
        guest_talker_start = console.send(b"velaros_ros2_talker 3\n")
        guest_talker = console.wait_until(
            "guest talker completion",
            lambda data: (
                data.count(b"VelaROS SENT:") >= 3
                and b"VelaROS talker complete: SENT=3" in data
                and PROMPT.search(data)
            ),
            60,
            guest_talker_start,
        )
        host_listener = host.wait_for(b"HOST listener complete: RECEIVED=3", 60)
        host.finish()
        host = None
        if host_listener.count(b"HOST RECEIVED:") != 3:
            raise AcceptanceError("host did not receive exactly three guest samples")
        if guest_talker.count(b"VelaROS SENT:") != 3:
            raise AcceptanceError("guest did not publish exactly three samples")

        guest_listener_start = console.send(b"velaros_ros2_listener 3 &\n")
        console.wait_until(
            "guest listener readiness",
            lambda data: b"VelaROS listener ready:" in data,
            30,
            guest_listener_start,
        )
        host = HostNode(host_node_script, profile, "talker", runtime_log)
        host.wait_for(b"HOST talker complete: SENT=3", 60)
        host.finish()
        host = None
        guest_listener = console.wait_until(
            "guest listener completion",
            lambda data: (
                data.count(b"VelaROS RECEIVED:") >= 3
                and b"VelaROS listener complete: RECEIVED=3" in data
            ),
            60,
            guest_listener_start,
        )
        if guest_listener.count(b"VelaROS RECEIVED:") != 3:
            raise AcceptanceError("guest did not receive exactly three host samples")

        console.drain_for(2)
        ps_start = console.send(b"ps\n")
        process_list = console.wait_until(
            "post-interoperability process listing",
            lambda data: PROMPT.search(data),
            15,
            ps_start,
        )
        if b"velaros_ros2_talker" in process_list or b"velaros_ros2_listener" in process_list:
            raise AcceptanceError("a VelaROS ROS 2 task remains after cleanup")

        passed = True
        print("\nVelaROS <-> ROS 2 Lyrical bidirectional acceptance: PASS")
        print("guest -> host: 3 std_msgs/String samples")
        print("host -> guest: 3 std_msgs/String samples")
        print("transport: Fast DDS UDP unicast through deterministic QEMU redirects")
        print(f"ELF SHA256: {sha256(firmware)}")
        print(f"log: {runtime_log}")
        return 0
    finally:
        if host is not None:
            host.terminate()
        try:
            console.stop_emulator()
        finally:
            console.close()
        if not passed:
            print(f"runtime transcript retained at {runtime_log}", file=sys.stderr)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AcceptanceError, OSError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
