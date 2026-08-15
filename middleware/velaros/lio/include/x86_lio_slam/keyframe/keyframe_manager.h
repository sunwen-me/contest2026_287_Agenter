#pragma once

#include "x86_lio_slam/common/config.h"
#include "x86_lio_slam/keyframe/keyframe.h"

#include <cstdint>
#include <span>
#include <vector>

namespace x86_lio_slam {

class KeyframeManager {
public:
    explicit KeyframeManager(KeyframeConfig config = KeyframeConfig{})
        : config_(config) {}

    void Reset();

    bool ShouldCreate(const OdometryState& current) const;

    Keyframe Create(const OdometryState& state,
                    std::span<const Eigen::Vector3f> cloud_body);

    const std::vector<Keyframe>& Keyframes() const { return keyframes_; }

    std::vector<Keyframe>& MutableKeyframes() { return keyframes_; }

    bool UpdateMapPose(std::uint32_t id, const Pose3d& pose);

    const Keyframe* Last() const;

private:
    KeyframeConfig config_;
    std::vector<Keyframe> keyframes_;
};

}  // namespace x86_lio_slam
