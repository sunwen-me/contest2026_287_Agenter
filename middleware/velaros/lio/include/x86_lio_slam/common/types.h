#pragma once

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cstdint>
#include <cmath>
#include <limits>
#include <vector>

namespace x86_lio_slam {

// T_a_b maps a point expressed in frame b into frame a.
struct PointXYZIT {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float intensity = 0.0F;
    double timestamp = 0.0;

    Eigen::Vector3f Vector() const { return Eigen::Vector3f{x, y, z}; }
};

struct ImuSample {
    double timestamp = 0.0;
    Eigen::Vector3d angular_velocity = Eigen::Vector3d::Zero();
    Eigen::Vector3d linear_acceleration = Eigen::Vector3d::Zero();
};

struct Pose3d {
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    Eigen::Matrix3d rotation = Eigen::Matrix3d::Identity();
    Eigen::Vector3d translation = Eigen::Vector3d::Zero();

    static Pose3d Identity() { return Pose3d{}; }

    Eigen::Vector3d Transform(const Eigen::Vector3d& point) const {
        return rotation * point + translation;
    }

    Eigen::Vector3f Transform(const Eigen::Vector3f& point) const {
        return (rotation.cast<float>() * point) + translation.cast<float>();
    }
};

struct OdometryState {
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    double timestamp = 0.0;
    Pose3d T_odom_body{};
    Eigen::Vector3d velocity = Eigen::Vector3d::Zero();
    Eigen::Vector3d angular_velocity = Eigen::Vector3d::Zero();
    Eigen::Matrix<double, 6, 6> pose_covariance =
        Eigen::Matrix<double, 6, 6>::Identity();
    bool valid = false;
};

inline bool IsFinite(const PointXYZIT& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) &&
           std::isfinite(point.z) && std::isfinite(point.intensity) &&
           std::isfinite(point.timestamp);
}

inline bool IsFinite(const ImuSample& sample) {
    return std::isfinite(sample.timestamp) &&
           sample.angular_velocity.allFinite() &&
           sample.linear_acceleration.allFinite();
}

}  // namespace x86_lio_slam
