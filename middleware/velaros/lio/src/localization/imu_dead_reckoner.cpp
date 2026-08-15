#include "x86_lio_slam/localization/imu_dead_reckoner.h"

#include "x86_lio_slam/common/geometry.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace x86_lio_slam {

namespace {

constexpr double kTimestampEpsilon = 1e-9;

ImuDeadReckonerConfig NormalizeConfig(ImuDeadReckonerConfig config) {
    config.initialization_samples =
        std::max<std::size_t>(1, config.initialization_samples);
    if (!(config.gravity_norm > 0.0) || !std::isfinite(config.gravity_norm)) {
        config.gravity_norm = 9.7946;
    }
    if (!(config.max_dt > 0.0) || !std::isfinite(config.max_dt)) {
        config.max_dt = 0.2;
    }
    if (!config.information.allFinite()) {
        config.information = DefaultDeadReckoningInformation();
    }
    return config;
}

}  // namespace

ImuDeadReckoner::ImuDeadReckoner(ImuDeadReckonerConfig config)
    : config_(NormalizeConfig(std::move(config))) {
    Reset();
}

void ImuDeadReckoner::Reset() {
    initialized_ = false;
    last_timestamp_ = -1.0;
    last_imu_ = ImuSample{};
    gravity_ = Eigen::Vector3d(0.0, 0.0, -config_.gravity_norm);
    gyro_bias_ = Eigen::Vector3d::Zero();
    velocity_ = Eigen::Vector3d::Zero();
    pose_ = initial_pose_set_ ? initial_pose_ : Pose3d::Identity();
    initialization_count_ = 0;
    initialization_acceleration_sum_.setZero();
    initialization_gyro_sum_.setZero();
    last_observation_ = DeadReckoningObservation{};
    last_observation_.information = config_.information;
}

void ImuDeadReckoner::SetInitialPose(const Pose3d& pose) {
    if (!IsFinitePose(pose)) {
        return;
    }
    initial_pose_ = pose;
    initial_pose_set_ = true;
    Reset();
}

bool ImuDeadReckoner::IsFinitePose(const Pose3d& pose) const {
    return pose.rotation.allFinite() && pose.translation.allFinite();
}

bool ImuDeadReckoner::AddImu(const ImuSample& sample,
                             DeadReckoningObservation& output) {
    output = DeadReckoningObservation{};
    output.timestamp = sample.timestamp;
    output.information = config_.information;
    output.valid = false;
    output.reliable = true;

    if (!IsFinite(sample) ||
        (last_timestamp_ >= 0.0 &&
         sample.timestamp < last_timestamp_ - kTimestampEpsilon)) {
        return false;
    }

    if (!initialized_) {
        initialization_acceleration_sum_ += sample.linear_acceleration;
        initialization_gyro_sum_ += sample.angular_velocity;
        ++initialization_count_;
        last_timestamp_ = sample.timestamp;
        last_imu_ = sample;

        if (initialization_count_ < config_.initialization_samples) {
            return false;
        }

        const Eigen::Vector3d mean_acceleration =
            initialization_acceleration_sum_ /
            static_cast<double>(initialization_count_);
        if (mean_acceleration.norm() <= 1e-6) {
            return false;
        }

        // The M2DGR IMU reports specific force, so gravity is opposite the
        // stationary acceleration. Keep the initial body frame as the DR
        // odometry frame, matching the frontend convention.
        gravity_ = -config_.gravity_norm * mean_acceleration.normalized();
        if (config_.remove_initial_gyro_bias) {
            gyro_bias_ = initialization_gyro_sum_ /
                         static_cast<double>(initialization_count_);
        }
        pose_ = initial_pose_set_ ? initial_pose_ : Pose3d::Identity();
        velocity_.setZero();
        initialized_ = true;
    } else {
        const double dt = sample.timestamp - last_timestamp_;
        if (!(dt > kTimestampEpsilon) || dt > config_.max_dt) {
            // Duplicate samples do not advance the state; a large gap is
            // accepted as a new propagation anchor without inventing motion.
            if (std::abs(dt) <= kTimestampEpsilon) {
                output.timestamp = sample.timestamp;
                output.pose = pose_;
                output.velocity = velocity_;
                output.valid = true;
                last_observation_ = output;
                last_imu_ = sample;
                last_timestamp_ = sample.timestamp;
                return true;
            }
            last_imu_ = sample;
            last_timestamp_ = sample.timestamp;
            output.pose = pose_;
            output.velocity = velocity_;
            output.valid = true;
            last_observation_ = output;
            return true;
        }

        const Eigen::Vector3d omega_previous =
            last_imu_.angular_velocity - gyro_bias_;
        const Eigen::Vector3d omega_current =
            sample.angular_velocity - gyro_bias_;
        const Eigen::Vector3d omega_mid = 0.5 *
            (omega_previous + omega_current);
        const Eigen::Matrix3d next_rotation =
            pose_.rotation * ExpSO3(omega_mid * dt);
        const Eigen::Vector3d acceleration_previous =
            pose_.rotation * last_imu_.linear_acceleration;
        const Eigen::Vector3d acceleration_current =
            next_rotation * sample.linear_acceleration;
        const Eigen::Vector3d acceleration =
            0.5 * (acceleration_previous + acceleration_current) + gravity_;

        pose_.translation += velocity_ * dt + 0.5 * acceleration * dt * dt;
        velocity_ += acceleration * dt;
        pose_.rotation = next_rotation;
    }

    last_timestamp_ = sample.timestamp;
    last_imu_ = sample;
    output.timestamp = sample.timestamp;
    output.pose = pose_;
    output.velocity = velocity_;
    output.valid = initialized_ && IsFinitePose(pose_) &&
                   velocity_.allFinite();
    last_observation_ = output;
    return output.valid;
}

}  // namespace x86_lio_slam
