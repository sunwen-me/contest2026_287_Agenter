#pragma once

#include "x86_lio_slam/common/types.h"

namespace x86_lio_slam {

class MapOdomCorrection {
public:
    void Reset();

    void Update(const Pose3d& T_map_body_anchor,
                const Pose3d& T_odom_body_anchor,
                double smoothing_alpha = 1.0);

    Pose3d T_map_odom() const { return correction_; }

    Pose3d MapBody(const Pose3d& T_odom_body) const;

    bool Initialized() const { return initialized_; }

private:
    Pose3d correction_{};
    bool initialized_ = false;
};

}  // namespace x86_lio_slam
