#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#
"""Verify bidirectional ROS 2 Lyrical communication with the openvela simulator."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import time

from check_velaros_dds_sim import AcceptanceError, PROMPT, PtyConsole


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify VelaROS communication against ROS 2 Lyrical"
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="simulator output directory (defaults to the development profile)",
    )
    parser.add_argument(
        "--service-only",
        action="store_true",
        help="verify the release SetBool request-response path without demo topics",
    )
    parser.add_argument(
        "--action-only",
        action="store_true",
        help="verify only bidirectional bounded Fibonacci Action communication",
    )
    parser.add_argument(
        "--robot-only",
        action="store_true",
        help="verify the /cmd_vel and MoveRelative product profile",
    )
    args = parser.parse_args()
    if sum((args.service_only, args.action_only, args.robot_only)) > 1:
        parser.error("--service-only, --action-only, and --robot-only are mutually exclusive")
    return args


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def emulator_console_command(port: int, command: str) -> None:
    token_path = Path.home() / ".emulator_console_auth_token"
    token = token_path.read_text(encoding="utf-8").strip()

    def recv_until_ok(connection: socket.socket) -> bytes:
        response = bytearray()
        while b"OK" not in response:
            chunk = connection.recv(4096)
            if not chunk:
                break
            response.extend(chunk)
        return bytes(response)

    with socket.create_connection(("127.0.0.1", port), timeout=5) as connection:
        connection.settimeout(5)
        greeting = connection.recv(4096)
        if (
            b"Authentication required" in greeting
            or b"type 'auth" in greeting
        ):
            connection.sendall(f"auth {token}\n".encode())
            response = recv_until_ok(connection)
            if b"OK" not in response:
                raise AcceptanceError(
                    f"emulator console authentication failed: {response!r}"
                )
        connection.sendall(f"{command}\n".encode())
        response = recv_until_ok(connection)
        if b"OK" not in response:
            raise AcceptanceError(
                f"emulator console command failed ({command}): {response!r}"
            )
        connection.sendall(b"quit\n")


class HostNode:
    def __init__(
        self,
        script: Path,
        profile: Path,
        mode: str,
        log_path: Path,
        extra_setup: Path | None = None,
    ) -> None:
        sample_count = (
            "2"
            if mode in ("service", "action-client", "action-server", "robot-client")
            else "3"
        )
        node_command = [
            "python3",
            str(script),
            mode,
            "--count",
            sample_count,
            "--timeout",
            "45",
        ]
        if os.environ.get("VELAROS_HOST_STRACE") == "1":
            trace_prefix = log_path.parent / "velaros-host-strace"
            node_command = [
                "strace",
                "-ff",
                "-tt",
                "-yy",
                "-s",
                "128",
                "-e",
                "trace=network",
                "-o",
                str(trace_prefix),
                *node_command,
            ]
        setup_arguments: list[str] = []
        setup_command = "source /opt/ros/lyrical/setup.bash && "
        if extra_setup is not None:
            setup_command += 'source "$1" && shift && '
            setup_arguments.append(str(extra_setup))
        command = [
            "/bin/bash",
            "--noprofile",
            "--norc",
            "-c",
            setup_command + 'unset ROS_STATIC_PEERS && exec "$@"',
            "--",
            *setup_arguments,
            *node_command,
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

    def _read_output(self) -> bool:
        """Read one available chunk and mirror it to the transcript."""
        assert self.process.stdout is not None
        try:
            chunk = os.read(self.process.stdout.fileno(), 65536)
        except BlockingIOError:
            return False
        if not chunk:
            return False
        self.buffer.extend(chunk)
        self.log.write(chunk)
        self.log.flush()
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        return True

    def _drain_output(self) -> None:
        while self._read_output():
            pass

    def wait_for(self, marker: bytes, timeout: float) -> bytes:
        assert self.process.stdout is not None
        deadline = time.monotonic() + timeout
        while marker not in self.buffer:
            return_code = self.process.poll()
            if return_code is not None:
                # A short-lived node may finish after writing the marker but
                # before the parent is scheduled again.  Consume the pipe's
                # remaining bytes before treating that as an early exit.
                self._drain_output()
                if marker in self.buffer:
                    break
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
                self._read_output()
        return bytes(self.buffer)

    def finish(self, timeout: float = 10) -> None:
        try:
            return_code = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            self.process.terminate()
            self.process.wait(timeout=5)
            raise AcceptanceError("host node did not exit cleanly") from error
        finally:
            self._drain_output()
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
        self._drain_output()
        if not self.log.closed:
            self.log.close()


def configure_guest_network(console: PtyConsole) -> None:
    address_start = console.send(b"ifconfig eth0 10.0.2.15\n")
    console.wait_until(
        "guest eth0 address configuration",
        lambda data: PROMPT.search(data) is not None,
        15,
        address_start,
    )

    ifup_start = console.send(b"ifup eth0\n")
    console.wait_until(
        "guest eth0 activation",
        lambda data: (
            b"ifup eth0...OK" in data
            and PROMPT.search(data) is not None
        ),
        15,
        ifup_start,
    )

    status_start = console.send(b"ifconfig eth0\n")
    console.wait_until(
        "guest eth0 status verification",
        lambda data: (
            b"inet addr:" in data
            and b"10.0.2.15" in data
            and b"RUNNING" in data
            and PROMPT.search(data) is not None
        ),
        15,
        status_start,
    )


def main() -> int:
    args = parse_args()
    script_dir = Path(__file__).resolve().parent
    workspace_root = script_dir.parent.parent
    default_output_dir = (
        workspace_root
        / "cmake_out"
        / "contest2026_287_Agenter_goldfish-arm64-v8a-ap-fastdds"
    )
    output_dir = args.output.resolve() if args.output else default_output_dir
    firmware = output_dir / "nuttx"
    emulator = workspace_root / "emulator.sh"
    profile = script_dir / "velaros_host_fastdds.xml"
    host_node_script = script_dir / "velaros_host_node.py"
    host_interface_builder = script_dir / "build_velaros_host_interfaces.sh"
    host_interface_setup = (
        workspace_root / "cmake_out/velaros-host-interfaces/install/setup.bash"
    )
    runtime_log = workspace_root / "cmake_out" / "velaros-ros2-host-runtime.log"
    host_log = workspace_root / "cmake_out" / "velaros-ros2-host-node.log"
    runtime_log.parent.mkdir(parents=True, exist_ok=True)
    runtime_log.write_bytes(b"")
    host_log.write_bytes(b"")

    if args.robot_only:
        subprocess.run([str(host_interface_builder)], check=True)

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
        configure_guest_network(console)
        print("Guest eth0: 10.0.2.15/RUNNING")
        emulator_console_command(5554, "redir add udp:17410:7410")
        emulator_console_command(5554, "redir add udp:17411:7411")
        emulator_console_command(5554, "redir add udp:17412:7412")
        emulator_console_command(5554, "redir add udp:17413:7413")
        print(
            "QEMU UDP redirects: 17410->7410, 17411->7411, "
            "17412->7412, 17413->7413"
        )

        if not args.service_only and not args.action_only and not args.robot_only:
            host = HostNode(host_node_script, profile, "listener", host_log)
            host.wait_for(b"HOST listener ready:", 20)
            guest_talker_start = console.send(b"velaros_talker 3\n")
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
            host_listener = host.wait_for(
                b"HOST listener complete: RECEIVED=3", 60
            )
            host.finish()
            host = None
            if host_listener.count(b"HOST RECEIVED:") != 3:
                raise AcceptanceError("host did not receive exactly three guest samples")
            if guest_talker.count(b"VelaROS SENT:") != 3:
                raise AcceptanceError("guest did not publish exactly three samples")

            guest_listener_start = console.send(b"velaros_listener 3 &\n")
            console.wait_until(
                "guest listener readiness",
                lambda data: b"VelaROS listener ready:" in data,
                30,
                guest_listener_start,
            )
            host = HostNode(host_node_script, profile, "talker", host_log)
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

        if not args.action_only and not args.robot_only:
            guest_service_start = console.send(b"velaros_bridge_service 2 &\n")
            console.wait_until(
                "guest service readiness",
                lambda data: b"VelaROS service ready:" in data,
                30,
                guest_service_start,
            )
            host = HostNode(host_node_script, profile, "service", host_log)
            host_service = host.wait_for(b"HOST service complete: RESPONSES=2", 60)
            host.finish()
            host = None
            guest_service = console.wait_until(
                "guest service completion",
                lambda data: (
                    data.count(b"VelaROS SERVICE REQUEST:") >= 2
                    and b"VelaROS service complete: REQUESTS=2" in data
                ),
                60,
                guest_service_start,
            )
            if host_service.count(b"HOST SERVICE RESPONSE:") != 2:
                raise AcceptanceError("host did not receive exactly two service responses")
            if guest_service.count(b"VelaROS SERVICE REQUEST:") != 2:
                raise AcceptanceError("guest did not handle exactly two service requests")

        if not args.service_only and not args.robot_only:
            guest_action_server_start = console.send(b"velaros_action_server 2 &\n")
            console.wait_until(
                "guest Action server readiness",
                lambda data: b"VelaROS Action server ready:" in data,
                45,
                guest_action_server_start,
            )
            host = HostNode(host_node_script, profile, "action-client", host_log)
            host_action_client = host.wait_for(
                b"HOST action client complete: RESULTS=2", 90
            )
            host.finish()
            host = None
            guest_action_server = console.wait_until(
                "guest Action server completion",
                lambda data: (
                    data.count(b"VelaROS ACTION RESULT:") >= 2
                    and b"VelaROS Action server complete: RESULTS=2" in data
                ),
                90,
                guest_action_server_start,
            )
            if host_action_client.count(b"HOST ACTION RESULT:") != 2:
                raise AcceptanceError("host did not receive exactly two Action results")
            if guest_action_server.count(b"VelaROS ACTION RESULT:") != 2:
                raise AcceptanceError("guest Action server did not return two results")

            host = HostNode(host_node_script, profile, "action-server", host_log)
            host.wait_for(b"HOST action server ready:", 30)
            guest_action_normal_start = console.send(b"velaros_action_client 8 0\n")
            host.wait_for(
                b"HOST ACTION SERVER RESULT: status=succeeded", 90
            )
            guest_action_normal = console.wait_until(
                "guest Action client successful result",
                lambda data: (
                    b"VelaROS ACTION CLIENT RESULT: status=4" in data
                    and b"VelaROS Action client complete:" in data
                    and b"STATUS=4" in data
                    and PROMPT.search(data)
                ),
                90,
                guest_action_normal_start,
            )
            guest_action_cancel_start = console.send(
                b"velaros_action_client 20 1 1\n"
            )
            host.wait_for(
                b"HOST ACTION SERVER RESULT: status=canceled", 90
            )
            guest_action_cancel = console.wait_until(
                "guest Action client canceled result",
                lambda data: (
                    b"VelaROS ACTION CLIENT CANCEL RESPONSE:" in data
                    and b"VelaROS ACTION CLIENT RESULT: status=5" in data
                    and b"VelaROS Action client complete:" in data
                    and b"STATUS=5" in data
                    and PROMPT.search(data)
                ),
                90,
                guest_action_cancel_start,
            )
            host_action_server = host.wait_for(
                b"HOST action server complete: RESULTS=2", 90
            )
            host.finish()
            host = None
            if b"VelaROS ACTION CLIENT FEEDBACK:" not in guest_action_normal:
                raise AcceptanceError("guest Action client received no normal feedback")
            if b"VelaROS ACTION CLIENT FEEDBACK:" not in guest_action_cancel:
                raise AcceptanceError("guest Action client received no cancel feedback")
            if host_action_server.count(b"HOST ACTION SERVER RESULT:") != 2:
                raise AcceptanceError("host Action server did not complete two goals")

        if args.robot_only:
            guest_robot_start = console.send(
                b"velaros_robot_node --qemu-interop --cmd-vel 1 --goals 2 &\n"
            )
            console.wait_until(
                "guest robot product node readiness",
                lambda data: b"VelaROS product node ready:" in data,
                45,
                guest_robot_start,
            )
            host = HostNode(
                host_node_script,
                profile,
                "robot-client",
                host_log,
                host_interface_setup,
            )
            host_robot = host.wait_for(
                b"HOST robot client complete: RESULTS=2", 90
            )
            host.finish()
            host = None
            guest_robot = console.wait_until(
                "guest robot product completion",
                lambda data: (
                    b"VelaROS product node: PASS" in data
                    and data.count(b"VelaROS MoveRelative completed: PASS") >= 1
                    and data.count(b"VelaROS MoveRelative canceled: PASS") >= 1
                ),
                90,
                guest_robot_start,
            )
            if host_robot.count(b"HOST ROBOT RESULT:") != 2:
                raise AcceptanceError("host did not receive two product Action results")
            if b"VelaROS /cmd_vel -> uORB: PASS" not in guest_robot:
                raise AcceptanceError("guest did not bridge /cmd_vel into uORB")

        console.drain_for(2)
        ps_start = console.send(b"ps\n")
        process_list = console.wait_until(
            "post-interoperability process listing",
            lambda data: PROMPT.search(data),
            15,
            ps_start,
        )
        if (
            (
                not args.action_only
                and not args.robot_only
                and b"velaros_bridge_service" in process_list
            )
            or (
                not args.service_only
                and not args.robot_only
                and (
                    b"velaros_talker" in process_list
                    or b"velaros_listener" in process_list
                    or b"velaros_action_server" in process_list
                    or b"velaros_action_client" in process_list
                )
            )
            or (args.robot_only and b"velaros_robot_node" in process_list)
        ):
            raise AcceptanceError("a VelaROS ROS 2 task remains after cleanup")

        passed = True
        print("\nVelaROS <-> ROS 2 Lyrical communication acceptance: PASS")
        if not args.service_only and not args.action_only and not args.robot_only:
            print("guest -> host: 3 std_msgs/String samples")
            print("host -> guest: 3 std_msgs/String samples")
        if not args.action_only and not args.robot_only:
            print("host -> guest: 2 std_srvs/SetBool request-response calls")
        if not args.service_only and not args.robot_only:
            print("guest Action server -> host client: succeeded + canceled goals")
            print("host Action server -> guest client: succeeded + canceled goals")
        if args.robot_only:
            print("host -> guest: geometry_msgs/Twist /cmd_vel -> openVela uORB")
            print("host -> guest: MoveRelative succeeded + canceled product goals")
        print("transport: Fast DDS UDP unicast through deterministic QEMU redirects")
        print(f"ELF SHA256: {sha256(firmware)}")
        print(f"guest log: {runtime_log}")
        print(f"host log: {host_log}")
        return 0
    finally:
        if host is not None:
            host.terminate()
        try:
            console.stop_emulator()
        finally:
            console.close()
        if not passed:
            print(f"guest transcript retained at {runtime_log}", file=sys.stderr)
            print(f"host transcript retained at {host_log}", file=sys.stderr)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AcceptanceError, OSError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
