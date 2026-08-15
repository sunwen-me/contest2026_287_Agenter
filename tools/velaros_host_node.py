#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#
"""Deterministic ROS 2 Lyrical Topic, Service, and Action peer for VelaROS."""

from __future__ import annotations

import argparse
import ctypes
import os
import time

import rclpy
from action_msgs.msg import GoalStatus
from example_interfaces.action import Fibonacci
from geometry_msgs.msg import Twist
from rclpy.action import ActionClient, ActionServer, CancelResponse, GoalResponse
from rclpy.callback_groups import ReentrantCallbackGroup
from rclpy.executors import MultiThreadedExecutor
from std_msgs.msg import String
from std_srvs.srv import SetBool


TOPIC = "/velaros/chatter"
ACTION = "/velaros/fibonacci"


def wait_for(predicate, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        _EXECUTOR.spin_once(timeout_sec=0.1)
        if predicate():
            return True
    return False


def enable_fastdds_debug() -> None:
    if os.environ.get("VELAROS_FASTDDS_DEBUG") != "1":
        return

    library = ctypes.CDLL("libfastdds.so")
    set_verbosity = getattr(
        library,
        "_ZN8eprosima7fastdds3dds3Log12SetVerbosityENS2_4KindE",
    )
    set_verbosity.argtypes = [ctypes.c_int]
    set_verbosity.restype = None
    # Fast DDS Log::Kind::Info is the third enum value.
    set_verbosity(2)
    print("Fast DDS Info logging enabled", flush=True)


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
        _EXECUTOR.spin_once(timeout_sec=0.2)
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


def run_service_client(count: int, timeout: float) -> int:
    client = _NODE.create_client(SetBool, "/velaros/runtime/set_bridge")
    print("HOST service client ready: /velaros/runtime/set_bridge", flush=True)
    if not client.wait_for_service(timeout_sec=timeout):
        print("HOST service discovery timeout", flush=True)
        return 1

    for sequence in range(1, count + 1):
        request = SetBool.Request()
        # End in the enabled state for deterministic subsequent acceptance.
        request.data = sequence == count
        future = client.call_async(request)
        _EXECUTOR.spin_until_future_complete(future, timeout_sec=timeout)
        if not future.done() or future.exception() is not None:
            print(f"HOST service call failed: sequence={sequence}", flush=True)
            return 1
        response = future.result()
        if response is None or not response.success:
            message = "no response" if response is None else response.message
            print(
                f"HOST service rejected: sequence={sequence} message={message}",
                flush=True,
            )
            return 1
        print(
            "HOST SERVICE RESPONSE: "
            f"bridge={'enabled' if request.data else 'disabled'} "
            f"message={response.message}",
            flush=True,
        )
    print(f"HOST service complete: RESPONSES={count}", flush=True)
    return 0


def run_one_action_goal(
    client: ActionClient, order: int, cancel: bool, timeout: float
) -> bool:
    feedback_count = 0
    cancel_future = None

    def feedback_callback(message) -> None:
        nonlocal feedback_count
        feedback_count += 1
        print(
            "HOST ACTION FEEDBACK: "
            f"order={order} values={len(message.feedback.sequence)}",
            flush=True,
        )

    goal = Fibonacci.Goal()
    goal.order = order
    send_future = client.send_goal_async(goal, feedback_callback=feedback_callback)
    _EXECUTOR.spin_until_future_complete(send_future, timeout_sec=timeout)
    if not send_future.done() or send_future.exception() is not None:
        print(f"HOST Action goal send failed: order={order}", flush=True)
        return False
    goal_handle = send_future.result()
    if goal_handle is None or not goal_handle.accepted:
        print(f"HOST Action goal rejected: order={order}", flush=True)
        return False
    print(f"HOST ACTION GOAL: order={order} accepted=yes", flush=True)

    result_future = goal_handle.get_result_async()
    deadline = time.monotonic() + timeout
    while not result_future.done() and time.monotonic() < deadline:
        _EXECUTOR.spin_once(timeout_sec=0.1)
        if cancel and feedback_count > 0 and cancel_future is None:
            cancel_future = goal_handle.cancel_goal_async()
            print(f"HOST ACTION CANCEL SENT: order={order}", flush=True)

    if not result_future.done() or result_future.exception() is not None:
        print(f"HOST Action result timeout: order={order}", flush=True)
        return False
    wrapped_result = result_future.result()
    expected_status = (
        GoalStatus.STATUS_CANCELED if cancel else GoalStatus.STATUS_SUCCEEDED
    )
    if wrapped_result is None or wrapped_result.status != expected_status:
        actual = None if wrapped_result is None else wrapped_result.status
        print(
            f"HOST Action result status mismatch: expected={expected_status} actual={actual}",
            flush=True,
        )
        return False
    if cancel_future is not None:
        _EXECUTOR.spin_until_future_complete(cancel_future, timeout_sec=timeout)
        if not cancel_future.done() or cancel_future.exception() is not None:
            print(f"HOST Action cancel response failed: order={order}", flush=True)
            return False
    if feedback_count < 1:
        print(f"HOST Action received no feedback: order={order}", flush=True)
        return False
    print(
        "HOST ACTION RESULT: "
        f"order={order} status={wrapped_result.status} "
        f"values={len(wrapped_result.result.sequence)} feedback={feedback_count}",
        flush=True,
    )
    return True


def run_action_client(count: int, timeout: float) -> int:
    client = ActionClient(_NODE, Fibonacci, ACTION)
    print(f"HOST action client ready: {ACTION}", flush=True)
    try:
        if not client.wait_for_server(timeout_sec=timeout):
            print("HOST Action server discovery timeout", flush=True)
            return 1
        cases = ((8, False), (20, True))
        if count > len(cases):
            print(f"HOST Action count exceeds bounded test cases: {count}", flush=True)
            return 1
        for order, cancel in cases[:count]:
            if not run_one_action_goal(client, order, cancel, timeout):
                return 1
        print(f"HOST action client complete: RESULTS={count}", flush=True)
        return 0
    finally:
        client.destroy()


def run_action_server(count: int, timeout: float) -> int:
    completed = 0
    callback_group = ReentrantCallbackGroup()

    def goal_callback(goal_request: Fibonacci.Goal) -> GoalResponse:
        accepted = 1 <= goal_request.order <= 30
        print(
            "HOST ACTION SERVER GOAL: "
            f"order={goal_request.order} accepted={'yes' if accepted else 'no'}",
            flush=True,
        )
        return GoalResponse.ACCEPT if accepted else GoalResponse.REJECT

    def cancel_callback(_goal_handle) -> CancelResponse:
        print("HOST ACTION SERVER CANCEL: accepted=yes", flush=True)
        return CancelResponse.ACCEPT

    def execute_callback(goal_handle) -> Fibonacci.Result:
        nonlocal completed
        sequence = [0, 1]
        for _index in range(2, goal_handle.request.order):
            if goal_handle.is_cancel_requested:
                goal_handle.canceled()
                result = Fibonacci.Result()
                result.sequence = sequence
                completed += 1
                print(
                    f"HOST ACTION SERVER RESULT: status=canceled values={len(sequence)}",
                    flush=True,
                )
                return result
            sequence.append(sequence[-1] + sequence[-2])
            feedback = Fibonacci.Feedback()
            feedback.sequence = sequence
            goal_handle.publish_feedback(feedback)
            time.sleep(0.15)
        goal_handle.succeed()
        result = Fibonacci.Result()
        result.sequence = sequence
        completed += 1
        print(
            f"HOST ACTION SERVER RESULT: status=succeeded values={len(sequence)}",
            flush=True,
        )
        return result

    server = ActionServer(
        _NODE,
        Fibonacci,
        ACTION,
        execute_callback=execute_callback,
        goal_callback=goal_callback,
        cancel_callback=cancel_callback,
        callback_group=callback_group,
        result_timeout=2,
    )
    print(f"HOST action server ready: {ACTION}", flush=True)
    try:
        if not wait_for(lambda: completed >= count, timeout):
            print(
                f"HOST Action server timeout: expected={count} completed={completed}",
                flush=True,
            )
            return 1
        # The execute callback completing makes the result available, but the
        # pending GetResult request still needs another executor turn before
        # ActionServer may be destroyed.  Keep this deterministic instead of
        # racing server teardown against the DDS response writer.
        drain_deadline = time.monotonic() + 1.0
        while time.monotonic() < drain_deadline:
            _EXECUTOR.spin_once(timeout_sec=0.1)
        print(f"HOST action server complete: RESULTS={completed}", flush=True)
        return 0
    finally:
        server.destroy()


def run_one_move_relative_goal(
    client: ActionClient,
    distance: float,
    yaw: float,
    cancel: bool,
    timeout: float,
) -> bool:
    from velaros_interfaces.action import MoveRelative

    feedback_count = 0
    cancel_future = None

    def feedback_callback(message) -> None:
        nonlocal feedback_count
        feedback_count += 1
        print(
            "HOST ROBOT FEEDBACK: "
            f"traveled={message.feedback.traveled_m:.3f} "
            f"turned={message.feedback.turned_rad:.3f}",
            flush=True,
        )

    goal = MoveRelative.Goal()
    goal.distance_m = distance
    goal.yaw_rad = yaw
    goal.max_linear_speed_mps = 0.3
    goal.max_angular_speed_rps = 0.5
    send_future = client.send_goal_async(goal, feedback_callback=feedback_callback)
    _EXECUTOR.spin_until_future_complete(send_future, timeout_sec=timeout)
    if not send_future.done() or send_future.exception() is not None:
        print("HOST robot Action goal send failed", flush=True)
        return False
    goal_handle = send_future.result()
    if goal_handle is None or not goal_handle.accepted:
        print("HOST robot Action goal rejected", flush=True)
        return False
    print(
        f"HOST ROBOT GOAL: distance={distance:.3f} yaw={yaw:.3f} accepted=yes",
        flush=True,
    )

    result_future = goal_handle.get_result_async()
    deadline = time.monotonic() + timeout
    while not result_future.done() and time.monotonic() < deadline:
        _EXECUTOR.spin_once(timeout_sec=0.1)
        if cancel and feedback_count > 0 and cancel_future is None:
            cancel_future = goal_handle.cancel_goal_async()
            print("HOST ROBOT CANCEL SENT", flush=True)

    if not result_future.done() or result_future.exception() is not None:
        print("HOST robot Action result timeout", flush=True)
        return False
    wrapped = result_future.result()
    expected_ros_status = (
        GoalStatus.STATUS_CANCELED if cancel else GoalStatus.STATUS_SUCCEEDED
    )
    expected_product_status = (
        MoveRelative.Result.STATUS_CANCELED
        if cancel
        else MoveRelative.Result.STATUS_SUCCEEDED
    )
    if (
        wrapped is None
        or wrapped.status != expected_ros_status
        or wrapped.result.status != expected_product_status
    ):
        actual_ros = None if wrapped is None else wrapped.status
        actual_product = None if wrapped is None else wrapped.result.status
        print(
            "HOST robot result mismatch: "
            f"ros={actual_ros} product={actual_product}",
            flush=True,
        )
        return False
    if cancel_future is not None:
        _EXECUTOR.spin_until_future_complete(cancel_future, timeout_sec=timeout)
        if not cancel_future.done() or cancel_future.exception() is not None:
            print("HOST robot cancel response failed", flush=True)
            return False
    if feedback_count < 1:
        print("HOST robot Action received no feedback", flush=True)
        return False
    print(
        "HOST ROBOT RESULT: "
        f"ros_status={wrapped.status} product_status={wrapped.result.status} "
        f"traveled={wrapped.result.traveled_m:.3f} "
        f"turned={wrapped.result.turned_rad:.3f} feedback={feedback_count}",
        flush=True,
    )
    return True


def run_robot_client(count: int, timeout: float) -> int:
    from velaros_interfaces.action import MoveRelative

    publisher = _NODE.create_publisher(Twist, "/cmd_vel", 4)
    client = ActionClient(_NODE, MoveRelative, "/velaros/move_relative")
    print("HOST robot client ready: /cmd_vel + /velaros/move_relative", flush=True)
    try:
        if not wait_for(
            lambda: publisher.get_subscription_count() > 0
            and client.server_is_ready(),
            timeout,
        ):
            print("HOST robot communication discovery timeout", flush=True)
            return 1
        command = Twist()
        command.linear.x = 0.12
        command.angular.z = -0.08
        publisher.publish(command)
        print("HOST ROBOT CMD_VEL: linear=0.120 angular=-0.080", flush=True)
        for _index in range(5):
            _EXECUTOR.spin_once(timeout_sec=0.1)

        cases = ((0.15, 0.10, False), (1.0, -0.5, True))
        if count > len(cases):
            print(f"HOST robot goal count exceeds test cases: {count}", flush=True)
            return 1
        for distance, yaw, cancel in cases[:count]:
            if not run_one_move_relative_goal(
                client, distance, yaw, cancel, timeout
            ):
                return 1
        print(f"HOST robot client complete: RESULTS={count}", flush=True)
        return 0
    finally:
        client.destroy()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "mode",
        choices=(
            "talker",
            "listener",
            "service",
            "action-client",
            "action-server",
            "robot-client",
        ),
    )
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()
    if args.count < 1 or args.timeout <= 0:
        parser.error("--count and --timeout must be positive")

    global _EXECUTOR, _NODE
    enable_fastdds_debug()
    rclpy.init(args=None)
    _NODE = rclpy.create_node(
        f"velaros_host_{args.mode.replace('-', '_')}",
        enable_rosout=False,
        start_parameter_services=False,
        enable_logger_service=False,
    )
    _EXECUTOR = MultiThreadedExecutor(num_threads=4, context=_NODE.context)
    _EXECUTOR.add_node(_NODE)
    try:
        if args.mode == "talker":
            return run_talker(args.count, args.timeout)
        if args.mode == "service":
            return run_service_client(args.count, args.timeout)
        if args.mode == "action-client":
            return run_action_client(args.count, args.timeout)
        if args.mode == "action-server":
            return run_action_server(args.count, args.timeout)
        if args.mode == "robot-client":
            return run_robot_client(args.count, args.timeout)
        return run_listener(args.count, args.timeout)
    finally:
        _EXECUTOR.remove_node(_NODE)
        _EXECUTOR.shutdown(wait_for_threads=True)
        _NODE.destroy_node()
        rclpy.shutdown()


_NODE = None
_EXECUTOR = None

if __name__ == "__main__":
    raise SystemExit(main())
