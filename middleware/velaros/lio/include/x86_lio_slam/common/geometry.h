#pragma once

#include "x86_lio_slam/common/types.h"

#include <Eigen/Core>

namespace x86_lio_slam {

Eigen::Matrix3d Hat(const Eigen::Vector3d& value);

Eigen::Matrix3d ExpSO3(const Eigen::Vector3d& tangent);

Eigen::Vector3d LogSO3(const Eigen::Matrix3d& rotation);

Pose3d Compose(const Pose3d& lhs, const Pose3d& rhs);

Pose3d Inverse(const Pose3d& pose);

Pose3d RelativePose(const Pose3d& from, const Pose3d& to);

Pose3d ApplyRightIncrement(const Pose3d& pose,
                           const Eigen::Matrix<double, 6, 1>& increment);

Eigen::Matrix<double, 6, 1> PoseResidual(const Pose3d& measurement,
                                          const Pose3d& from,
                                          const Pose3d& to);

double RotationDistance(const Eigen::Matrix3d& rotation);

}  // namespace x86_lio_slam
