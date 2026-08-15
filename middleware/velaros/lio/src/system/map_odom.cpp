#include "x86_lio_slam/system/map_odom.h"

#include "x86_lio_slam/common/geometry.h"

#include <algorithm>

namespace x86_lio_slam {

void MapOdomCorrection::Reset() {
    correction_ = Pose3d::Identity();
    initialized_ = false;
}

void MapOdomCorrection::Update(const Pose3d& T_map_body_anchor,
                               const Pose3d& T_odom_body_anchor,
                               double smoothing_alpha) {
    const Pose3d target =
        Compose(T_map_body_anchor, Inverse(T_odom_body_anchor));
    const double alpha = std::clamp(smoothing_alpha, 0.0, 1.0);
    if (!initialized_ || alpha >= 1.0) {
        correction_ = target;
        initialized_ = true;
        return;
    }
    const Eigen::Quaterniond current_rotation(correction_.rotation);
    const Eigen::Quaterniond target_rotation(target.rotation);
    const Eigen::Quaterniond blended =
        current_rotation.slerp(alpha, target_rotation).normalized();
    correction_.rotation = blended.toRotationMatrix();
    correction_.translation =
        (1.0 - alpha) * correction_.translation + alpha * target.translation;
    initialized_ = true;
}

Pose3d MapOdomCorrection::MapBody(const Pose3d& T_odom_body) const {
    return Compose(correction_, T_odom_body);
}

}  // namespace x86_lio_slam
