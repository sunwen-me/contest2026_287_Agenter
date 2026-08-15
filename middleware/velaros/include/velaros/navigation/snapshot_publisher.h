#pragma once

#include "velaros/navigation/nav_costmap.h"
#include "velaros/navigation/snapshot.h"


#include <cstddef>
#include <cstdint>

namespace velaros_navigation {

class NavigationSnapshotPublisher final {
public:
    explicit NavigationSnapshotPublisher(
        NavCostmapConfig costmap_config = NavCostmapConfig{});

    bool Publish(const PoseSnapshot& pose, const Vector3f* map_points,
                 std::size_t map_point_count,
                 std::uint64_t map_version);

    void Reset();

    NavigationSnapshotBuffer<>& Buffer() { return buffer_; }

    const NavigationSnapshotBuffer<>& Buffer() const { return buffer_; }

    NavigationSnapshotBuffer<>::ReadGuard AcquireLatest() const {
        return buffer_.AcquireLatest();
    }

    NavCostmap& Costmap() { return costmap_; }

    const NavCostmap& Costmap() const { return costmap_; }

private:
    NavCostmap costmap_;
    NavigationSnapshotBuffer<> buffer_;
};

}  // namespace velaros_navigation
