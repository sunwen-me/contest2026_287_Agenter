#include "velaros/navigation/uorb_sink.h"

#include <cmath>

namespace velaros_navigation::velaros {

bool UorbMotionCommandSink::Init() {
    if (Initialized()) {
        return true;
    }
    int instance = 0;
    last_command_ = velaros_motion_command_s{};
    advertisement_fd_ = orb_advertise_multi_queue_persist(
        ORB_ID(velaros_motion_command), &last_command_, &instance, 1);
    return Initialized();
}

void UorbMotionCommandSink::Fini() {
    if (advertisement_fd_ >= 0) {
        (void)orb_unadvertise(advertisement_fd_);
        advertisement_fd_ = -1;
    }
    last_command_ = velaros_motion_command_s{};
}

bool UorbMotionCommandSink::Publish(const VelaRosMotionCommand& command) {
    if (!Initialized() || !std::isfinite(command.linear_x_mps) ||
        !std::isfinite(command.linear_y_mps) ||
        !std::isfinite(command.angular_z_rps)) {
        return false;
    }

    velaros_motion_command_s encoded{};
    encoded.timestamp = command.timestamp_us;
    encoded.timeout_ms = command.timeout_ms;
    encoded.sequence = command.sequence;
    if (command.valid && !command.stop &&
        std::fabs(command.linear_y_mps) <= 1e-5F) {
        encoded.linear_x_mps = command.linear_x_mps;
        encoded.angular_z_rps = command.angular_z_rps;
    }

    if (orb_publish(ORB_ID(velaros_motion_command), advertisement_fd_,
                    &encoded) < 0) {
        return false;
    }
    last_command_ = encoded;
    return true;
}

bool UorbMotionCommandSink::Sink(const VelaRosMotionCommand& command,
                                 void* user_data) {
    if (user_data == nullptr) {
        return false;
    }
    return static_cast<UorbMotionCommandSink*>(user_data)->Publish(command);
}

}  // namespace velaros_navigation::velaros
