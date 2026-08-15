#include "velaros/navigation/snapshot_publisher.h"

namespace velaros_navigation {

NavigationSnapshotPublisher::NavigationSnapshotPublisher(
    NavCostmapConfig costmap_config)
    : costmap_(costmap_config) {}

bool NavigationSnapshotPublisher::Publish(
    const PoseSnapshot& pose, const Vector3f* map_points,
    std::size_t map_point_count,
    std::uint64_t map_version) {
    if (!pose.valid || !IsFinite(pose) || map_version == 0) {
        return false;
    }
    const Vector2d pose_position(pose.T_map_body.translation.x(),
                                 pose.T_map_body.translation.y());
    if (!costmap_.Build(map_points, map_point_count, pose_position,
                        map_version, pose.timestamp)) {
        return false;
    }

    NavMapView map = costmap_.View();
    if (pose.frame != map.frame) {
        return false;
    }
    return buffer_.Publish(pose, map);
}

void NavigationSnapshotPublisher::Reset() {
    costmap_.Clear();
    (void)buffer_.Reset();
}

}  // namespace velaros_navigation
