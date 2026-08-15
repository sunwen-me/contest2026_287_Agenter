#include "velaros/navigation/astar_planner.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace velaros_navigation {

namespace {

constexpr std::array<int, 8> kNeighborX{{1, 1, 0, -1, -1, -1, 0, 1}};
constexpr std::array<int, 8> kNeighborY{{0, 1, 1, 1, 0, -1, -1, -1}};
constexpr std::array<float, 8> kNeighborDistance{
    {1.0F, 1.41421356237F, 1.0F, 1.41421356237F, 1.0F,
     1.41421356237F, 1.0F, 1.41421356237F}};

float SafeHeuristicWeight(float value) {
    return std::isfinite(value) && value >= 1.0F ? value : 1.0F;
}

}  // namespace

void AStarPlanner::Reset() {
    heap_size_ = 0;
    last_expansions_ = 0;
}

bool AStarPlanner::IsBetter(std::size_t lhs, std::size_t rhs) const {
    constexpr float kTieEpsilon = 1e-6F;
    const NodeRecord& left = records_[lhs];
    const NodeRecord& right = records_[rhs];
    if (left.f + kTieEpsilon < right.f) {
        return true;
    }
    if (right.f + kTieEpsilon < left.f) {
        return false;
    }
    if (left.g + kTieEpsilon < right.g) {
        return true;
    }
    if (right.g + kTieEpsilon < left.g) {
        return false;
    }
    return lhs < rhs;
}

void AStarPlanner::HeapPush(std::size_t index) {
    std::size_t position = heap_size_++;
    heap_[position] = index;
    heap_positions_[index] = static_cast<std::int32_t>(position);
    while (position > 0) {
        const std::size_t parent = (position - 1U) / 2U;
        if (!IsBetter(heap_[position], heap_[parent])) {
            break;
        }
        std::swap(heap_[position], heap_[parent]);
        heap_positions_[heap_[position]] = static_cast<std::int32_t>(position);
        heap_positions_[heap_[parent]] = static_cast<std::int32_t>(parent);
        position = parent;
    }
}

std::size_t AStarPlanner::HeapPop() {
    const std::size_t result = static_cast<std::size_t>(heap_[0]);
    heap_positions_[result] = -1;
    --heap_size_;
    if (heap_size_ == 0) {
        return result;
    }
    heap_[0] = heap_[heap_size_];
    heap_positions_[heap_[0]] = 0;
    std::size_t position = 0;
    while (true) {
        const std::size_t left = position * 2U + 1U;
        if (left >= heap_size_) {
            break;
        }
        const std::size_t right = left + 1U;
        std::size_t child = left;
        if (right < heap_size_ && IsBetter(heap_[right], heap_[left])) {
            child = right;
        }
        if (!IsBetter(heap_[child], heap_[position])) {
            break;
        }
        std::swap(heap_[position], heap_[child]);
        heap_positions_[heap_[position]] = static_cast<std::int32_t>(position);
        heap_positions_[heap_[child]] = static_cast<std::int32_t>(child);
        position = child;
    }
    return result;
}

void AStarPlanner::HeapRemove(std::size_t index) {
    const std::int32_t stored_position = heap_positions_[index];
    if (stored_position < 0) {
        return;
    }
    const std::size_t position = static_cast<std::size_t>(stored_position);
    heap_positions_[index] = -1;
    --heap_size_;
    if (position == heap_size_) {
        return;
    }
    heap_[position] = heap_[heap_size_];
    heap_positions_[heap_[position]] = static_cast<std::int32_t>(position);
    if (position > 0 &&
        IsBetter(heap_[position], heap_[(position - 1U) / 2U])) {
        std::size_t current = position;
        while (current > 0) {
            const std::size_t parent = (current - 1U) / 2U;
            if (!IsBetter(heap_[current], heap_[parent])) {
                break;
            }
            std::swap(heap_[current], heap_[parent]);
            heap_positions_[heap_[current]] =
                static_cast<std::int32_t>(current);
            heap_positions_[heap_[parent]] = static_cast<std::int32_t>(parent);
            current = parent;
        }
        return;
    }
    std::size_t current = position;
    while (true) {
        const std::size_t left = current * 2U + 1U;
        if (left >= heap_size_) {
            break;
        }
        const std::size_t right = left + 1U;
        std::size_t child = left;
        if (right < heap_size_ && IsBetter(heap_[right], heap_[left])) {
            child = right;
        }
        if (!IsBetter(heap_[child], heap_[current])) {
            break;
        }
        std::swap(heap_[current], heap_[child]);
        heap_positions_[heap_[current]] = static_cast<std::int32_t>(current);
        heap_positions_[heap_[child]] = static_cast<std::int32_t>(child);
        current = child;
    }
}

float AStarPlanner::Heuristic(GridCell from, GridCell to) const {
    const float dx = std::abs(static_cast<float>(from.x) - to.x);
    const float dy = std::abs(static_cast<float>(from.y) - to.y);
    const float diagonal = std::min(dx, dy);
    return (dx + dy - diagonal) + 1.41421356237F * diagonal;
}

float AStarPlanner::StepCost(const NavCell& cell, float distance) const {
    const NavCellState state = GetNavCellState(cell);
    if (state == NavCellState::Free) {
        return distance;
    }
    const float normalized = static_cast<float>(cell.cost) / 255.0F;
    return distance * (1.0F + config_.clearance_penalty * normalized);
}

PlanStatus AStarPlanner::Plan(const NavMapView& map, GridCell start,
                              GridCell goal, PlannedPath& output) {
    output = PlannedPath{};
    output.map_version = map.version;
    Reset();

    if (!map.IsValid() || map.cell_count > records_.size()) {
        output.status = PlanStatus::InvalidMap;
        return output.status;
    }
    if (!map.InBounds(static_cast<int>(start.x), static_cast<int>(start.y))) {
        output.status = PlanStatus::StartOutOfBounds;
        return output.status;
    }
    if (!map.InBounds(static_cast<int>(goal.x), static_cast<int>(goal.y))) {
        output.status = PlanStatus::GoalOutOfBounds;
        return output.status;
    }
    if (!map.IsTraversable(start, config_.allow_unknown,
                           config_.allow_inflated)) {
        output.status = PlanStatus::StartOccupied;
        return output.status;
    }
    if (!map.IsTraversable(goal, config_.allow_unknown,
                           config_.allow_inflated)) {
        output.status = PlanStatus::GoalOccupied;
        return output.status;
    }

    const std::size_t start_index = Index(map, start);
    const std::size_t goal_index = Index(map, goal);
    if (start_index == goal_index) {
        output.points[0].cell = start;
        output.points[0].position = Vector2f(map.CellCenter(start));
        output.size = 1;
        output.status = PlanStatus::Success;
        return output.status;
    }

    const std::size_t cell_count = map.cell_count;
    for (std::size_t index = 0; index < cell_count; ++index) {
        records_[index] = NodeRecord{};
        heap_positions_[index] = -1;
    }

    records_[start_index].g = 0.0F;
    const float start_h = Heuristic(start, goal);
    records_[start_index].f = start_h *
                               SafeHeuristicWeight(config_.heuristic_weight);
    records_[start_index].state = kOpen;
    HeapPush(start_index);

    const auto begin = std::chrono::steady_clock::now();
    while (heap_size_ > 0) {
        if (last_expansions_ >= config_.max_expansions) {
            output.status = PlanStatus::Timeout;
            return output.status;
        }
        const std::size_t current_index = HeapPop();
        NodeRecord& current_record = records_[current_index];
        current_record.state = kClosed;
        ++last_expansions_;
        if (current_index == goal_index) {
            return BuildPath(map, start_index, goal_index, output);
        }
        if (config_.max_planning_microseconds != 0 &&
            (last_expansions_ & 0x0FU) == 0U) {
            const auto elapsed = std::chrono::duration_cast<
                std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                           begin);
            if (elapsed.count() >= config_.max_planning_microseconds) {
                output.status = PlanStatus::Timeout;
                return output.status;
            }
        }

        const GridCell current = CellFromIndex(map, current_index);
        for (std::size_t neighbor_index = 0;
             neighbor_index < kNeighborX.size(); ++neighbor_index) {
            const int next_x = static_cast<int>(current.x) +
                               kNeighborX[neighbor_index];
            const int next_y = static_cast<int>(current.y) +
                               kNeighborY[neighbor_index];
            if (!map.InBounds(next_x, next_y)) {
                continue;
            }
            GridCell next;
            next.x = static_cast<std::uint16_t>(next_x);
            next.y = static_cast<std::uint16_t>(next_y);
            if (!map.IsTraversable(next, config_.allow_unknown,
                                   config_.allow_inflated)) {
                continue;
            }
            if (kNeighborX[neighbor_index] != 0 &&
                kNeighborY[neighbor_index] != 0) {
                GridCell side_x = current;
                side_x.x = static_cast<std::uint16_t>(next_x);
                GridCell side_y = current;
                side_y.y = static_cast<std::uint16_t>(next_y);
                if (!map.IsTraversable(side_x, config_.allow_unknown,
                                       config_.allow_inflated) ||
                    !map.IsTraversable(side_y, config_.allow_unknown,
                                       config_.allow_inflated)) {
                    continue;
                }
            }

            const std::size_t next_index = Index(map, next);
            NodeRecord& next_record = records_[next_index];
            if (next_record.state == kClosed) {
                continue;
            }
            const NavCell* next_cell = map.Cell(next_x, next_y);
            if (next_cell == nullptr) {
                continue;
            }
            const float candidate_g =
                current_record.g +
                StepCost(*next_cell, kNeighborDistance[neighbor_index]);
            if (next_record.state != kOpen || candidate_g < next_record.g) {
                next_record.parent = static_cast<std::int32_t>(current_index);
                next_record.g = candidate_g;
                const float next_h = Heuristic(next, goal);
                next_record.f = next_record.g +
                                SafeHeuristicWeight(config_.heuristic_weight) *
                                    next_h;
                if (next_record.state != kOpen) {
                    next_record.state = kOpen;
                    HeapPush(next_index);
                } else {
                    HeapRemove(next_index);
                    HeapPush(next_index);
                }
            }
        }
    }

    output.status = PlanStatus::NoPath;
    return output.status;
}

PlanStatus AStarPlanner::Plan(const NavMapView& map, const Vector2d& start,
                              const Vector2d& goal,
                              PlannedPath& output) {
    GridCell start_cell;
    GridCell goal_cell;
    if (!map.IsValid()) {
        output = PlannedPath{};
        output.map_version = map.version;
        output.status = PlanStatus::InvalidMap;
        return output.status;
    }
    if (!map.WorldToCell(start, start_cell)) {
        output = PlannedPath{};
        output.map_version = map.version;
        output.status = PlanStatus::StartOutOfBounds;
        return output.status;
    }
    if (!map.WorldToCell(goal, goal_cell)) {
        output = PlannedPath{};
        output.map_version = map.version;
        output.status = PlanStatus::GoalOutOfBounds;
        return output.status;
    }
    return Plan(map, start_cell, goal_cell, output);
}

PlanStatus AStarPlanner::BuildPath(const NavMapView& map,
                                   std::size_t start_index,
                                   std::size_t goal_index,
                                   PlannedPath& output) {
    std::size_t current = goal_index;
    while (true) {
        if (output.size >= output.points.size()) {
            output.status = PlanStatus::PathCapacityExceeded;
            output.size = 0;
            return output.status;
        }
        const GridCell cell = CellFromIndex(map, current);
        output.points[output.size].cell = cell;
        output.points[output.size].position = Vector2f(map.CellCenter(cell));
        ++output.size;
        if (current == start_index) {
            break;
        }
        const std::int32_t parent = records_[current].parent;
        if (parent < 0) {
            output.size = 0;
            output.status = PlanStatus::NoPath;
            return output.status;
        }
        current = static_cast<std::size_t>(parent);
    }
    std::reverse(output.points.begin(), output.points.begin() + output.size);
    output.status = PlanStatus::Success;
    return output.status;
}

}  // namespace velaros_navigation
