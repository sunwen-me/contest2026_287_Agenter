#include "velaros/navigation/navigation_pipeline.h"

#include <cmath>

namespace velaros_navigation {

NavigationPipeline::NavigationPipeline(NavigationSnapshotBuffer<>& snapshots,
                                       NavigationPipelineConfig config)
    : snapshots_(&snapshots), config_(config), planner_(config.planner),
      controller_(config.controller) {}

void NavigationPipeline::Reset() {
    goal_ = NavigationGoal{};
    path_ = PlannedPath{};
    goal_generation_ = 0;
    last_plan_time_ = -1.0;
    last_plan_status_ = PlanStatus::None;
    goal_set_ = false;
    controller_.Reset();
    planner_.Reset();
}

bool NavigationPipeline::SetGoal(const NavigationGoal& goal, double now) {
    if (!goal.valid || !IsFinite(goal) || !goal.frame.Valid()) {
        return false;
    }
    goal_ = goal;
    if (goal_.timestamp <= 0.0 || !std::isfinite(goal_.timestamp)) {
        goal_.timestamp = now;
    }
    ++goal_generation_;
    if (goal_generation_ == 0) {
        goal_generation_ = 1;
    }
    path_ = PlannedPath{};
    path_.goal_generation = goal_generation_;
    last_plan_time_ = -1.0;
    last_plan_status_ = PlanStatus::None;
    goal_set_ = true;
    return true;
}

void NavigationPipeline::ClearGoal() {
    goal_set_ = false;
    goal_ = NavigationGoal{};
    path_ = PlannedPath{};
    last_plan_status_ = PlanStatus::None;
    last_plan_time_ = -1.0;
}

bool NavigationPipeline::ShouldReplan(const NavigationSnapshotView& snapshot,
                                      double now) const {
    if (!path_.valid() || path_.map_version != snapshot.map.version ||
        path_.goal_generation != goal_generation_) {
        return true;
    }
    if (last_plan_time_ < 0.0 || now < last_plan_time_ ||
        now - last_plan_time_ >= config_.planner_period) {
        return true;
    }
    const Vector2d position(snapshot.pose.T_map_body.translation.x(),
                            snapshot.pose.T_map_body.translation.y());
    const Vector2d first = path_.points[0].position;
    return (position - first).norm() >= config_.replan_distance;
}

ControlStatus NavigationPipeline::Tick(double now, VelocityCommand& output) {
    output = VelocityCommand{};
    output.timestamp = now;
    output.frame = config_.controller.body_frame;
    output.valid = true;
    output.stop = true;
    if (snapshots_ == nullptr) {
        output.status = ControlStatus::InvalidMap;
        return output.status;
    }
    if (!goal_set_) {
        output.status = ControlStatus::StaleGoal;
        return output.status;
    }

    auto guard = snapshots_->AcquireLatest();
    if (!guard.Valid()) {
        output.status = ControlStatus::InvalidMap;
        return output.status;
    }
    const NavigationSnapshotView snapshot = guard.View();
    if (!snapshot.pose.valid || !snapshot.map.IsValid()) {
        output.status = !snapshot.pose.valid ? ControlStatus::InvalidPose
                                             : ControlStatus::InvalidMap;
        return output.status;
    }
    if (snapshot.pose.frame != snapshot.map.frame ||
        goal_.frame != snapshot.map.frame) {
        output.status = ControlStatus::FrameMismatch;
        return output.status;
    }

    if (ShouldReplan(snapshot, now)) {
        path_.goal_generation = goal_generation_;
        const Vector2d pose_position(snapshot.pose.T_map_body.translation.x(),
                                     snapshot.pose.T_map_body.translation.y());
        last_plan_status_ = planner_.Plan(snapshot.map, pose_position,
                                          goal_.position, path_);
        last_plan_time_ = now;
        if (last_plan_status_ != PlanStatus::Success) {
            output.status = ControlStatus::NoPath;
            return output.status;
        }
    }

    return controller_.Compute(snapshot.pose, snapshot.map, path_, goal_, now,
                               output);
}

}  // namespace velaros_navigation
