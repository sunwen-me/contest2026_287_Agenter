#include "velaros/lio_runtime.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace velaros_lio {

namespace {

double SecondsFromMicros(std::uint64_t timestamp_us) {
    return static_cast<double>(timestamp_us) * 1e-6;
}

}  // namespace

x86_lio_slam::LioConfig Runtime::MakeLioConfig() {
    x86_lio_slam::LioConfig config;
    config.min_distance = 0.05F;
    config.max_distance = 80.0F;
    config.point_filter_num = 1;
    config.space_downsample = true;
    config.space_downsample_leaf_size = 0.15F;
    config.map_resolution = 0.15F;
    config.map_capacity = VELAROS_LIO_MAX_LOCAL_VOXELS;
    config.init_map_size = 10;
    config.gravity = Eigen::Vector3d(0.0, 0.0, -9.81);
    config.fix_gravity_direction = false;
    config.point_kernel_backend = x86_lio_slam::PointKernelBackend::Scalar;
    return config;
}

x86_lio_slam::SlamSystemConfig Runtime::MakeSystemConfig() {
    x86_lio_slam::SlamSystemConfig config;
    config.keyframes.translation_threshold = 0.20;
    config.keyframes.rotation_threshold = 0.12;
    config.keyframes.time_threshold = 0.50;
    config.keyframes.max_cloud_points = 1024;
    config.global_map.voxel_size = 0.15F;
    config.global_map.max_points = VELAROS_LIO_MAX_GLOBAL_POINTS;
    config.loop.enabled = false;
    return config;
}

Runtime::Runtime(velaros_navigation::NavigationSnapshotPublisher& publisher)
    : frontend_(MakeLioConfig()), backend_(8),
      system_(frontend_, backend_, MakeSystemConfig()), publisher_(&publisher) {}

bool Runtime::AddImu(const ImuSample& sample) {
    const double timestamp = SecondsFromMicros(sample.timestamp_us);
    if (!std::isfinite(timestamp)) {
        return false;
    }
    for (float value : sample.angular_velocity) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    for (float value : sample.linear_acceleration) {
        if (!std::isfinite(value)) {
            return false;
        }
    }

    x86_lio_slam::ImuSample converted;
    converted.timestamp = timestamp;
    converted.angular_velocity = Eigen::Vector3d(
        sample.angular_velocity[0], sample.angular_velocity[1],
        sample.angular_velocity[2]);
    converted.linear_acceleration = Eigen::Vector3d(
        sample.linear_acceleration[0], sample.linear_acceleration[1],
        sample.linear_acceleration[2]);
    system_.AddImu(converted);
    return true;
}

bool Runtime::AddPointCloud(const PointSample* points, std::size_t count) {
    if (count == 0 || count > cloud_buffer_.size() || points == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const PointSample& point = points[index];
        if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
            !std::isfinite(point.z) || !std::isfinite(point.intensity)) {
            return false;
        }
        cloud_buffer_[index] = x86_lio_slam::PointXYZIT{
            point.x, point.y, point.z, point.intensity,
            SecondsFromMicros(point.timestamp_us)};
        if (!std::isfinite(cloud_buffer_[index].timestamp)) {
            return false;
        }
    }
    system_.AddPointCloud(
        std::span<const x86_lio_slam::PointXYZIT>(cloud_buffer_.data(), count));
    return true;
}

bool Runtime::PublishSnapshot(const x86_lio_slam::OdometryState& state) {
    if (publisher_ == nullptr || !state.valid ||
        !state.T_odom_body.rotation.allFinite() ||
        !state.T_odom_body.translation.allFinite() ||
        !state.velocity.allFinite() || !state.angular_velocity.allFinite()) {
        return false;
    }

    const x86_lio_slam::Pose3d map_body = system_.MapBody(state.T_odom_body);
    const Eigen::Vector3d map_velocity =
        system_.T_map_odom().rotation * state.velocity;

    velaros_navigation::PoseSnapshot snapshot;
    snapshot.timestamp = state.timestamp;
    snapshot.frame.Set("map");
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            snapshot.T_map_body.rotation(row, column) =
                map_body.rotation(row, column);
            snapshot.covariance(row, column) =
                state.pose_covariance(static_cast<Eigen::Index>(row),
                                      static_cast<Eigen::Index>(column));
        }
    }
    snapshot.T_map_body.translation = velaros_navigation::Vector3d(
        map_body.translation.x(), map_body.translation.y(),
        map_body.translation.z());
    snapshot.planar_velocity = velaros_navigation::Vector2d(
        map_velocity.x(), map_velocity.y());
    snapshot.yaw_rate = state.angular_velocity.z();
    snapshot.covariance_valid = state.pose_covariance.allFinite();
    snapshot.valid = snapshot.frame.Valid() &&
                     velaros_navigation::IsFinite(snapshot);
    if (!snapshot.valid) {
        return false;
    }

    const auto& points = system_.GlobalMap();
    map_point_count_ = std::min(points.size(), map_buffer_.size());
    for (std::size_t index = 0; index < map_point_count_; ++index) {
        map_buffer_[index] = velaros_navigation::Vector3f(
            points[index].x(), points[index].y(), points[index].z());
    }
    return publisher_->Publish(snapshot, map_buffer_.data(), map_point_count_,
                               system_.GlobalMapVersion());
}

bool Runtime::Process() {
    if (!system_.Process(last_state_)) {
        return false;
    }
    has_output_ = PublishSnapshot(last_state_);
    return has_output_;
}

void Runtime::Reset() {
    system_.Reset();
    if (publisher_ != nullptr) {
        publisher_->Reset();
    }
    cloud_buffer_.fill(x86_lio_slam::PointXYZIT{});
    map_buffer_.fill(velaros_navigation::Vector3f{});
    last_state_ = x86_lio_slam::OdometryState{};
    map_point_count_ = 0;
    has_output_ = false;
}

}  // namespace velaros_lio
