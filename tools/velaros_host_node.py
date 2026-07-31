#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#
"""Deterministic ROS 2 Lyrical std_msgs/String peer for VelaROS acceptance."""

from __future__ import annotations

import argparse
import time

import rclpy
from std_msgs.msg import String


TOPIC = "/velaros/chatter"


def wait_for(predicate, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        rclpy.spin_once(_NODE, timeout_sec=0.1)
        if predicate():
            return True
    return False


def run_talker(count: int, timeout: float) -> int:
    publisher = _NODE.create_publisher(String, TOPIC, 10)
    print(f"HOST talker ready: {TOPIC}", flush=True)
    if not wait_for(lambda: publisher.get_subscription_count() > 0, timeout):
        print("HOST talker match timeout", flush=True)
        return 1
    print(
        f"HOST talker matched {publisher.get_subscription_count()} subscriber(s)",
        flush=True,
    )
    for sequence in range(1, count + 1):
        message = String()
        message.data = f"Hello from ROS 2 Lyrical host #{sequence}"
        publisher.publish(message)
        print(f"HOST SENT: {message.data}", flush=True)
        rclpy.spin_once(_NODE, timeout_sec=0.2)
    time.sleep(0.5)
    print(f"HOST talker complete: SENT={count}", flush=True)
    return 0


def run_listener(count: int, timeout: float) -> int:
    received: list[str] = []

    def callback(message: String) -> None:
        received.append(message.data)
        print(f"HOST RECEIVED: {message.data}", flush=True)

    _NODE.create_subscription(String, TOPIC, callback, 10)
    print(f"HOST listener ready: {TOPIC}", flush=True)
    if not wait_for(lambda: len(received) >= count, timeout):
        print(
            f"HOST listener timeout: expected={count} received={len(received)}",
            flush=True,
        )
        return 1
    print(f"HOST listener complete: RECEIVED={len(received)}", flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("talker", "listener"))
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()
    if args.count < 1 or args.timeout <= 0:
        parser.error("--count and --timeout must be positive")

    global _NODE
    rclpy.init(args=None)
    _NODE = rclpy.create_node(f"velaros_host_{args.mode}")
    try:
        if args.mode == "talker":
            return run_talker(args.count, args.timeout)
        return run_listener(args.count, args.timeout)
    finally:
        _NODE.destroy_node()
        rclpy.shutdown()


_NODE = None

if __name__ == "__main__":
    raise SystemExit(main())
