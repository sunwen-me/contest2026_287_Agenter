#pragma once

#include "x86_lio_slam/common/types.h"

#include <Eigen/Core>

#include <cstdint>
#include <span>
#include <vector>

namespace x86_lio_slam {

struct Keyframe {
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    std::uint32_t id = 0;
    double timestamp = 0.0;
    Pose3d T_odom_body{};
    Pose3d T_map_body{};
    Eigen::Matrix<double, 6, 6> covariance =
        Eigen::Matrix<double, 6, 6>::Identity();
    std::vector<Eigen::Vector3f> cloud_body;
};

}  // namespace x86_lio_slam
