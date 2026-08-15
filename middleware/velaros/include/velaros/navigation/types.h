#pragma once

#include "velaros/navigation/math.h"


#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace velaros_navigation {

inline constexpr std::size_t kNavigationFrameCapacity = 24;

#ifndef VELAROS_NAV_MAX_MAP_CELLS
#define VELAROS_NAV_MAX_MAP_CELLS 65536
#endif

#ifndef VELAROS_NAV_MAX_PATH_NODES
#define VELAROS_NAV_MAX_PATH_NODES 8192
#endif

inline constexpr std::size_t kMaxNavMapCells = VELAROS_NAV_MAX_MAP_CELLS;
inline constexpr std::size_t kMaxNavPathNodes = VELAROS_NAV_MAX_PATH_NODES;

static_assert(kMaxNavMapCells > 0);
static_assert(kMaxNavPathNodes > 0);

// Keep the navigation boundary independent from the LIO implementation. The
// target receives this fixed-layout pose through a snapshot producer.
struct Pose3d {
    Matrix3d rotation = Matrix3d::Identity();
    Vector3d translation = Vector3d::Zero();
};

struct FrameId {
    std::array<char, kNavigationFrameCapacity> value{};

    FrameId() { Set("map"); }

    bool Set(std::string_view text) {
        if (text.empty() || text.size() >= value.size()) {
            return false;
        }
        std::array<char, kNavigationFrameCapacity> copy{};
        for (std::size_t index = 0; index < text.size(); ++index) {
            copy[index] = text[index];
        }
        value = copy;
        return true;
    }

    bool Set(const char* text) {
        if (text == nullptr) {
            return false;
        }
        std::size_t length = 0;
        while (length < value.size() && text[length] != '\0') {
            ++length;
        }
        if (length == 0 || length >= value.size()) {
            return false;
        }
        return Set(std::string_view(text, length));
    }

    bool SetFixed(const char* text, std::size_t capacity) {
        if (text == nullptr) {
            return false;
        }
        std::size_t length = 0;
        while (length < capacity && length < value.size() &&
               text[length] != '\0') {
            ++length;
        }
        if (length == 0 || length >= value.size()) {
            return false;
        }
        return Set(std::string_view(text, length));
    }

    bool Valid() const {
        std::size_t length = 0;
        while (length < value.size() && value[length] != '\0') {
            ++length;
        }
        return length > 0 && length < value.size();
    }

    bool operator==(const FrameId& other) const { return value == other.value; }

    bool operator!=(const FrameId& other) const { return !(*this == other); }
};

enum class UnknownSpacePolicy : std::uint8_t {
    Blocked,
    Free,
};

enum class NavCellState : std::uint8_t {
    Free,
    Inflated,
    Occupied,
    Unknown,
};

inline constexpr std::uint8_t kNavCostFree = 0U;
inline constexpr std::uint8_t kNavCostInflated = 200U;
inline constexpr std::uint8_t kNavCostOccupied = 254U;
inline constexpr std::uint8_t kNavCostUnknown = 255U;

struct NavCell {
    std::uint8_t cost = kNavCostFree;
};

inline NavCellState GetNavCellState(const NavCell& cell) {
    if (cell.cost == kNavCostUnknown) {
        return NavCellState::Unknown;
    }
    if (cell.cost >= kNavCostOccupied) {
        return NavCellState::Occupied;
    }
    if (cell.cost >= kNavCostInflated) {
        return NavCellState::Inflated;
    }
    return NavCellState::Free;
}

struct GridCell {
    std::uint16_t x = 0;
    std::uint16_t y = 0;

    bool operator==(const GridCell& other) const {
        return x == other.x && y == other.y;
    }
};

struct NavMapView {
    std::uint64_t version = 0;
    double timestamp = 0.0;
    FrameId frame{};
    float resolution = 0.0F;
    Vector2f origin = Vector2f::Zero();
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    const NavCell* cells = nullptr;
    std::size_t cell_count = 0;
    UnknownSpacePolicy unknown_policy = UnknownSpacePolicy::Blocked;

    bool IsValid() const {
        return version != 0 && resolution > 0.0F && width != 0 && height != 0 &&
               cells != nullptr &&
               static_cast<std::size_t>(width) * height == cell_count;
    }

    bool InBounds(int x, int y) const {
        return x >= 0 && y >= 0 && x < static_cast<int>(width) &&
               y < static_cast<int>(height);
    }

    std::size_t Index(int x, int y) const {
        return static_cast<std::size_t>(y) * width +
               static_cast<std::size_t>(x);
    }

    const NavCell* Cell(int x, int y) const {
        return InBounds(x, y) ? &cells[Index(x, y)] : nullptr;
    }

    bool WorldToCell(const Vector2d& point, GridCell& cell) const {
        if (!IsValid() || !point.allFinite()) {
            return false;
        }
        const int x = static_cast<int>(std::floor(
            (point.x() - static_cast<double>(origin.x())) / resolution));
        const int y = static_cast<int>(std::floor(
            (point.y() - static_cast<double>(origin.y())) / resolution));
        if (!InBounds(x, y)) {
            return false;
        }
        cell.x = static_cast<std::uint16_t>(x);
        cell.y = static_cast<std::uint16_t>(y);
        return true;
    }

    Vector2d CellCenter(GridCell cell) const {
        return Vector2d(
            static_cast<double>(origin.x()) +
                (static_cast<double>(cell.x) + 0.5) * resolution,
            static_cast<double>(origin.y()) +
                (static_cast<double>(cell.y) + 0.5) * resolution);
    }

    // Return a bounded nearest occupied-cell distance. The compact costmap
    // stores occupancy rather than a full distance field, so this local query
    // supplies the clearance signal needed by RPP without adding an ESDF.
    double DistanceToOccupied(const Vector2d& point,
                              double max_distance) const {
        if (!IsValid() || !point.allFinite() ||
            !std::isfinite(max_distance) || max_distance < 0.0) {
            return 0.0;
        }
        GridCell center;
        if (!WorldToCell(point, center)) {
            return 0.0;
        }
        const double map_diagonal =
            std::hypot(static_cast<double>(width) * resolution,
                       static_cast<double>(height) * resolution);
        const double search_limit = std::min(max_distance, map_diagonal);
        const int radius =
            static_cast<int>(std::ceil(search_limit / resolution)) + 1;
        double nearest = max_distance;
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int x = static_cast<int>(center.x) + dx;
                const int y = static_cast<int>(center.y) + dy;
                const NavCell* cell = Cell(x, y);
                if (cell == nullptr ||
                    GetNavCellState(*cell) != NavCellState::Occupied) {
                    continue;
                }
                nearest = std::min(nearest, (CellCenter(GridCell{
                    static_cast<std::uint16_t>(x),
                    static_cast<std::uint16_t>(y)}) - point).norm());
            }
        }
        return nearest;
    }

    bool IsTraversable(GridCell cell, bool allow_unknown,
                       bool allow_inflated) const {
        const NavCell* value = Cell(static_cast<int>(cell.x),
                                    static_cast<int>(cell.y));
        if (value == nullptr) {
            return false;
        }
        const NavCellState state = GetNavCellState(*value);
        if (state == NavCellState::Occupied) {
            return false;
        }
        if (state == NavCellState::Unknown) {
            return allow_unknown || unknown_policy == UnknownSpacePolicy::Free;
        }
        return state != NavCellState::Inflated || allow_inflated;
    }
};

using NavMapSnapshot = NavMapView;

struct PoseSnapshot {

    double timestamp = 0.0;
    FrameId frame{};
    Pose3d T_map_body{};
    // Published in the snapshot map frame. Controllers must rotate it into
    // body coordinates before using its forward component.
    Vector2d planar_velocity = Vector2d::Zero();
    double yaw_rate = 0.0;
    Matrix3d covariance = Matrix3d::Identity();
    bool covariance_valid = false;
    bool valid = false;
};

struct NavigationSnapshotView {
    PoseSnapshot pose{};
    NavMapView map{};

    bool IsValid() const { return pose.valid && map.IsValid(); }
};

struct NavigationGoal {

    double timestamp = 0.0;
    FrameId frame{};
    Vector2d position = Vector2d::Zero();
    double heading = 0.0;
    bool has_heading = false;
    bool valid = false;
};

enum class PlanStatus : std::uint8_t {
    None,
    Success,
    InvalidMap,
    StartOutOfBounds,
    GoalOutOfBounds,
    StartOccupied,
    GoalOccupied,
    Timeout,
    NoPath,
    PathCapacityExceeded,
};

struct PathPoint {
    GridCell cell{};
    Vector2f position = Vector2f::Zero();
};

struct PlannedPath {
    std::uint64_t map_version = 0;
    std::uint64_t goal_generation = 0;
    PlanStatus status = PlanStatus::None;
    std::size_t size = 0;
    std::array<PathPoint, kMaxNavPathNodes> points{};

    bool valid() const {
        return status == PlanStatus::Success && size > 0 &&
               size <= points.size();
    }
};

enum class ControlStatus : std::uint8_t {
    Commanded,
    GoalReached,
    NoPath,
    InvalidPose,
    InvalidMap,
    StalePose,
    StaleMap,
    StaleGoal,
    FrameMismatch,
    UnsafePath,
    EmergencyStop,
};

struct VelocityCommand {

    double timestamp = 0.0;
    FrameId frame{};
    Vector2d planar_velocity = Vector2d::Zero();
    double yaw_rate = 0.0;
    ControlStatus status = ControlStatus::InvalidPose;
    bool valid = false;
    bool stop = true;
};

inline bool IsFinite(const PoseSnapshot& snapshot) {
    return std::isfinite(snapshot.timestamp) &&
           snapshot.T_map_body.rotation.allFinite() &&
           snapshot.T_map_body.translation.allFinite() &&
           snapshot.planar_velocity.allFinite() &&
           std::isfinite(snapshot.yaw_rate);
}

inline bool IsFinite(const NavigationGoal& goal) {
    return std::isfinite(goal.timestamp) && goal.position.allFinite() &&
           std::isfinite(goal.heading);
}

}  // namespace velaros_navigation
