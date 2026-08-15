/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Target-side smoke for the migrated Small Point-LIO runtime. The synthetic
 * sensor records exercise the same fixed POD ingress used by a future uORB
 * IMU/LiDAR driver; pose and map assertions come from the LIO system itself.
 */

#include "velaros/lio_runtime.h"
#include "velaros/navigation/uorb_sink.h"
#include "velaros/navigation/velaros_navigation_adapter.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>

namespace
{

using velaros_lio::ImuSample;
using velaros_lio::PointSample;
using velaros_lio::Runtime;
using namespace velaros_navigation;
using namespace velaros_navigation::velaros;

constexpr std::size_t kCloudPointCount = 64;
constexpr std::size_t kCloudSide = 8;
constexpr std::size_t kFrameCount = 12;
constexpr std::uint64_t kFirstTimestampUs = 1000000;
constexpr std::uint64_t kFramePeriodUs = 50000;

NavCostmapConfig MakeMapConfig()
{
  NavCostmapConfig config;
  config.width = 64;
  config.height = 64;
  config.resolution = 0.25F;
  config.fixed_origin = Vector2f(-4.0F, -4.0F);
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
  config.controller.goal_position_tolerance = 0.20F;
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

struct LioSmokeState
{
  NavigationSnapshotPublisher publisher;
  Runtime runtime;
  NavigationPipeline pipeline;
  UorbMotionCommandSink sink;
  VelaRosNavigationAdapter adapter;
  std::array<PointSample, kCloudPointCount> cloud{};

  LioSmokeState()
    : publisher(MakeMapConfig()),
      runtime(publisher),
      pipeline(publisher.Buffer(), MakePipelineConfig()),
      sink(),
      adapter(
        pipeline, UorbMotionCommandSink::Sink, &sink,
        MakeAdapterConfig())
  {
  }
};

void FillCloud(std::array<PointSample, kCloudPointCount> & cloud,
               std::uint64_t timestamp_us)
{
  for (std::size_t row = 0; row < kCloudSide; ++row) {
    for (std::size_t column = 0; column < kCloudSide; ++column) {
      const std::size_t index = row * kCloudSide + column;
      cloud[index].x = 2.0F + static_cast<float>(column) * 0.25F;
      cloud[index].y = -0.875F + static_cast<float>(row) * 0.25F;
      cloud[index].z = 0.0F;
      cloud[index].intensity = 1.0F;
      cloud[index].timestamp_us = timestamp_us;
    }
  }
}

int RunLioSmoke()
{
  static LioSmokeState state;
  state.runtime.Reset();
  state.pipeline.Reset();

  if (!state.sink.Init()) {
    std::fprintf(stderr, "VelaROS LIO smoke uORB sink init failed\n");
    return 1;
  }

  VelaRosGoalMessage goal;
  goal.timestamp_us = kFirstTimestampUs;
  goal.x_m = 0.75F;
  goal.y_m = 0.0F;
  goal.frame[0] = 'm';
  goal.frame[1] = 'a';
  goal.frame[2] = 'p';
  if (!state.adapter.ReceiveGoal(goal,
                                 static_cast<double>(kFirstTimestampUs) *
                                   1e-6)) {
    std::fprintf(stderr, "VelaROS LIO smoke navigation goal failed\n");
    state.sink.Fini();
    return 1;
  }

  std::size_t processed_frames = 0;
  std::size_t navigation_commands = 0;
  std::size_t map_points = 0;
  std::uint64_t map_version = 0;
  try {
    for (std::size_t frame = 0; frame < kFrameCount; ++frame) {
      const std::uint64_t timestamp_us =
        kFirstTimestampUs + frame * kFramePeriodUs;
      FillCloud(state.cloud, timestamp_us);

      ImuSample imu;
      imu.timestamp_us = timestamp_us;
      imu.linear_acceleration[2] = 9.81F;
      if (!state.runtime.AddImu(imu) ||
          !state.runtime.AddPointCloud(state.cloud.data(), state.cloud.size()) ||
          !state.runtime.Process()) {
        std::fprintf(stderr,
                     "VelaROS LIO smoke input/process failed at frame=%zu\n",
                     frame);
        state.sink.Fini();
        return 1;
      }

      const auto & output = state.runtime.LastState();
      const auto guard = state.publisher.AcquireLatest();
      if (!state.runtime.HasOutput() || !output.valid || !guard.Valid()) {
        std::fprintf(stderr,
                     "VelaROS LIO smoke invalid pose snapshot at frame=%zu\n",
                     frame);
        state.sink.Fini();
        return 1;
      }
      const NavigationSnapshotView snapshot = guard.View();
      if (!snapshot.pose.valid || !snapshot.map.IsValid() ||
          snapshot.map.version == 0 ||
          state.runtime.LastMapPointCount() == 0 ||
          state.runtime.System().GlobalMapVersion() == 0) {
        std::fprintf(stderr,
                     "VelaROS LIO smoke invalid map snapshot at frame=%zu\n",
                     frame);
        state.sink.Fini();
        return 1;
      }

      if (!state.adapter.Tick(timestamp_us) ||
          state.adapter.LastCommand().sequence == 0U ||
          state.sink.LastCommand().timestamp != timestamp_us) {
        std::fprintf(stderr,
                     "VelaROS LIO smoke navigation/uORB failed at frame=%zu\n",
                     frame);
        state.sink.Fini();
        return 1;
      }

      ++processed_frames;
      ++navigation_commands;
      map_points = state.runtime.LastMapPointCount();
      map_version = state.runtime.System().GlobalMapVersion();
    }
  } catch (const std::exception & error) {
    std::fprintf(stderr, "VelaROS LIO smoke exception: %s\n", error.what());
    state.sink.Fini();
    return 1;
  }

  state.sink.Fini();
  std::printf("VelaROS LIO smoke: PASS frames=%zu map_points=%zu map_version=%llu "
              "navigation_commands=%zu\n",
              processed_frames, map_points,
              static_cast<unsigned long long>(map_version),
              navigation_commands);
  return 0;
}

}  // namespace

#if defined(__NuttX__)
extern "C"
#endif
int main()
{
  return RunLioSmoke();
}
