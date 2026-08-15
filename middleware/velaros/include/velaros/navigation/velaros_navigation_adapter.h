#pragma once

#include "velaros/navigation/navigation_pipeline.h"

#include <cstddef>
#include <cstdint>

namespace velaros_navigation::velaros {

// These fixed-layout PODs mirror the VelaROS mobile-base boundary. The
// adapter deliberately does not include rclcpp, geometry_msgs, or uORB;
// product glue can pass the POD to its generated message/uORB bridge.
struct VelaRosGoalMessage {
    std::uint64_t timestamp_us = 0;
    float x_m = 0.0F;
    float y_m = 0.0F;
    float yaw_rad = 0.0F;
    std::uint8_t has_yaw = 0;
    char frame[kNavigationFrameCapacity]{};
};

struct VelaRosMotionCommand {
    std::uint64_t timestamp_us = 0;
    float linear_x_mps = 0.0F;
    float linear_y_mps = 0.0F;
    float angular_z_rps = 0.0F;
    std::uint32_t timeout_ms = 250;
    std::uint32_t sequence = 0;
    std::uint8_t valid = 0;
    std::uint8_t stop = 1;
};

using MotionCommandSink = bool (*)(const VelaRosMotionCommand& command,
                                   void* user_data);

struct VelaRosNavigationAdapterConfig {
    FrameId map_frame{};
    float max_linear_speed = 0.8F;
    float max_angular_speed = 1.5F;
    std::uint32_t command_timeout_ms = 250;
};

class VelaRosNavigationAdapter final {
public:
    VelaRosNavigationAdapter(
        NavigationPipeline& pipeline, MotionCommandSink sink,
        void* user_data = nullptr,
        VelaRosNavigationAdapterConfig config =
            VelaRosNavigationAdapterConfig{});

    bool ReceiveGoal(const VelaRosGoalMessage& message, double now);

    bool Tick(std::uint64_t now_us);

    void SetEmergencyStop(bool enabled);

    const VelaRosMotionCommand& LastCommand() const { return last_command_; }

private:
    bool EncodeAndPublish(const VelocityCommand& command,
                          std::uint64_t now_us);

    NavigationPipeline* pipeline_ = nullptr;
    MotionCommandSink sink_ = nullptr;
    void* user_data_ = nullptr;
    VelaRosNavigationAdapterConfig config_{};
    VelaRosMotionCommand last_command_{};
    std::uint32_t sequence_ = 0;
};

using VelaRosAdapter = VelaRosNavigationAdapter;

}  // namespace velaros_navigation::velaros
