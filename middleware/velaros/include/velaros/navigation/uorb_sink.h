#pragma once

#include "velaros/navigation/velaros_navigation_adapter.h"
#include "velaros/uorb_topics.h"

namespace velaros_navigation::velaros {

// Target-only bridge from the bounded navigation command POD to the existing
// persistent VelaROS motion-command uORB topic.
class UorbMotionCommandSink final {
public:
    UorbMotionCommandSink() = default;

    UorbMotionCommandSink(const UorbMotionCommandSink&) = delete;
    UorbMotionCommandSink& operator=(const UorbMotionCommandSink&) = delete;

    bool Init();
    void Fini();
    bool Publish(const VelaRosMotionCommand& command);

    bool Initialized() const { return advertisement_fd_ >= 0; }

    const velaros_motion_command_s& LastCommand() const {
        return last_command_;
    }

    static bool Sink(const VelaRosMotionCommand& command, void* user_data);

private:
    int advertisement_fd_ = -1;
    velaros_motion_command_s last_command_{};
};

}  // namespace velaros_navigation::velaros
