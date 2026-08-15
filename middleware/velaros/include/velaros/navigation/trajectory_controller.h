#pragma once

#include "velaros/navigation/types.h"

#include <cstdint>

namespace velaros_navigation {

struct TrajectoryControllerConfig {
    float max_linear_speed = 0.8F;
    float min_linear_speed = 0.0F;
    float max_angular_speed = 1.5F;
    float min_angular_speed = -1.5F;
    float max_linear_acceleration = 1.0F;
    float max_linear_deceleration = 1.0F;
    float max_angular_acceleration = 3.0F;
    float max_angular_deceleration = 3.0F;
    float lookahead_distance = 0.8F;
    float min_lookahead_distance = 0.3F;
    float max_lookahead_distance = 0.9F;
    float lookahead_time = 1.5F;
    float goal_position_tolerance = 0.20F;
    float goal_heading_tolerance = 0.12F;
    float rotate_to_heading_angular_speed = 1.8F;
    float rotate_to_heading_min_angle = 0.785F;
    float min_approach_linear_speed = 0.05F;
    float approach_velocity_scaling_distance = 0.6F;
    float regulated_linear_scaling_min_radius = 0.9F;
    float regulated_linear_scaling_min_speed = 0.25F;
    float cost_scaling_distance = 0.6F;
    float cost_scaling_gain = 1.0F;
    float curvature_lookahead_distance = 0.6F;
    float robot_radius = 0.25F;
    float control_period = 0.05F;
    double pose_timeout = 0.25;
    double map_timeout = 0.75;
    double goal_timeout = 30.0;
    float max_allowed_time_to_collision = 1.0F;
    bool use_velocity_scaled_lookahead = false;
    bool use_regulated_linear_velocity_scaling = true;
    bool use_cost_regulated_linear_velocity_scaling = true;
    bool use_fixed_curvature_lookahead = false;
    bool use_rotate_to_heading = true;
    bool use_collision_detection = true;
    bool use_dynamic_window = true;
    bool use_lio_velocity_for_dynamic_window = true;
    bool allow_reversing = false;
    bool require_goal_heading = false;
    FrameId body_frame{};
};

class TrajectoryController final {
public:
    explicit TrajectoryController(
        TrajectoryControllerConfig config = TrajectoryControllerConfig{})
        : config_(config) {}

    void Reset();

    void SetEmergencyStop(bool enabled) { emergency_stop_ = enabled; }

    bool EmergencyStop() const { return emergency_stop_; }

    ControlStatus Compute(const PoseSnapshot& pose, const NavMapView& map,
                          const PlannedPath& path, const NavigationGoal& goal,
                          double now, VelocityCommand& output);

    const TrajectoryControllerConfig& Config() const { return config_; }

    void SetConfig(TrajectoryControllerConfig config) { config_ = config; }

private:
    static double WrapAngle(double value);

    static double Clamp(double value, double lower, double upper);

    static bool Fresh(double now, double timestamp, double timeout);

    void Stop(double now, ControlStatus status, VelocityCommand& output);

    TrajectoryControllerConfig config_{};
    Vector2d previous_command_ = Vector2d::Zero();
    double previous_yaw_rate_ = 0.0;
    double previous_timestamp_ = -1.0;
    bool emergency_stop_ = false;
};

}  // namespace velaros_navigation
