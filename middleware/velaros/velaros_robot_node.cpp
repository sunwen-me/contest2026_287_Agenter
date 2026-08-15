/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * VelaROS mobile-base product profile.
 *
 * A standard ROS 2 /cmd_vel Topic and the bounded MoveRelative Action share
 * one openVela uORB command boundary.  The simulator integrates commanded
 * motion as a deterministic reference plant; a board driver can consume the
 * same velaros_motion_command topic and replace that progress source with
 * wheel odometry without changing the ROS wire interface.
 */

#include <algorithm>
#include <cinttypes>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>

#include "geometry_msgs/msg/twist.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "velaros/uorb_bridge.h"
#include "velaros/uorb_topics.h"

namespace
{

using MoveRelative = velaros::action::MoveRelative;
using GoalHandle = rclcpp_action::ServerGoalHandle<MoveRelative>;
using Twist = geometry_msgs::msg::Twist;

constexpr char kCmdVelTopic[] = "/cmd_vel";
constexpr char kMoveRelativeAction[] = "/velaros/move_relative";
constexpr float kControlPeriodSeconds = 0.05F;
constexpr float kPositionEpsilon = 0.0005F;
constexpr float kMaxLinearSpeed = 2.0F;
constexpr float kMaxAngularSpeed = 4.0F;
constexpr uint32_t kCommandTimeoutMs = 250;

void log_init_stage(const char * stage)
{
  std::printf("VelaROS robot init: %s\n", stage);
  std::fflush(stdout);
}

struct GoalExecution
{
  float traveled_m;
  float turned_rad;
  bool initialized;
};

struct RobotState
{
  velaros_ros_to_uorb_bridge_t bridge;
  velaros_motion_command_s command;
  GoalExecution goals[CONFIG_VELAROS_ACTION_MAX_GOALS];
  uint32_t sequence;
  size_t cmd_vel_received;
  size_t completed_goals;
  size_t feedback_count;
  size_t required_cmd_vel;
  size_t required_goals;
  bool action_owns_control;
  bool qemu_interop;
  bool failed;
};

uint64_t monotonic_microseconds()
{
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(now.tv_sec) * 1000000ULL +
    static_cast<uint64_t>(now.tv_nsec) / 1000ULL;
}

bool finite_twist(const Twist & message)
{
  return std::isfinite(message.linear.x) &&
    std::isfinite(message.linear.y) &&
    std::isfinite(message.linear.z) &&
    std::isfinite(message.angular.x) &&
    std::isfinite(message.angular.y) &&
    std::isfinite(message.angular.z);
}

bool twist_to_uorb(
  const void * ros_message, void * uorb_message, void * user_data)
{
  const auto & twist = *static_cast<const Twist *>(ros_message);
  auto & command = *static_cast<velaros_motion_command_s *>(uorb_message);
  auto & state = *static_cast<RobotState *>(user_data);

  if (!finite_twist(twist) ||
      std::fabs(twist.linear.x) > kMaxLinearSpeed ||
      std::fabs(twist.angular.z) > kMaxAngularSpeed) {
    return false;
  }

  command.timestamp = monotonic_microseconds();
  command.linear_x_mps = static_cast<float>(twist.linear.x);
  command.angular_z_rps = static_cast<float>(twist.angular.z);
  command.timeout_ms = kCommandTimeoutMs;
  command.sequence = ++state.sequence;
  return true;
}

bool publish_motion(RobotState & state, float linear_x, float angular_z)
{
  Twist command{};
  command.linear.x = linear_x;
  command.angular.z = angular_z;
  velaros_ros_to_uorb_bridge_callback(&command, &state.bridge);
  if (state.bridge.last_result != VELAROS_BRIDGE_OK) {
    state.failed = true;
    return false;
  }
  return true;
}

void on_cmd_vel(const Twist & command, void * user_data)
{
  auto & state = *static_cast<RobotState *>(user_data);
  if (state.action_owns_control) {
    std::printf("VelaROS /cmd_vel ignored: MoveRelative owns control\n");
    return;
  }

  velaros_ros_to_uorb_bridge_callback(&command, &state.bridge);
  if (state.bridge.last_result != VELAROS_BRIDGE_OK) {
    state.failed = true;
    std::fprintf(stderr, "VelaROS /cmd_vel validation or uORB publish failed\n");
    return;
  }

  ++state.cmd_vel_received;
  std::printf(
    "VelaROS /cmd_vel -> uORB: PASS linear=%.3f angular=%.3f sequence=%" PRIu32 "\n",
    state.command.linear_x_mps, state.command.angular_z_rps,
    state.command.sequence);
}

bool valid_goal(const MoveRelative::Goal & goal)
{
  if (!std::isfinite(goal.distance_m) || !std::isfinite(goal.yaw_rad) ||
      !std::isfinite(goal.max_linear_speed_mps) ||
      !std::isfinite(goal.max_angular_speed_rps)) {
    return false;
  }
  if (std::fabs(goal.distance_m) <= kPositionEpsilon &&
      std::fabs(goal.yaw_rad) <= kPositionEpsilon) {
    return false;
  }
  if ((std::fabs(goal.distance_m) > kPositionEpsilon &&
       goal.max_linear_speed_mps <= 0.0F) ||
      (std::fabs(goal.yaw_rad) > kPositionEpsilon &&
       goal.max_angular_speed_rps <= 0.0F)) {
    return false;
  }
  return goal.max_linear_speed_mps <= kMaxLinearSpeed &&
    goal.max_angular_speed_rps <= kMaxAngularSpeed;
}

rclcpp_action::GoalResponse on_goal(
  const rclcpp_action::GoalUUID &,
  const MoveRelative::Goal & goal,
  void * user_data)
{
  auto & state = *static_cast<RobotState *>(user_data);
  if (state.action_owns_control || !valid_goal(goal)) {
    return rclcpp_action::GoalResponse::REJECT;
  }
  state.action_owns_control = true;
  std::printf(
    "VelaROS MoveRelative goal accepted: distance=%.3f yaw=%.3f\n",
    goal.distance_m, goal.yaw_rad);
  return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
}

rclcpp_action::CancelResponse on_cancel(GoalHandle &, void *)
{
  return rclcpp_action::CancelResponse::ACCEPT;
}

float advance(float current, float target, float speed)
{
  const float remaining = target - current;
  const float step = std::min(std::fabs(remaining), speed * kControlPeriodSeconds);
  return current + std::copysign(step, remaining);
}

void finish_goal(
  RobotState & state, GoalExecution & execution, GoalHandle & handle,
  int8_t status, bool canceled)
{
  (void)publish_motion(state, 0.0F, 0.0F);
  MoveRelative::Result result{};
  result.status = status;
  result.traveled_m = execution.traveled_m;
  result.turned_rad = execution.turned_rad;
  if (canceled) {
    handle.canceled(result);
    std::printf("VelaROS MoveRelative canceled: PASS\n");
  } else {
    handle.succeed(result);
    std::printf("VelaROS MoveRelative completed: PASS\n");
  }
  execution = GoalExecution{};
  state.action_owns_control = false;
  ++state.completed_goals;
}

void on_execute(GoalHandle & handle, void * user_data)
{
  auto & state = *static_cast<RobotState *>(user_data);
  GoalExecution & execution = state.goals[handle.slot_index()];
  const MoveRelative::Goal & goal = handle.get_goal();
  if (!execution.initialized) {
    execution = GoalExecution{};
    execution.initialized = true;
  }

  if (handle.is_canceling()) {
    finish_goal(
      state, execution, handle,
      velaros_interfaces__action__MoveRelative_Result__STATUS_CANCELED, true);
    return;
  }

  execution.traveled_m = advance(
    execution.traveled_m, goal.distance_m, goal.max_linear_speed_mps);
  execution.turned_rad = advance(
    execution.turned_rad, goal.yaw_rad, goal.max_angular_speed_rps);

  const float remaining_distance = goal.distance_m - execution.traveled_m;
  const float remaining_yaw = goal.yaw_rad - execution.turned_rad;
  const float linear_velocity =
    std::fabs(remaining_distance) > kPositionEpsilon ?
    std::copysign(goal.max_linear_speed_mps, remaining_distance) : 0.0F;
  const float angular_velocity =
    std::fabs(remaining_yaw) > kPositionEpsilon ?
    std::copysign(goal.max_angular_speed_rps, remaining_yaw) : 0.0F;
  if (!publish_motion(state, linear_velocity, angular_velocity)) {
    MoveRelative::Result result{};
    result.status =
      velaros_interfaces__action__MoveRelative_Result__STATUS_ACTUATION_ERROR;
    result.traveled_m = execution.traveled_m;
    result.turned_rad = execution.turned_rad;
    handle.abort(result);
    execution = GoalExecution{};
    state.action_owns_control = false;
    ++state.completed_goals;
    return;
  }

  MoveRelative::Feedback feedback{};
  feedback.traveled_m = execution.traveled_m;
  feedback.turned_rad = execution.turned_rad;
  feedback.remaining_distance_m = remaining_distance;
  feedback.remaining_yaw_rad = remaining_yaw;
  handle.publish_feedback(feedback);
  ++state.feedback_count;

  if (std::fabs(remaining_distance) <= kPositionEpsilon &&
      std::fabs(remaining_yaw) <= kPositionEpsilon) {
    finish_goal(
      state, execution, handle,
      velaros_interfaces__action__MoveRelative_Result__STATUS_SUCCEEDED, false);
  }
}

bool parse_count(int argc, char * argv[], const char * option, size_t & value)
{
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::strcmp(argv[index], option) == 0) {
      char * end = nullptr;
      const unsigned long parsed = std::strtoul(argv[index + 1], &end, 10);
      if (end == argv[index + 1] || *end != '\0') {
        return false;
      }
      value = static_cast<size_t>(parsed);
    }
  }
  return true;
}

void parse_transport(int argc, char * argv[], RobotState & state)
{
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--qemu-interop") == 0) {
      state.qemu_interop = true;
    }
  }
}

int run_robot_node(int argc, char * argv[])
{
  log_init_stage("entry");
  RobotState state{};
  state.bridge = velaros_ros_to_uorb_bridge_get_zero_initialized();
  parse_transport(argc, argv, state);
  if (!parse_count(argc, argv, "--cmd-vel", state.required_cmd_vel) ||
      !parse_count(argc, argv, "--goals", state.required_goals)) {
    std::fprintf(
      stderr,
      "usage: velaros_robot_node [--qemu-interop] [--cmd-vel N] [--goals N]\n");
    return 2;
  }
  log_init_stage("uORB bridge");
  if (velaros_ros_to_uorb_bridge_init(
      &state.bridge, ORB_ID(velaros_motion_command), &state.command,
      twist_to_uorb, &state) != VELAROS_BRIDGE_OK) {
    std::fprintf(stderr, "VelaROS robot uORB bridge init failed\n");
    return 1;
  }
  log_init_stage("uORB bridge ready");

  int result = 0;
  {
    log_init_stage("rclcpp context");
    rclcpp::ContextOptions context_options;
    context_options.qemu_interop = state.qemu_interop;
    context_options.qemu_participant_id = 0;
    rclcpp::Context context(context_options);
    log_init_stage("rclcpp node");
    rclcpp::Node node(context, "velaros_robot_node", "/velaros");
    log_init_stage("/cmd_vel subscription");
    auto cmd_vel = node.create_subscription<Twist>(
      kCmdVelTopic, rclcpp::QoS(4).best_effort(), on_cmd_vel, &state);
    log_init_stage("MoveRelative action server");
    rclcpp_action::Server<MoveRelative>::Options action_options;
    action_options.result_timeout_ns = RCL_S_TO_NS(2);
    auto move_relative = rclcpp_action::create_server<MoveRelative>(
      node, kMoveRelativeAction, on_goal, on_cancel, on_execute,
      &state, action_options);

    log_init_stage("executor");
    rclcpp::ExecutorOptions executor_options;
    executor_options.subscriptions = 1;
    executor_options.action_servers = 1;
    rclcpp::executors::SingleThreadedExecutor executor(
      context, executor_options);
    executor.add_subscription(*cmd_vel);
    executor.add_action_server(*move_relative);
    log_init_stage("executor ready");

    std::printf(
      "VelaROS product node ready: %s + %s -> uORB\n",
      kCmdVelTopic, kMoveRelativeAction);
    int drain_iterations = -1;
    while (context.ok()) {
      (void)executor.spin_once(std::chrono::milliseconds(50));
      if (state.failed) {
        result = 1;
        break;
      }
      const bool acceptance_mode =
        state.required_cmd_vel > 0 || state.required_goals > 0;
      const bool complete = acceptance_mode && !state.action_owns_control &&
        state.cmd_vel_received >= state.required_cmd_vel &&
        state.completed_goals >= state.required_goals;
      if (complete && drain_iterations < 0) {
        drain_iterations = 20;
      }
      if (drain_iterations == 0) {
        break;
      }
      if (drain_iterations > 0) {
        --drain_iterations;
      }
    }
  }

  if (velaros_ros_to_uorb_bridge_fini(&state.bridge) != VELAROS_BRIDGE_OK) {
    result = 1;
  }
  if (result == 0) {
    std::printf(
      "VelaROS product node: PASS cmd_vel=%zu goals=%zu feedback=%zu uorb=%" PRIu32 "\n",
      state.cmd_vel_received, state.completed_goals,
      state.feedback_count, state.sequence);
  }
  return result;
}

}  // namespace

#if defined(__NuttX__)
extern "C"
#endif
int main(int argc, char * argv[])
{
  try {
    return run_robot_node(argc, argv);
  } catch (const std::exception & error) {
    std::fprintf(stderr, "VelaROS product node failed: %s\n", error.what());
    return 1;
  }
}
