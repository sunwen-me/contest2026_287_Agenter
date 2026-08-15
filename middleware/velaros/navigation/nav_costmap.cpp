#include "velaros/navigation/nav_costmap.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace velaros_navigation {

namespace {

bool IsFinite(double value) { return std::isfinite(value); }

}  // namespace

NavCostmap::NavCostmap(NavCostmapConfig config) { Configure(config); }

bool NavCostmap::Configure(NavCostmapConfig config) {
    config_ = config;
    cell_count_ = 0;
    version_ = 0;
    timestamp_ = 0.0;
    origin_.setZero();
    cells_.fill(NavCell{});
    occupied_.fill(0U);
    return IsConfigured();
}

bool NavCostmap::IsConfigured() const {
    if (config_.width == 0 || config_.height == 0 || config_.resolution <= 0.0F ||
        !std::isfinite(config_.resolution) ||
        !config_.frame.Valid() ||
        !std::isfinite(config_.obstacle_min_height) ||
        !std::isfinite(config_.obstacle_max_height) ||
        config_.obstacle_min_height > config_.obstacle_max_height ||
        !std::isfinite(config_.inflation_radius) ||
        config_.inflation_radius < 0.0F) {
        return false;
    }
    if (!config_.fixed_origin.allFinite()) {
        return false;
    }
    const std::size_t cell_count =
        static_cast<std::size_t>(config_.width) * config_.height;
    return cell_count > 0 && cell_count <= CellIndexLimit;
}

bool NavCostmap::Build(const Vector3f* map_points,
                       std::size_t map_point_count,
                       const Vector2d& rolling_center,
                       std::uint64_t version, double timestamp) {
    if (!IsConfigured() || version == 0 || !IsFinite(timestamp) ||
        !rolling_center.allFinite() ||
        (map_point_count != 0 && map_points == nullptr)) {
        return false;
    }

    cell_count_ = static_cast<std::size_t>(config_.width) * config_.height;
    version_ = version;
    timestamp_ = timestamp;
    if (config_.rolling_window) {
        const double half_width =
            0.5 * static_cast<double>(config_.width) * config_.resolution;
        const double half_height =
            0.5 * static_cast<double>(config_.height) * config_.resolution;
        origin_.x() = static_cast<float>(
            std::floor(rolling_center.x() / config_.resolution) *
                config_.resolution -
            half_width);
        origin_.y() = static_cast<float>(
            std::floor(rolling_center.y() / config_.resolution) *
                config_.resolution -
            half_height);
    } else {
        origin_ = config_.fixed_origin;
    }

    const std::uint8_t initial_cost =
        config_.unknown_policy == UnknownSpacePolicy::Free ? kNavCostFree
                                                            : kNavCostUnknown;
    for (std::size_t index = 0; index < cell_count_; ++index) {
        cells_[index].cost = initial_cost;
        occupied_[index] = 0U;
    }

    const float inverse_resolution = 1.0F / config_.resolution;
    for (std::size_t point_index = 0; point_index < map_point_count;
         ++point_index) {
        const Vector3f& point = map_points[point_index];
        if (!point.allFinite() || point.z() < config_.obstacle_min_height ||
            point.z() > config_.obstacle_max_height) {
            continue;
        }
        const int x = static_cast<int>(std::floor(
            (static_cast<double>(point.x()) - origin_.x()) *
            inverse_resolution));
        const int y = static_cast<int>(std::floor(
            (static_cast<double>(point.y()) - origin_.y()) *
            inverse_resolution));
        if (x < 0 || y < 0 || x >= static_cast<int>(config_.width) ||
            y >= static_cast<int>(config_.height)) {
            continue;
        }
        const std::size_t index =
            static_cast<std::size_t>(y) * config_.width +
            static_cast<std::size_t>(x);
        occupied_[index] = 1U;
        cells_[index].cost = kNavCostOccupied;
    }

    const float radius_in_cells =
        config_.inflation_radius / config_.resolution;
    const int radius_cells = static_cast<int>(std::min<float>(
        std::ceil(radius_in_cells),
        static_cast<float>(std::max(config_.width, config_.height))));
    const float radius_squared = config_.inflation_radius *
                                 config_.inflation_radius +
                                 0.25F * config_.resolution * config_.resolution;
    for (int y = 0; y < static_cast<int>(config_.height); ++y) {
        for (int x = 0; x < static_cast<int>(config_.width); ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * config_.width +
                                      static_cast<std::size_t>(x);
            if (occupied_[index] == 0U) {
                continue;
            }
            for (int dy = -radius_cells; dy <= radius_cells; ++dy) {
                for (int dx = -radius_cells; dx <= radius_cells; ++dx) {
                    const float distance_squared =
                        static_cast<float>(dx * dx + dy * dy) *
                        config_.resolution * config_.resolution;
                    if (distance_squared > radius_squared) {
                        continue;
                    }
                    const int neighbor_x = x + dx;
                    const int neighbor_y = y + dy;
                    if (neighbor_x < 0 || neighbor_y < 0 ||
                        neighbor_x >= static_cast<int>(config_.width) ||
                        neighbor_y >= static_cast<int>(config_.height)) {
                        continue;
                    }
                    const std::size_t neighbor_index =
                        static_cast<std::size_t>(neighbor_y) * config_.width +
                        static_cast<std::size_t>(neighbor_x);
                    if (occupied_[neighbor_index] == 0U) {
                        cells_[neighbor_index].cost = kNavCostInflated;
                    }
                }
            }
        }
    }
    return true;
}

void NavCostmap::Clear() {
    cell_count_ = 0;
    version_ = 0;
    timestamp_ = 0.0;
    cells_.fill(NavCell{});
    occupied_.fill(0U);
}

NavMapView NavCostmap::View() const {
    NavMapView view;
    view.version = version_;
    view.timestamp = timestamp_;
    view.frame = config_.frame;
    view.resolution = config_.resolution;
    view.origin = origin_;
    view.width = config_.width;
    view.height = config_.height;
    view.cells = cells_.data();
    view.cell_count = cell_count_;
    view.unknown_policy = config_.unknown_policy;
    return view;
}

NavCellState NavCostmap::State(GridCell cell) const {
    const NavMapView view = View();
    const NavCell* value = view.Cell(static_cast<int>(cell.x),
                                     static_cast<int>(cell.y));
    return value == nullptr ? NavCellState::Unknown : GetNavCellState(*value);
}

bool NavCostmap::IsTraversable(GridCell cell, bool allow_unknown,
                               bool allow_inflated) const {
    return View().IsTraversable(cell, allow_unknown, allow_inflated);
}

bool NavCostmap::IsPathSafe(const PlannedPath& path, bool allow_unknown,
                            bool allow_inflated) const {
    if (!path.valid() || path.map_version != version_) {
        return false;
    }
    for (std::size_t index = 0; index < path.size; ++index) {
        if (!IsTraversable(path.points[index].cell, allow_unknown,
                           allow_inflated)) {
            return false;
        }
    }
    return true;
}

}  // namespace velaros_navigation
