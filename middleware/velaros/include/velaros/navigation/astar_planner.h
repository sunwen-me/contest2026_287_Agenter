#pragma once

#include "velaros/navigation/types.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace velaros_navigation {

struct AStarPlannerConfig {
    std::uint32_t max_expansions = static_cast<std::uint32_t>(kMaxNavMapCells);
    std::uint32_t max_planning_microseconds = 0;
    bool allow_unknown = false;
    bool allow_inflated = false;
    float heuristic_weight = 1.0F;
    float clearance_penalty = 2.0F;
};

class AStarPlanner final {
public:
    explicit AStarPlanner(AStarPlannerConfig config = AStarPlannerConfig{})
        : config_(config) {}

    void Reset();

    PlanStatus Plan(const NavMapView& map, GridCell start, GridCell goal,
                    PlannedPath& output);

    PlanStatus Plan(const NavMapView& map, const Vector2d& start,
                    const Vector2d& goal, PlannedPath& output);

    const AStarPlannerConfig& Config() const { return config_; }

    void SetConfig(AStarPlannerConfig config) { config_ = config; }

    std::uint32_t LastExpansionCount() const { return last_expansions_; }

private:
    static constexpr std::uint8_t kUnvisited = 0U;
    static constexpr std::uint8_t kOpen = 1U;
    static constexpr std::uint8_t kClosed = 2U;

    struct NodeRecord {
        float g = 0.0F;
        float f = 0.0F;
        std::int32_t parent = -1;
        std::uint8_t state = kUnvisited;
    };

    static std::size_t Index(const NavMapView& map, GridCell cell) {
        return static_cast<std::size_t>(cell.y) * map.width + cell.x;
    }

    static GridCell CellFromIndex(const NavMapView& map, std::size_t index) {
        GridCell cell;
        cell.x = static_cast<std::uint16_t>(index % map.width);
        cell.y = static_cast<std::uint16_t>(index / map.width);
        return cell;
    }

    bool IsBetter(std::size_t lhs, std::size_t rhs) const;

    void HeapPush(std::size_t index);

    std::size_t HeapPop();

    void HeapRemove(std::size_t index);

    float Heuristic(GridCell from, GridCell to) const;

    float StepCost(const NavCell& cell, float distance) const;

    PlanStatus BuildPath(const NavMapView& map, std::size_t start_index,
                         std::size_t goal_index, PlannedPath& output);

    AStarPlannerConfig config_{};
    std::array<NodeRecord, kMaxNavMapCells> records_{};
    std::array<std::uint32_t, kMaxNavMapCells> heap_{};
    std::array<std::int32_t, kMaxNavMapCells> heap_positions_{};
    std::size_t heap_size_ = 0;
    std::uint32_t last_expansions_ = 0;
};

}  // namespace velaros_navigation
