#include "x86_lio_slam/keyframe/keyframe_manager.h"

namespace x86_lio_slam {

bool KeyframeManager::UpdateMapPose(std::uint32_t id, const Pose3d& pose) {
    for (Keyframe& keyframe : keyframes_) {
        if (keyframe.id == id) {
            keyframe.T_map_body = pose;
            return true;
        }
    }
    return false;
}

}  // namespace x86_lio_slam
