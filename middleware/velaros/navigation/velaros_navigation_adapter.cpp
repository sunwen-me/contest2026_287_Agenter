#include "velaros/navigation/velaros_navigation_adapter.h"

#include <algorithm>
#include <cmath>

namespace velaros_navigation::velaros {

namespace {

double MicrosecondsToSeconds(std::uint64_t timestamp_us) {
    return static_cast<double>(timestamp_us) * 1e-6;
}

float Clamp(float value, float lower, float upper) {
    return std::max(lower, std::min(value, upper));
}

}  // namespace

VelaRosNavigationAdapter::VelaRosNavigationAdapter(
    NavigationPipeline& pipeline, MotionCommandSink sink, void* user_data,
    VelaRosNavigationAdapterConfig config)
    : pipeline_(&pipeline), sink_(sink), user_data_(user_data),
      config_(config) {}

bool VelaRosNavigationAdapter::ReceiveGoal(
    const VelaRosGoalMessage& message, double now) {
    if (pipeline_ == nullptr || !std::isfinite(message.x_m) ||
        !std::isfinite(message.y_m) || !std::isfinite(message.yaw_rad)) {
        return false;
    }
    NavigationGoal goal;
    goal.timestamp = MicrosecondsToSeconds(message.timestamp_us);
    goal.position = Vector2d(message.x_m, message.y_m);
    goal.heading = message.yaw_rad;
    goal.has_heading = message.has_yaw != 0U;
    goal.valid = true;
    if (message.frame[0] != '\0') {
        if (!goal.frame.SetFixed(message.frame, sizeof(message.frame))) {
            return false;
        }
    } else {
        goal.frame = config_.map_frame;
    }
    if (!goal.frame.Valid()) {
        return false;
    }
    return pipeline_->SetGoal(goal, now);
}

bool VelaRosNavigationAdapter::EncodeAndPublish(
    const VelocityCommand& command, std::uint64_t now_us) {
    VelaRosMotionCommand encoded;
    encoded.timestamp_us = now_us;
    encoded.timeout_ms = config_.command_timeout_ms;
    encoded.sequence = ++sequence_;
    encoded.valid = command.valid ? 1U : 0U;
    encoded.stop = command.stop ? 1U : 0U;
    if (command.valid && !command.stop) {
        encoded.linear_x_mps = Clamp(
            static_cast<float>(command.planar_velocity.x()),
            -std::abs(config_.max_linear_speed),
            std::abs(config_.max_linear_speed));
        encoded.linear_y_mps = Clamp(
            static_cast<float>(command.planar_velocity.y()),
            -std::abs(config_.max_linear_speed),
            std::abs(config_.max_linear_speed));
        encoded.angular_z_rps = Clamp(
            static_cast<float>(command.yaw_rate),
            -std::abs(config_.max_angular_speed),
            std::abs(config_.max_angular_speed));
        if (std::abs(encoded.linear_y_mps) > 1e-5F) {
            encoded.linear_x_mps = 0.0F;
            encoded.angular_z_rps = 0.0F;
            encoded.linear_y_mps = 0.0F;
            encoded.valid = 0U;
            encoded.stop = 1U;
        }
    }
    last_command_ = encoded;
    return sink_ == nullptr || sink_(last_command_, user_data_);
}

bool VelaRosNavigationAdapter::Tick(std::uint64_t now_us) {
    const double now = MicrosecondsToSeconds(now_us);
    VelocityCommand command;
    if (pipeline_ == nullptr) {
        command.status = ControlStatus::InvalidMap;
        command.stop = true;
        command.valid = true;
    } else {
        (void)pipeline_->Tick(now, command);
    }
    return EncodeAndPublish(command, now_us);
}

void VelaRosNavigationAdapter::SetEmergencyStop(bool enabled) {
    if (pipeline_ != nullptr) {
        pipeline_->SetEmergencyStop(enabled);
    }
}

}  // namespace velaros_navigation::velaros
