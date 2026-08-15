#include "velaros/navigation/trajectory_controller.h"

#include "velaros/navigation/dynamic_window_pure_pursuit.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace velaros_navigation {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEpsilon = 1e-9;

double Clamp(double value, double lower, double upper) {
    return std::max(lower, std::min(value, upper));
}

double PositiveOrZero(double value) {
    return std::isfinite(value) ? std::max(0.0, value) : 0.0;
}

double ComputeCurvature(const Vector2d& body_point) {
    const double distance_squared = body_point.squaredNorm();
    if (!std::isfinite(distance_squared) || distance_squared <= 1e-6) {
        return 0.0;
    }
    return 2.0 * body_point.y() / distance_squared;
}

std::size_t FindNearestPathPoint(const PlannedPath& path,
                                 const Vector2d& position) {
    std::size_t nearest = 0;
    double nearest_distance = std::numeric_limits<double>::max();
    for (std::size_t index = 0; index < path.size; ++index) {
        const Vector2d difference =
            path.points[index].position - position;
        const double distance = difference.squaredNorm();
        if (distance < nearest_distance) {
            nearest_distance = distance;
            nearest = index;
        }
    }
    return nearest;
}

Vector2d FindLookaheadPoint(const PlannedPath& path,
                                   const Vector2d& position,
                                   std::size_t nearest,
                                   double lookahead_distance) {
    const double radius = PositiveOrZero(lookahead_distance);
    Vector2d target =
        path.points[std::min(nearest, path.size - 1U)].position;
    if (radius <= kEpsilon) {
        return target;
    }

    for (std::size_t index = nearest; index + 1U < path.size; ++index) {
        const Vector2d start =
            path.points[index].position;
        const Vector2d end =
            path.points[index + 1U].position;
        const Vector2d segment = end - start;
        const double segment_squared = segment.squaredNorm();
        if (segment_squared <= kEpsilon) {
            target = end;
            continue;
        }

        const Vector2d relative_start = start - position;
        const Vector2d relative_end = end - position;
        const double end_distance = relative_end.norm();
        if (end_distance < radius) {
            target = end;
            continue;
        }

        // Intersect this path segment with the lookahead circle. The path is
        // already bounded, so this quadratic has a fixed, predictable cost.
        const double a = segment_squared;
        const double b = 2.0 * relative_start.dot(segment);
        const double c = relative_start.squaredNorm() - radius * radius;
        const double discriminant = b * b - 4.0 * a * c;
        if (discriminant >= 0.0 && std::isfinite(discriminant)) {
            const double root = std::sqrt(discriminant);
            const double first = (-b - root) / (2.0 * a);
            const double second = (-b + root) / (2.0 * a);
            double parameter = std::numeric_limits<double>::infinity();
            if (first >= 0.0 && first <= 1.0) {
                parameter = first;
            }
            if (second >= 0.0 && second <= 1.0) {
                parameter = std::min(parameter, second);
            }
            if (std::isfinite(parameter)) {
                return start + parameter * segment;
            }
        }
        target = end;
    }
    return target;
}

double RemainingPathLength(const PlannedPath& path, std::size_t nearest,
                           const Vector2d& position) {
    if (path.size == 0 || nearest >= path.size) {
        return 0.0;
    }
    double length =
        (path.points[nearest].position - position).norm();
    for (std::size_t index = nearest; index + 1U < path.size; ++index) {
        length += (path.points[index + 1U].position -
                   path.points[index].position)
                      .norm();
    }
    return length;
}

double RegulateByCurvature(double velocity, double curvature,
                           double minimum_radius) {
    const double radius_limit = PositiveOrZero(minimum_radius);
    if (radius_limit <= kEpsilon || std::abs(curvature) <= kEpsilon) {
        return velocity;
    }
    const double radius = 1.0 / std::abs(curvature);
    if (radius >= radius_limit) {
        return velocity;
    }
    const double scale =
        Clamp(1.0 - std::abs(radius - radius_limit) / radius_limit, 0.0, 1.0);
    return velocity * scale;
}

double RegulateByClearance(double velocity, const NavMapView& map,
                           const Vector2d& position, double distance,
                           double gain) {
    const double limit = PositiveOrZero(distance);
    if (limit <= kEpsilon) {
        return velocity;
    }
    const double clearance = map.DistanceToOccupied(position, limit);
    if (clearance >= limit) {
        return velocity;
    }
    const double scale = Clamp(PositiveOrZero(gain) * clearance / limit,
                               0.0, 1.0);
    return velocity * scale;
}

bool CollisionImminent(const NavMapView& map, const Vector2d& start,
                       double yaw, double linear_velocity,
                       double angular_velocity, double carrot_distance,
                       double max_time, double robot_radius) {
    GridCell start_cell;
    if (!map.WorldToCell(start, start_cell) ||
        !map.IsTraversable(start_cell, false, false)) {
        return true;
    }
    if (!std::isfinite(max_time) || max_time <= 0.0 ||
        (std::abs(linear_velocity) <= kEpsilon &&
         std::abs(angular_velocity) <= kEpsilon)) {
        return false;
    }

    const double effective_radius =
        std::max(PositiveOrZero(robot_radius),
                 static_cast<double>(map.resolution));
    const double spatial_speed = std::max(
        std::abs(linear_velocity), std::abs(angular_velocity) * effective_radius);
    double step = static_cast<double>(map.resolution) / spatial_speed;
    step = Clamp(step, 0.005, 0.10);
    const std::size_t steps = std::min<std::size_t>(
        512U, static_cast<std::size_t>(std::ceil(max_time / step)));

    Vector2d position = start;
    double heading = yaw;
    double elapsed = 0.0;
    for (std::size_t index = 0; index < steps; ++index) {
        const double duration = std::min(step, max_time - elapsed);
        if (duration <= 0.0) {
            break;
        }
        if (std::abs(angular_velocity) > kEpsilon) {
            const double next_heading =
                heading + angular_velocity * duration;
            position.x() += linear_velocity / angular_velocity *
                            (std::sin(next_heading) - std::sin(heading));
            position.y() += linear_velocity / angular_velocity *
                            (-std::cos(next_heading) + std::cos(heading));
            heading = next_heading;
        } else {
            position += duration * linear_velocity *
                        Vector2d(std::cos(heading), std::sin(heading));
        }
        elapsed += duration;

        if (std::abs(linear_velocity) > kEpsilon &&
            (position - start).norm() > std::max(carrot_distance, 0.0)) {
            break;
        }
        GridCell cell;
        if (!map.WorldToCell(position, cell) ||
            !map.IsTraversable(cell, false, false)) {
            return true;
        }
    }
    return false;
}

}  // namespace

void TrajectoryController::Reset() {
    previous_command_.setZero();
    previous_yaw_rate_ = 0.0;
    previous_timestamp_ = -1.0;
    emergency_stop_ = false;
}

double TrajectoryController::WrapAngle(double value) {
    while (value > kPi) {
        value -= 2.0 * kPi;
    }
    while (value < -kPi) {
        value += 2.0 * kPi;
    }
    return value;
}

double TrajectoryController::Clamp(double value, double lower, double upper) {
    return std::max(lower, std::min(value, upper));
}

bool TrajectoryController::Fresh(double now, double timestamp, double timeout) {
    if (!std::isfinite(now) || !std::isfinite(timestamp) || timeout < 0.0) {
        return false;
    }
    return now >= timestamp && now - timestamp <= timeout;
}

void TrajectoryController::Stop(double now, ControlStatus status,
                                VelocityCommand& output) {
    output = VelocityCommand{};
    output.timestamp = now;
    output.frame = config_.body_frame;
    output.status = status;
    // A stop is a valid bounded command even when its cause is stale or
    // invalid localization/map data.
    output.valid = true;
    output.stop = true;
    previous_command_.setZero();
    previous_yaw_rate_ = 0.0;
    previous_timestamp_ = now;
}

ControlStatus TrajectoryController::Compute(const PoseSnapshot& pose,
                                            const NavMapView& map,
                                            const PlannedPath& path,
                                            const NavigationGoal& goal,
                                            double now,
                                            VelocityCommand& output) {
    if (emergency_stop_) {
        Stop(now, ControlStatus::EmergencyStop, output);
        return output.status;
    }
    if (!pose.valid || !IsFinite(pose)) {
        Stop(now, ControlStatus::InvalidPose, output);
        return output.status;
    }
    if (!map.IsValid()) {
        Stop(now, ControlStatus::InvalidMap, output);
        return output.status;
    }
    if (!Fresh(now, pose.timestamp, config_.pose_timeout)) {
        Stop(now, ControlStatus::StalePose, output);
        return output.status;
    }
    if (!Fresh(now, map.timestamp, config_.map_timeout)) {
        Stop(now, ControlStatus::StaleMap, output);
        return output.status;
    }
    if (!goal.valid || !IsFinite(goal) ||
        !Fresh(now, goal.timestamp, config_.goal_timeout)) {
        Stop(now, ControlStatus::StaleGoal, output);
        return output.status;
    }
    if (pose.frame != map.frame || goal.frame != map.frame) {
        Stop(now, ControlStatus::FrameMismatch, output);
        return output.status;
    }
    if (!path.valid() || path.map_version != map.version) {
        Stop(now, ControlStatus::NoPath, output);
        return output.status;
    }

    for (std::size_t index = 0; index < path.size; ++index) {
        if (!map.IsTraversable(path.points[index].cell, false, false)) {
            Stop(now, ControlStatus::UnsafePath, output);
            return output.status;
        }
    }

    const Vector2d pose_position(pose.T_map_body.translation.x(),
                                 pose.T_map_body.translation.y());
    const double yaw = std::atan2(pose.T_map_body.rotation(1, 0),
                                  pose.T_map_body.rotation(0, 0));
    const std::size_t nearest = FindNearestPathPoint(path, pose_position);

    double dt = PositiveOrZero(config_.control_period);
    if (dt <= kEpsilon) {
        dt = 0.05;
    }
    if (previous_timestamp_ >= 0.0 && now > previous_timestamp_) {
        dt = std::min(now - previous_timestamp_, 0.25);
    }
    if (!std::isfinite(dt) || dt <= 0.0) {
        dt = 0.05;
    }

    // Nav2's DWPP uses the last command for open-loop speed control. This
    // integration has a measured LIO velocity, so prefer it after converting
    // the map-frame snapshot into body coordinates and retain the command as a
    // bounded fallback for bad or implausible estimates.
    double current_linear_velocity = previous_command_.x();
    double current_yaw_rate = previous_yaw_rate_;
    if (config_.use_lio_velocity_for_dynamic_window) {
        const Vector2d map_velocity = pose.planar_velocity;
        const Vector2d body_velocity(
            std::cos(yaw) * map_velocity.x() +
                std::sin(yaw) * map_velocity.y(),
            -std::sin(yaw) * map_velocity.x() +
                std::cos(yaw) * map_velocity.y());
        const double linear_limit = std::max(
            1.0, 2.0 * PositiveOrZero(config_.max_linear_speed));
        const double angular_limit = std::max(
            1.0, 2.0 * PositiveOrZero(config_.max_angular_speed));
        if (body_velocity.allFinite() && std::isfinite(pose.yaw_rate) &&
            std::abs(body_velocity.x()) <= linear_limit &&
            std::abs(pose.yaw_rate) <= angular_limit) {
            current_linear_velocity = body_velocity.x();
            current_yaw_rate = pose.yaw_rate;
        }
    }
    const double lookahead_speed = std::abs(current_linear_velocity);
    double lookahead_distance = PositiveOrZero(config_.lookahead_distance);
    if (config_.use_velocity_scaled_lookahead) {
        lookahead_distance = Clamp(
            lookahead_speed * PositiveOrZero(config_.lookahead_time),
            PositiveOrZero(config_.min_lookahead_distance),
            std::max(PositiveOrZero(config_.min_lookahead_distance),
                     PositiveOrZero(config_.max_lookahead_distance)));
    }
    const Vector2d target = FindLookaheadPoint(
        path, pose_position, nearest, lookahead_distance);

    const double goal_distance = (goal.position - pose_position).norm();
    const double current_yaw = yaw;
    const bool at_goal_position =
        goal_distance <= PositiveOrZero(config_.goal_position_tolerance);
    bool rotate_to_heading = false;
    double requested_heading_error = 0.0;
    if (at_goal_position) {
        if (config_.require_goal_heading && !goal.has_heading) {
            Stop(now, ControlStatus::StaleGoal, output);
            return output.status;
        }
        if (!goal.has_heading ||
            std::abs(WrapAngle(goal.heading - current_yaw)) <=
                PositiveOrZero(config_.goal_heading_tolerance)) {
            Stop(now, ControlStatus::GoalReached, output);
            return output.status;
        }
        rotate_to_heading = true;
        requested_heading_error = WrapAngle(goal.heading - current_yaw);
    }

    const double cosine = std::cos(yaw);
    const double sine = std::sin(yaw);
    const Vector2d map_delta = target - pose_position;
    const Vector2d body_delta(cosine * map_delta.x() +
                                         sine * map_delta.y(),
                                     -sine * map_delta.x() +
                                         cosine * map_delta.y());
    const double target_distance = body_delta.norm();
    const double path_heading_error =
        std::atan2(body_delta.y(), body_delta.x());
    if (!rotate_to_heading && config_.use_rotate_to_heading &&
        std::abs(path_heading_error) >
            PositiveOrZero(config_.rotate_to_heading_min_angle)) {
        rotate_to_heading = true;
        requested_heading_error = path_heading_error;
    }

    const double direction =
        config_.allow_reversing && body_delta.x() < 0.0 ? -1.0 : 1.0;
    Vector2d curvature_body_delta = body_delta;
    if (config_.use_fixed_curvature_lookahead) {
        const double curvature_target_distance =
            PositiveOrZero(config_.curvature_lookahead_distance);
        const Vector2d curvature_target = FindLookaheadPoint(
            path, pose_position, nearest, curvature_target_distance);
        const Vector2d curvature_map_delta =
            curvature_target - pose_position;
        curvature_body_delta = Vector2d(
            cosine * curvature_map_delta.x() + sine * curvature_map_delta.y(),
            -sine * curvature_map_delta.x() + cosine * curvature_map_delta.y());
    }
    const double curvature = ComputeCurvature(curvature_body_delta);

    double linear_velocity = 0.0;
    double angular_velocity = 0.0;
    if (rotate_to_heading) {
        linear_velocity = 0.0;
        const double rotate_speed = PositiveOrZero(
            config_.rotate_to_heading_angular_speed);
        const double max_angular = PositiveOrZero(config_.max_angular_speed);
        const double max_acceleration =
            PositiveOrZero(config_.max_angular_acceleration);
        const double stop_speed = std::sqrt(
            2.0 * max_acceleration * std::abs(requested_heading_error));
        const double desired_magnitude =
            std::min({rotate_speed, max_angular, stop_speed});
        const double desired =
            requested_heading_error >= 0.0 ? desired_magnitude
                                           : -desired_magnitude;
        const double step = max_acceleration * dt;
        angular_velocity = Clamp(
            desired, current_yaw_rate - step, current_yaw_rate + step);
    } else {
        const double max_linear = PositiveOrZero(config_.max_linear_speed);
        double curvature_velocity = max_linear;
        if (config_.use_regulated_linear_velocity_scaling) {
            curvature_velocity = RegulateByCurvature(
                max_linear, curvature,
                config_.regulated_linear_scaling_min_radius);
        }
        double clearance_velocity = max_linear;
        if (config_.use_cost_regulated_linear_velocity_scaling) {
            clearance_velocity = RegulateByClearance(
                max_linear, map, pose_position,
                config_.cost_scaling_distance, config_.cost_scaling_gain);
        }
        double regulated_velocity = std::min(curvature_velocity,
                                             clearance_velocity);
        regulated_velocity = std::max(
            regulated_velocity,
            PositiveOrZero(config_.regulated_linear_scaling_min_speed));

        const double approach_distance =
            PositiveOrZero(config_.approach_velocity_scaling_distance);
        const double remaining_path =
            RemainingPathLength(path, nearest, pose_position);
        if (approach_distance > kEpsilon &&
            remaining_path < approach_distance) {
            const Vector2d final_point =
                path.points[path.size - 1U].position;
            const double scale = Clamp(
                (final_point - pose_position).norm() / approach_distance,
                0.0, 1.0);
            double approach_velocity = regulated_velocity * scale;
            approach_velocity = std::max(
                approach_velocity,
                PositiveOrZero(config_.min_approach_linear_speed));
            regulated_velocity = std::min(regulated_velocity, approach_velocity);
        }
        regulated_velocity = Clamp(
            std::abs(regulated_velocity), 0.0, max_linear);
        if (direction < 0.0) {
            regulated_velocity = -regulated_velocity;
        }

        if (!config_.allow_reversing && body_delta.x() <= 0.0) {
            regulated_velocity = 0.0;
        }
        if (!config_.use_dynamic_window) {
            linear_velocity = regulated_velocity;
            angular_velocity = linear_velocity * curvature;
        } else {
            const double min_linear = std::isfinite(config_.min_linear_speed)
                                          ? config_.min_linear_speed
                                          : 0.0;
            const double min_angular = std::isfinite(config_.min_angular_speed)
                                           ? config_.min_angular_speed
                                           : -PositiveOrZero(
                                                 config_.max_angular_speed);
            const auto selected = dynamic_window_pure_pursuit::ComputeVelocity(
                current_linear_velocity, current_yaw_rate,
                PositiveOrZero(config_.max_linear_speed), min_linear,
                PositiveOrZero(config_.max_angular_speed), min_angular,
                PositiveOrZero(config_.max_linear_acceleration),
                PositiveOrZero(config_.max_linear_deceleration),
                PositiveOrZero(config_.max_angular_acceleration),
                PositiveOrZero(config_.max_angular_deceleration),
                std::abs(regulated_velocity), curvature, direction, dt);
            linear_velocity = selected.linear_velocity;
            angular_velocity = selected.angular_velocity;
        }
    }

    const double max_linear_speed = PositiveOrZero(config_.max_linear_speed);
    const double max_angular_speed = PositiveOrZero(config_.max_angular_speed);
    if (!std::isfinite(linear_velocity) || !std::isfinite(angular_velocity) ||
        std::abs(linear_velocity) > max_linear_speed + 1e-6 ||
        std::abs(angular_velocity) > max_angular_speed + 1e-6) {
        Stop(now, ControlStatus::InvalidMap, output);
        return output.status;
    }

    if (config_.use_collision_detection &&
        CollisionImminent(map, pose_position, yaw, linear_velocity,
                          angular_velocity, target_distance,
                          PositiveOrZero(config_.max_allowed_time_to_collision),
                          PositiveOrZero(config_.robot_radius))) {
        Stop(now, ControlStatus::UnsafePath, output);
        return output.status;
    }

    output = VelocityCommand{};
    output.timestamp = now;
    output.frame = config_.body_frame;
    output.planar_velocity = Vector2d(linear_velocity, 0.0);
    output.yaw_rate = angular_velocity;
    output.status = ControlStatus::Commanded;
    output.valid = true;
    output.stop = std::abs(linear_velocity) < 1e-9 &&
                  std::abs(angular_velocity) < 1e-9;
    previous_command_ = output.planar_velocity;
    previous_yaw_rate_ = output.yaw_rate;
    previous_timestamp_ = now;
    return output.status;
}

}  // namespace velaros_navigation
