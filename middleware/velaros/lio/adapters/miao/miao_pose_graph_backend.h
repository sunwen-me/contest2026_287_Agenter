#pragma once

#include "x86_lio_slam/backend/pose_graph_backend.h"

#include <memory>

namespace x86_lio_slam {

class MiaoPoseGraphBackend final : public IPoseGraphBackend {
public:
    MiaoPoseGraphBackend();
    ~MiaoPoseGraphBackend() override;

    MiaoPoseGraphBackend(const MiaoPoseGraphBackend&) = delete;
    MiaoPoseGraphBackend& operator=(const MiaoPoseGraphBackend&) = delete;

    void Reset() override;

    void AddKeyframe(const Keyframe& keyframe) override;

    void AddOdometryFactor(
        std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
        const Eigen::Matrix<double, 6, 6>& information) override;

    void AddDeadReckoningFactor(
        std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
        const Eigen::Matrix<double, 6, 6>& information) override;

    void AddLocalizationPrior(
        std::uint32_t id, const Pose3d& measurement,
        const Eigen::Matrix<double, 6, 6>& information) override;

    void AddLoopFactor(
        std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
        const Eigen::Matrix<double, 6, 6>& information) override;

    bool Optimize() override;

    Pose3d GetOptimizedPose(std::uint32_t id) const override;

private:
#if defined(X86_LIO_SLAM_WITH_MIAO)
    struct Impl;
    std::unique_ptr<Impl> impl_;
#else
    DensePoseGraphBackend fallback_;
#endif
};

}  // namespace x86_lio_slam
