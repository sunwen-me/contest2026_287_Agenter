#include "x86_lio_slam/keyframe/keyframe_manager.h"

#include "x86_lio_slam/common/geometry.h"

#include <algorithm>
#include <cmath>

namespace x86_lio_slam {

void KeyframeManager::Reset() {
    keyframes_.clear();
}

const Keyframe* KeyframeManager::Last() const {
    return keyframes_.empty() ? nullptr : &keyframes_.back();
}

bool KeyframeManager::ShouldCreate(const OdometryState& current) const {
    if (!current.valid) {
        return false;
    }
    const Keyframe* previous = Last();
    if (!previous) {
        return true;
    }

    const Pose3d delta = RelativePose(previous->T_odom_body,
                                       current.T_odom_body);
    const double translation = delta.translation.norm();
    const double rotation = RotationDistance(delta.rotation);
    const double elapsed = current.timestamp - previous->timestamp;
    return translation >= config_.translation_threshold ||
           rotation >= config_.rotation_threshold ||
           elapsed >= config_.time_threshold;
}

Keyframe KeyframeManager::Create(
    const OdometryState& state, std::span<const Eigen::Vector3f> cloud_body) {
    Keyframe keyframe;
    keyframe.id = static_cast<std::uint32_t>(keyframes_.size());
    keyframe.timestamp = state.timestamp;
    keyframe.T_odom_body = state.T_odom_body;
    keyframe.T_map_body = state.T_odom_body;
    keyframe.covariance = state.pose_covariance;

    const std::size_t max_points = config_.max_cloud_points;
    if (max_points == 0 || cloud_body.size() <= max_points) {
        keyframe.cloud_body.assign(cloud_body.begin(), cloud_body.end());
    } else {
        const std::size_t stride =
            std::max<std::size_t>(1, cloud_body.size() / max_points);
        keyframe.cloud_body.reserve(max_points);
        for (std::size_t index = 0;
             index < cloud_body.size() && keyframe.cloud_body.size() < max_points;
             index += stride) {
            keyframe.cloud_body.push_back(cloud_body[index]);
        }
    }

    keyframes_.push_back(keyframe);
    return keyframes_.back();
}

}  // namespace x86_lio_slam
