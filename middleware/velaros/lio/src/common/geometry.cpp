#include "x86_lio_slam/common/geometry.h"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>

namespace x86_lio_slam {

Eigen::Matrix3d Hat(const Eigen::Vector3d& value) {
    Eigen::Matrix3d result;
    result << 0.0, -value.z(), value.y(), value.z(), 0.0, -value.x(),
        -value.y(), value.x(), 0.0;
    return result;
}

Eigen::Matrix3d ExpSO3(const Eigen::Vector3d& tangent) {
    const double theta2 = tangent.squaredNorm();
    const Eigen::Matrix3d skew = Hat(tangent);
    if (theta2 < 1e-16) {
        return Eigen::Matrix3d::Identity() + skew + 0.5 * skew * skew;
    }

    const double theta = std::sqrt(theta2);
    return Eigen::Matrix3d::Identity() +
           (std::sin(theta) / theta) * skew +
           ((1.0 - std::cos(theta)) / theta2) * skew * skew;
}

Eigen::Vector3d LogSO3(const Eigen::Matrix3d& rotation) {
    Eigen::Quaterniond quaternion(rotation);
    quaternion.normalize();
    const Eigen::AngleAxisd angle_axis(quaternion);
    if (angle_axis.angle() < 1e-12) {
        return Eigen::Vector3d::Zero();
    }
    return angle_axis.axis() * angle_axis.angle();
}

Pose3d Compose(const Pose3d& lhs, const Pose3d& rhs) {
    Pose3d result;
    result.rotation = lhs.rotation * rhs.rotation;
    result.translation = lhs.rotation * rhs.translation + lhs.translation;
    return result;
}

Pose3d Inverse(const Pose3d& pose) {
    Pose3d result;
    result.rotation = pose.rotation.transpose();
    result.translation = -result.rotation * pose.translation;
    return result;
}

Pose3d RelativePose(const Pose3d& from, const Pose3d& to) {
    return Compose(Inverse(from), to);
}

Pose3d ApplyRightIncrement(const Pose3d& pose,
                           const Eigen::Matrix<double, 6, 1>& increment) {
    Pose3d result = pose;
    result.translation += increment.head<3>();
    result.rotation = result.rotation * ExpSO3(increment.tail<3>());
    return result;
}

Eigen::Matrix<double, 6, 1> PoseResidual(const Pose3d& measurement,
                                          const Pose3d& from,
                                          const Pose3d& to) {
    const Pose3d delta = Compose(Inverse(measurement), RelativePose(from, to));
    Eigen::Matrix<double, 6, 1> result;
    result.head<3>() = delta.translation;
    result.tail<3>() = LogSO3(delta.rotation);
    return result;
}

double RotationDistance(const Eigen::Matrix3d& rotation) {
    return LogSO3(rotation).norm();
}

}  // namespace x86_lio_slam
