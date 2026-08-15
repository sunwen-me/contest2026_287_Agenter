#pragma once

#include "x86_lio_slam/backend/pgo_fusion.h"
#include "x86_lio_slam/common/types.h"

#include <Eigen/Core>

#include <cstddef>

namespace x86_lio_slam {

struct ImuDeadReckonerConfig {
    // The initialization window is assumed to be stationary so its mean
    // acceleration supplies the initial gravity direction.
    std::size_t initialization_samples = 200;
    double gravity_norm = 9.7946;
    double max_dt = 0.2;
    Information6d information = DefaultDeadReckoningInformation();
    bool remove_initial_gyro_bias = true;
};

// IMU-only pose propagation used as the DR source for multi-source PGO.
// It deliberately has no point-cloud or LIO-state input, so it cannot become
// an accidental alias of the lidar odometry stream.
class ImuDeadReckoner final {
public:
    explicit ImuDeadReckoner(
        ImuDeadReckonerConfig config = ImuDeadReckonerConfig{});

    void Reset();

    void SetInitialPose(const Pose3d& pose);

    // Returns a valid observation once the stationary initialization window
    // has completed. Invalid or non-monotonic samples are rejected.
    bool AddImu(const ImuSample& sample, DeadReckoningObservation& output);

    bool IsInitialized() const { return initialized_; }

    const DeadReckoningObservation& LastObservation() const {
        return last_observation_;
    }

private:
    bool IsFinitePose(const Pose3d& pose) const;

    ImuDeadReckonerConfig config_;
    Pose3d initial_pose_{};
    bool initial_pose_set_ = false;

    bool initialized_ = false;
    double last_timestamp_ = -1.0;
    ImuSample last_imu_{};
    Eigen::Vector3d gravity_ = Eigen::Vector3d(0.0, 0.0, -9.7946);
    Eigen::Vector3d gyro_bias_ = Eigen::Vector3d::Zero();
    Eigen::Vector3d velocity_ = Eigen::Vector3d::Zero();
    Pose3d pose_{};

    std::size_t initialization_count_ = 0;
    Eigen::Vector3d initialization_acceleration_sum_ =
        Eigen::Vector3d::Zero();
    Eigen::Vector3d initialization_gyro_sum_ = Eigen::Vector3d::Zero();

    DeadReckoningObservation last_observation_{};
};

}  // namespace x86_lio_slam
