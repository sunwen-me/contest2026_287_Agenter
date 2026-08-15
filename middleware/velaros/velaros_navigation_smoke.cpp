/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Target-side smoke for the migrated ROS-free navigation core. The pose
 * update is a bounded reference plant so this command can focus on planner,
 * controller, and command-boundary behavior; velaros_lio_smoke covers the
 * real migrated LIO snapshot producer.
 */

#include "velaros/navigation/snapshot_publisher.h"
#include "velaros/navigation/uorb_sink.h"


#include <cmath>
#include <cstdint>
#include <cstdio>

namespace
{

using namespace velaros_navigation;
using namespace velaros_navigation::velaros;

constexpr std::uint64_t kControlPeriodUs = 50000;
constexpr int kMaxIterations = 280;
constexpr double kGoalX = 1.5;
constexpr double kGoalPositionTolerance = 0.20;
constexpr float kMaxLinearSpeed = 0.8F;
constexpr float kMaxAngularSpeed = 1.5F;
constexpr std::uint32_t kCommandTimeoutMs = 250;

NavCostmapConfig MakeMapConfig()
{
  NavCostmapConfig config;
  config.width = 80;
  config.height = 80;
  config.resolution = 0.25F;
  config.fixed_origin = Vector2f(-10.0F, -10.0F);
  config.rolling_window = false;
  config.unknown_policy = UnknownSpacePolicy::Free;
  config.obstacle_min_height = -1.0F;
  config.obstacle_max_height = 2.0F;
  config.inflation_radius = 0.0F;
  config.frame.Set("map");
  return config;
}

NavigationPipelineConfig MakePipelineConfig()
{
  NavigationPipelineConfig config;
  config.planner.max_expansions = 16384;
  config.planner.max_planning_microseconds = 0;
  config.planner.allow_unknown = false;
  config.planner.allow_inflated = false;
  config.controller.max_linear_speed = 0.8F;
  config.controller.max_angular_speed = 1.5F;
  config.controller.max_linear_acceleration = 2.0F;
  config.controller.max_linear_deceleration = 2.0F;
  config.controller.max_angular_acceleration = 4.0F;
  config.controller.max_angular_deceleration = 4.0F;
  config.controller.control_period = 0.05F;
  config.controller.pose_timeout = 0.25;
  config.controller.map_timeout = 0.75;
  config.controller.goal_timeout = 30.0;
  config.controller.goal_position_tolerance =
    static_cast<float>(kGoalPositionTolerance);
  config.controller.body_frame.Set("base_link");
  config.planner_period = 0.10;
  config.replan_distance = 0.25;
  return config;
}

VelaRosNavigationAdapterConfig MakeAdapterConfig()
{
  VelaRosNavigationAdapterConfig config;
  config.map_frame.Set("map");
  config.max_linear_speed = 0.8F;
  config.max_angular_speed = 1.5F;
  config.command_timeout_ms = 250;
  return config;
}

struct NavigationSmokeState
{
  NavigationSnapshotPublisher publisher;
  NavigationPipeline pipeline;
  UorbMotionCommandSink sink;
  VelaRosNavigationAdapter adapter;
  PoseSnapshot pose;
  std::uint64_t now_us = 1000000;
  double yaw = 0.0;

  NavigationSmokeState()
    : publisher(MakeMapConfig()),
      pipeline(publisher.Buffer(), MakePipelineConfig()),
      sink(),
      adapter(
        pipeline, UorbMotionCommandSink::Sink, &sink,
        MakeAdapterConfig())
  {
    pose.frame.Set("map");
    pose.T_map_body.rotation = Matrix3d::Identity();
    pose.valid = true;
  }

  bool PublishPose()
  {
    pose.timestamp = static_cast<double>(now_us) * 1e-6;
    pose.T_map_body.rotation = RotationFromYaw(yaw);
    return publisher.Publish(pose, nullptr, 0, 1);
  }

  void ApplyCommand(const VelaRosMotionCommand & command)
  {
    constexpr double dt = 0.05;
    if (command.valid && !command.stop) {
      yaw += static_cast<double>(command.angular_z_rps) * dt;
      pose.T_map_body.translation.x() +=
        std::cos(yaw) * static_cast<double>(command.linear_x_mps) * dt;
      pose.T_map_body.translation.y() +=
        std::sin(yaw) * static_cast<double>(command.linear_x_mps) * dt;
      pose.planar_velocity = Vector2d(
        std::cos(yaw) * command.linear_x_mps,
        std::sin(yaw) * command.linear_x_mps);
      pose.yaw_rate = command.angular_z_rps;
    } else {
      pose.planar_velocity.setZero();
      pose.yaw_rate = 0.0;
    }
    now_us += kControlPeriodUs;
  }
};

int RunNavigationSmoke()
{
  static NavigationSmokeState state;
  if (!state.sink.Init()) {
    std::fprintf(stderr, "VelaROS navigation uORB sink init failed\n");
    return 1;
  }

  VelaRosGoalMessage goal;
  goal.timestamp_us = state.now_us;
  goal.x_m = static_cast<float>(kGoalX);
  goal.y_m = 0.0F;
  goal.has_yaw = 0U;
  goal.frame[0] = 'm';
  goal.frame[1] = 'a';
  goal.frame[2] = 'p';
  if (!state.adapter.ReceiveGoal(goal, state.now_us * 1e-6)) {
    std::fprintf(stderr, "VelaROS navigation goal conversion failed\n");
    state.sink.Fini();
    return 1;
  }

  bool reached = false;
  bool command_bounds = true;
  bool command_metadata = true;
  std::uint32_t previous_sequence = 0;
  int iterations = 0;
  for (; iterations < kMaxIterations; ++iterations) {
    if (!state.PublishPose() || !state.adapter.Tick(state.now_us)) {
      std::fprintf(stderr, "VelaROS navigation tick failed\n");
      state.sink.Fini();
      return 1;
    }

    const VelaRosMotionCommand command = state.adapter.LastCommand();
    command_bounds = command_bounds &&
      std::fabs(command.linear_x_mps) <= kMaxLinearSpeed + 1e-5F &&
      std::fabs(command.linear_y_mps) <= kMaxLinearSpeed + 1e-5F &&
      std::fabs(command.angular_z_rps) <= kMaxAngularSpeed + 1e-5F &&
      std::fabs(command.linear_y_mps) <= 1e-5F;
    command_metadata = command_metadata &&
      command.timestamp_us == state.now_us &&
      command.timeout_ms == kCommandTimeoutMs &&
      command.sequence > previous_sequence;
    previous_sequence = command.sequence;
    const double distance = std::hypot(
      state.pose.T_map_body.translation.x() - kGoalX,
      state.pose.T_map_body.translation.y());
    if (command.stop && distance <= kGoalPositionTolerance) {
      reached = true;
      break;
    }
    state.ApplyCommand(command);
  }

  state.now_us += 500000;
  if (!state.adapter.Tick(state.now_us)) {
    std::fprintf(stderr, "VelaROS navigation stale-snapshot tick failed\n");
    state.sink.Fini();
    return 1;
  }
  const VelaRosMotionCommand stale = state.adapter.LastCommand();
  const bool stale_stopped = stale.stop &&
    std::fabs(stale.linear_x_mps) <= 1e-5F &&
    std::fabs(stale.linear_y_mps) <= 1e-5F &&
    std::fabs(stale.angular_z_rps) <= 1e-5F;

  state.adapter.SetEmergencyStop(true);
  if (!state.PublishPose() || !state.adapter.Tick(state.now_us)) {
    std::fprintf(stderr, "VelaROS navigation emergency-stop tick failed\n");
    state.sink.Fini();
    return 1;
  }
  const VelaRosMotionCommand emergency = state.adapter.LastCommand();
  const bool stopped = emergency.stop &&
    std::fabs(emergency.linear_x_mps) <= 1e-5F &&
    std::fabs(emergency.linear_y_mps) <= 1e-5F &&
    std::fabs(emergency.angular_z_rps) <= 1e-5F;

  VelaRosMotionCommand lateral{};
  lateral.timestamp_us = state.now_us;
  lateral.linear_y_mps = 0.25F;
  lateral.timeout_ms = kCommandTimeoutMs;
  lateral.sequence = previous_sequence + 1U;
  lateral.valid = 1U;
  lateral.stop = 0U;
  const bool lateral_published = state.sink.Publish(lateral);
  const velaros_motion_command_s & lateral_result = state.sink.LastCommand();
  const bool lateral_rejected = lateral_published &&
    lateral_result.timestamp == lateral.timestamp_us &&
    lateral_result.timeout_ms == kCommandTimeoutMs &&
    lateral_result.sequence == lateral.sequence &&
    std::fabs(lateral_result.linear_x_mps) <= 1e-5F &&
    std::fabs(lateral_result.angular_z_rps) <= 1e-5F;
  state.sink.Fini();

  if (!reached || !command_bounds || !command_metadata || !stale_stopped ||
      !stopped || !lateral_rejected) {
    std::fprintf(
      stderr,
      "VelaROS navigation smoke failed: reached=%d bounds=%d metadata=%d "
      "stale=%d emergency=%d lateral=%d x=%.3f y=%.3f\n",
      reached ? 1 : 0, command_bounds ? 1 : 0,
      command_metadata ? 1 : 0, stale_stopped ? 1 : 0, stopped ? 1 : 0,
      lateral_rejected ? 1 : 0,
      state.pose.T_map_body.translation.x(),
      state.pose.T_map_body.translation.y());
    return 1;
  }

  std::printf(
    "VelaROS navigation smoke: PASS iterations=%d bounds=1 metadata=1 "
    "stale_stop=1 emergency_stop=1 lateral_reject=1 x=%.3f y=%.3f\n",
    iterations, state.pose.T_map_body.translation.x(),
    state.pose.T_map_body.translation.y());
  return 0;
}

}  // namespace

#if defined(__NuttX__)
extern "C"
#endif
int main()
{
  return RunNavigationSmoke();
}
