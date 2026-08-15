#pragma once

#include "velaros/navigation/types.h"


#include <array>
#include <cstddef>
#include <cstdint>

namespace velaros_navigation {

struct NavCostmapConfig {
    std::uint16_t width = 160;
    std::uint16_t height = 160;
    float resolution = 0.25F;
    float obstacle_min_height = -0.20F;
    float obstacle_max_height = 1.80F;
    float inflation_radius = 0.35F;
    Vector2f fixed_origin = Vector2f::Zero();
    FrameId frame{};
    bool rolling_window = true;
    UnknownSpacePolicy unknown_policy = UnknownSpacePolicy::Free;
};

class NavCostmap final {
public:
    explicit NavCostmap(NavCostmapConfig config = NavCostmapConfig{});

    bool Configure(NavCostmapConfig config);

    // The input is the read-only global map owned by LIO. Only the compact 2D
    // cost representation is retained by this object.
    bool Build(const Vector3f* map_points, std::size_t map_point_count,
               const Vector2d& rolling_center, std::uint64_t version,
               double timestamp);

    void Clear();

    NavMapView View() const;

    const NavCostmapConfig& Config() const { return config_; }

    const NavCell* Cells() const { return cells_.data(); }

    std::size_t CellCount() const { return cell_count_; }

    std::uint64_t Version() const { return version_; }

    bool WorldToCell(const Vector2d& point, GridCell& cell) const {
        return View().WorldToCell(point, cell);
    }

    Vector2d CellCenter(GridCell cell) const {
        return View().CellCenter(cell);
    }

    NavCellState State(GridCell cell) const;

    bool IsTraversable(GridCell cell, bool allow_unknown = false,
                       bool allow_inflated = false) const;

    bool IsPathSafe(const PlannedPath& path, bool allow_unknown = false,
                    bool allow_inflated = false) const;

private:
    static constexpr std::size_t CellIndexLimit = kMaxNavMapCells;

    bool IsConfigured() const;

    NavCostmapConfig config_{};
    std::array<NavCell, CellIndexLimit> cells_{};
    std::array<std::uint8_t, CellIndexLimit> occupied_{};
    std::size_t cell_count_ = 0;
    std::uint64_t version_ = 0;
    double timestamp_ = 0.0;
    Vector2f origin_ = Vector2f::Zero();
};

}  // namespace velaros_navigation
