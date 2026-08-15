#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace velaros_navigation::dynamic_window_pure_pursuit {

// The window is the set of velocities reachable during one control period.
// Deceleration arguments are positive magnitudes at this project boundary;
// the helper applies them in the braking direction internally.
struct DynamicWindowBounds {
    double max_linear_velocity = 0.0;
    double min_linear_velocity = 0.0;
    double max_angular_velocity = 0.0;
    double min_angular_velocity = 0.0;
};

struct VelocityPair {
    double linear_velocity = 0.0;
    double angular_velocity = 0.0;
};

inline DynamicWindowBounds ComputeDynamicWindow(
    double current_linear_velocity, double current_angular_velocity,
    double max_linear_velocity, double min_linear_velocity,
    double max_angular_velocity, double min_angular_velocity,
    double max_linear_acceleration, double max_linear_deceleration,
    double max_angular_acceleration, double max_angular_deceleration,
    double dt) {
    DynamicWindowBounds window;
    constexpr double kVelocityEpsilon = 1e-3;
    const double duration = std::isfinite(dt) && dt > 0.0 ? dt : 0.0;

    const auto compute_dimension = [&](double current, double maximum,
                                       double minimum, double acceleration,
                                       double deceleration) {
        const double accel = std::max(0.0, acceleration);
        const double brake = std::max(0.0, deceleration);
        double candidate_max = 0.0;
        double candidate_min = 0.0;
        if (current > kVelocityEpsilon) {
            candidate_max = current + accel * duration;
            candidate_min = current - brake * duration;
        } else if (current < -kVelocityEpsilon) {
            candidate_max = current + brake * duration;
            candidate_min = current - accel * duration;
        } else {
            candidate_max = current + accel * duration;
            candidate_min = current - accel * duration;
        }
        const double upper = std::max(maximum, minimum);
        const double lower = std::min(maximum, minimum);
        return std::array<double, 2>{
            std::min(candidate_max, upper), std::max(candidate_min, lower)};
    };

    const auto linear = compute_dimension(
        current_linear_velocity, max_linear_velocity, min_linear_velocity,
        max_linear_acceleration, max_linear_deceleration);
    const auto angular = compute_dimension(
        current_angular_velocity, max_angular_velocity, min_angular_velocity,
        max_angular_acceleration, max_angular_deceleration);
    window.max_linear_velocity = linear[0];
    window.min_linear_velocity = linear[1];
    window.max_angular_velocity = angular[0];
    window.min_angular_velocity = angular[1];
    return window;
}

inline void ApplyRegulatedLinearVelocity(
    double regulated_linear_velocity, DynamicWindowBounds& window) {
    const double regulated_min = std::min(0.0, regulated_linear_velocity);
    const double regulated_max = std::max(0.0, regulated_linear_velocity);
    window.min_linear_velocity =
        std::max(window.min_linear_velocity, regulated_min);
    window.max_linear_velocity =
        std::min(window.max_linear_velocity, regulated_max);

    // A regulation limit can sit below the currently reachable velocity. In
    // that case retain the closest reachable boundary instead of producing an
    // inverted window.
    if (window.min_linear_velocity > window.max_linear_velocity) {
        if (window.min_linear_velocity > regulated_max) {
            window.max_linear_velocity = window.min_linear_velocity;
        } else {
            window.min_linear_velocity = window.max_linear_velocity;
        }
    }
}

inline VelocityPair ComputeOptimalVelocityWithinWindow(
    const DynamicWindowBounds& window, double curvature, double direction) {
    VelocityPair result;
    const double sign = direction >= 0.0 ? 1.0 : -1.0;

    if (!std::isfinite(curvature) || std::abs(curvature) < 1e-3) {
        result.linear_velocity = sign >= 0.0 ? window.max_linear_velocity
                                             : window.min_linear_velocity;
        if (window.min_angular_velocity <= 0.0 &&
            0.0 <= window.max_angular_velocity) {
            result.angular_velocity = 0.0;
        } else {
            result.angular_velocity =
                std::abs(window.min_angular_velocity) <=
                        std::abs(window.max_angular_velocity)
                    ? window.min_angular_velocity
                    : window.max_angular_velocity;
        }
        return result;
    }

    // Pure pursuit asks for the line omega = curvature * velocity. First use
    // an exact intersection, preferring the greatest velocity in the travel
    // direction, just like Nav2 DWPP.
    const std::array<std::array<double, 2>, 4> intersections{{
        {{window.min_linear_velocity,
          curvature * window.min_linear_velocity}},
        {{window.max_linear_velocity,
          curvature * window.max_linear_velocity}},
        {{window.min_angular_velocity / curvature,
          window.min_angular_velocity}},
        {{window.max_angular_velocity / curvature,
          window.max_angular_velocity}},
    }};
    double best_linear = -std::numeric_limits<double>::max() * sign;
    double best_angular = 0.0;
    for (const auto& candidate : intersections) {
        if (candidate[0] >= window.min_linear_velocity &&
            candidate[0] <= window.max_linear_velocity &&
            candidate[1] >= window.min_angular_velocity &&
            candidate[1] <= window.max_angular_velocity &&
            candidate[0] * sign > best_linear * sign) {
            best_linear = candidate[0];
            best_angular = candidate[1];
        }
    }
    if (best_linear != -std::numeric_limits<double>::max() * sign) {
        result.linear_velocity = best_linear;
        result.angular_velocity = best_angular;
        return result;
    }

    // If there is no intersection, the closest point of the convex rectangle
    // is one of its corners.
    const std::array<std::array<double, 2>, 4> corners{{
        {{window.min_linear_velocity, window.min_angular_velocity}},
        {{window.min_linear_velocity, window.max_angular_velocity}},
        {{window.max_linear_velocity, window.min_angular_velocity}},
        {{window.max_linear_velocity, window.max_angular_velocity}},
    }};
    const double denominator = std::sqrt(curvature * curvature + 1.0);
    double closest_distance = std::numeric_limits<double>::max();
    best_linear = -std::numeric_limits<double>::max() * sign;
    for (const auto& corner : corners) {
        const double distance =
            std::abs(curvature * corner[0] - corner[1]) / denominator;
        if (distance < closest_distance ||
            (std::abs(distance - closest_distance) <= 1e-3 &&
             corner[0] * sign > best_linear * sign)) {
            closest_distance = distance;
            best_linear = corner[0];
            best_angular = corner[1];
        }
    }
    result.linear_velocity = best_linear;
    result.angular_velocity = best_angular;
    return result;
}

inline VelocityPair ComputeVelocity(
    double current_linear_velocity, double current_angular_velocity,
    double max_linear_velocity, double min_linear_velocity,
    double max_angular_velocity, double min_angular_velocity,
    double max_linear_acceleration, double max_linear_deceleration,
    double max_angular_acceleration, double max_angular_deceleration,
    double regulated_linear_velocity, double curvature, double direction,
    double dt) {
    DynamicWindowBounds window = ComputeDynamicWindow(
        current_linear_velocity, current_angular_velocity, max_linear_velocity,
        min_linear_velocity, max_angular_velocity, min_angular_velocity,
        max_linear_acceleration, max_linear_deceleration,
        max_angular_acceleration, max_angular_deceleration, dt);
    ApplyRegulatedLinearVelocity(regulated_linear_velocity, window);
    return ComputeOptimalVelocityWithinWindow(window, curvature, direction);
}

}  // namespace velaros_navigation::dynamic_window_pure_pursuit
