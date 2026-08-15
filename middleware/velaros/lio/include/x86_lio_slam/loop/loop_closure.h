#pragma once

#include "x86_lio_slam/common/config.h"
#include "x86_lio_slam/common/types.h"
#include "x86_lio_slam/keyframe/keyframe.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace x86_lio_slam {

struct LoopCandidate {
    std::uint32_t query_id = 0;
    std::uint32_t target_id = 0;
    double score = 0.0;
};

struct LoopRegistrationResult {
    bool converged = false;
    Pose3d T_target_query{};
    double fitness = 0.0;
    std::size_t inliers = 0;
};

class ILoopDetector {
public:
    virtual ~ILoopDetector() = default;

    virtual void Reset() {}

    virtual std::vector<LoopCandidate> Detect(const Keyframe& query) = 0;
};

class ILoopRegistration {
public:
    virtual ~ILoopRegistration() = default;

    virtual LoopRegistrationResult Register(const Keyframe& query,
                                            const Keyframe& target) = 0;
};

class RadiusLoopDetector final : public ILoopDetector {
public:
    explicit RadiusLoopDetector(LoopConfig config = LoopConfig{})
        : config_(config) {}

    void Reset() override;

    std::vector<LoopCandidate> Detect(const Keyframe& query) override;

private:
    LoopConfig config_;
    std::vector<Keyframe> history_;
};

class CentroidLoopRegistration final : public ILoopRegistration {
public:
    explicit CentroidLoopRegistration(double correspondence_radius = 2.0,
                                      int iterations = 5)
        : correspondence_radius_(correspondence_radius),
          iterations_(iterations) {}

    LoopRegistrationResult Register(const Keyframe& query,
                                    const Keyframe& target) override;

private:
    double correspondence_radius_;
    int iterations_;
};

}  // namespace x86_lio_slam
