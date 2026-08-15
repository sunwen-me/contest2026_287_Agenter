#pragma once

#include "velaros/navigation/astar_planner.h"
#include "velaros/navigation/snapshot_publisher.h"
#include "velaros/navigation/trajectory_controller.h"

#include <cstdint>

namespace velaros_navigation {

struct NavigationPipelineConfig {
    AStarPlannerConfig planner{};
    TrajectoryControllerConfig controller{};
    double planner_period = 0.10;
    double replan_distance = 0.75;
};

class NavigationPipeline final {
public:
    explicit NavigationPipeline(
        NavigationSnapshotBuffer<>& snapshots,
        NavigationPipelineConfig config = NavigationPipelineConfig{});

    void Reset();

    bool SetGoal(const NavigationGoal& goal, double now);

    void ClearGoal();

    bool HasGoal() const { return goal_set_; }

    void SetEmergencyStop(bool enabled) {
        controller_.SetEmergencyStop(enabled);
    }

    ControlStatus Tick(double now, VelocityCommand& output);

    const PlannedPath& Path() const { return path_; }

    PlanStatus LastPlanStatus() const { return last_plan_status_; }

    const NavigationPipelineConfig& Config() const { return config_; }

private:
    bool ShouldReplan(const NavigationSnapshotView& snapshot, double now) const;

    NavigationSnapshotBuffer<>* snapshots_ = nullptr;
    NavigationPipelineConfig config_{};
    AStarPlanner planner_;
    TrajectoryController controller_;
    NavigationGoal goal_{};
    PlannedPath path_{};
    std::uint64_t goal_generation_ = 0;
    double last_plan_time_ = -1.0;
    PlanStatus last_plan_status_ = PlanStatus::None;
    bool goal_set_ = false;
};

}  // namespace velaros_navigation
