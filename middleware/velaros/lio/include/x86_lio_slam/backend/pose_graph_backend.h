#pragma once

#include "x86_lio_slam/common/types.h"
#include "x86_lio_slam/keyframe/keyframe.h"

#include <Eigen/Core>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace x86_lio_slam {

class IPoseGraphBackend {
public:
    virtual ~IPoseGraphBackend() = default;

    virtual void Reset() = 0;

    virtual void AddKeyframe(const Keyframe& keyframe) = 0;

    virtual void AddOdometryFactor(
        std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
        const Eigen::Matrix<double, 6, 6>& information) = 0;

    // DR is kept as a separate source at the fusion boundary. Backends that
    // do not need source-specific robust kernels may use the same relative
    // edge implementation as lidar odometry.
    virtual void AddDeadReckoningFactor(
        std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
        const Eigen::Matrix<double, 6, 6>& information) {
        AddOdometryFactor(from, to, measurement, information);
    }

    // Adds an absolute pose observation, matching Lightning-LM's LidarLoc
    // factor. Backends that do not receive a localization stream may ignore
    // this until one is connected.
    virtual void AddLocalizationPrior(
        std::uint32_t id, const Pose3d& measurement,
        const Eigen::Matrix<double, 6, 6>& information) = 0;

    virtual void AddLoopFactor(
        std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
        const Eigen::Matrix<double, 6, 6>& information) = 0;

    virtual bool Optimize() = 0;

    virtual Pose3d GetOptimizedPose(std::uint32_t id) const = 0;
};

// Uses the configured external optimizer when one is enabled at build time;
// otherwise returns the standalone dense fallback.
std::unique_ptr<IPoseGraphBackend> CreateDefaultPoseGraphBackend();

class DensePoseGraphBackend final : public IPoseGraphBackend {
public:
    explicit DensePoseGraphBackend(int iterations = 15)
        : iterations_(iterations) {}

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

    double Cost() const;

    std::size_t NodeCount() const { return nodes_.size(); }

    std::size_t FactorCount() const { return factors_.size(); }

private:
    enum class Kernel { None, Huber, Cauchy };

    enum class FactorType { Relative, Prior };

    struct Node {
        std::uint32_t id = 0;
        Pose3d pose{};
        bool fixed = false;
    };

    struct Factor {
        std::uint32_t from = 0;
        std::uint32_t to = 0;
        Pose3d measurement{};
        Eigen::Matrix<double, 6, 6> information =
            Eigen::Matrix<double, 6, 6>::Identity();
        Kernel kernel = Kernel::None;
        FactorType type = FactorType::Relative;
    };

    using NodeIndex = std::unordered_map<std::uint32_t, std::size_t>;

    double RobustWeight(double chi_squared, Kernel kernel) const;

    double RobustCost(double chi_squared, Kernel kernel) const;

    Eigen::Matrix<double, 6, 1> Residual(const Factor& factor) const;

    bool IsSequentialOdometryFactor(const Factor& factor) const;

    bool ApplyPendingOdometryFastPath();

    Node& NodeFor(std::uint32_t id);

    const Node& NodeFor(std::uint32_t id) const;

    std::vector<Node> nodes_;
    NodeIndex node_index_;
    std::vector<Factor> factors_;
    int iterations_ = 15;
    double last_cost_ = 0.0;
    std::size_t optimized_factor_count_ = 0;
    std::size_t loop_factor_count_ = 0;
    std::size_t optimized_loop_factor_count_ = 0;
    bool odometry_chain_ = true;
};

}  // namespace x86_lio_slam
