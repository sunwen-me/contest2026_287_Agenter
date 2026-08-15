#pragma once

#include "velaros/navigation/snapshot_publisher.h"
#include "x86_lio_slam/backend/pose_graph_backend.h"
#include "x86_lio_slam/common/types.h"
#include "x86_lio_slam/frontend/small_point_lio_frontend.h"
#include "x86_lio_slam/system/slam_system.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace velaros_lio {

#ifndef VELAROS_LIO_MAX_CLOUD_POINTS
#define VELAROS_LIO_MAX_CLOUD_POINTS 4096
#endif

#ifndef VELAROS_LIO_MAX_GLOBAL_POINTS
#define VELAROS_LIO_MAX_GLOBAL_POINTS 16384
#endif

#ifndef VELAROS_LIO_MAX_LOCAL_VOXELS
#define VELAROS_LIO_MAX_LOCAL_VOXELS 32768
#endif

struct ImuSample {
    std::uint64_t timestamp_us = 0;
    float angular_velocity[3]{};
    float linear_acceleration[3]{};
};

struct PointSample {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float intensity = 0.0F;
    std::uint64_t timestamp_us = 0;
};

class Runtime final {
public:
    static constexpr std::size_t kMaxCloudPoints =
        VELAROS_LIO_MAX_CLOUD_POINTS;
    static constexpr std::size_t kMaxGlobalPoints =
        VELAROS_LIO_MAX_GLOBAL_POINTS;

    explicit Runtime(velaros_navigation::NavigationSnapshotPublisher& publisher);

    bool AddImu(const ImuSample& sample);

    bool AddPointCloud(const PointSample* points, std::size_t count);

    // Processes all queued sensor records and publishes a current pose/map
    // snapshot when the source LIO has a valid output.
    bool Process();

    void Reset();

    bool Initialized() const { return frontend_.Initialized(); }

    bool HasOutput() const { return has_output_; }

    std::size_t LastMapPointCount() const { return map_point_count_; }

    const x86_lio_slam::OdometryState& LastState() const { return last_state_; }

    const x86_lio_slam::SlamSystem& System() const { return system_; }

private:
    static x86_lio_slam::LioConfig MakeLioConfig();

    static x86_lio_slam::SlamSystemConfig MakeSystemConfig();

    bool PublishSnapshot(const x86_lio_slam::OdometryState& state);

    std::array<x86_lio_slam::PointXYZIT, kMaxCloudPoints> cloud_buffer_{};
    std::array<velaros_navigation::Vector3f, kMaxGlobalPoints>
        map_buffer_{};
    x86_lio_slam::SmallPointLioFrontend frontend_;
    x86_lio_slam::DensePoseGraphBackend backend_;
    x86_lio_slam::SlamSystem system_;
    velaros_navigation::NavigationSnapshotPublisher* publisher_ = nullptr;
    x86_lio_slam::OdometryState last_state_{};
    std::size_t map_point_count_ = 0;
    bool has_output_ = false;
};

}  // namespace velaros_lio
