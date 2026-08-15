#pragma once

#include "x86_lio_slam/common/config.h"

#include <Eigen/Core>

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>

namespace x86_lio_slam {

// Point-to-plane inputs are kept in structure-of-arrays form so SIMD backends
// can load eight coordinates without gathering from Eigen's AoS vectors.
struct PointPlaneBatch {
    std::span<const float> body_x;
    std::span<const float> body_y;
    std::span<const float> body_z;
    std::span<const float> normal_x;
    std::span<const float> normal_y;
    std::span<const float> normal_z;
    std::span<const float> offsets;
    std::span<const float> ranges;
    std::span<const std::uint8_t> valid;
};

struct PointPlaneResidualOutput {
    std::span<float> residuals;
    // Six component-major arrays, each with one entry per input point.
    std::span<float> jacobians;
    std::span<std::uint8_t> accepted;
};

class IPointKernelBackend {
public:
    virtual ~IPointKernelBackend() = default;

    virtual void TransformPoints(
        std::span<const Eigen::Vector3f> input,
        const Eigen::Matrix3f& rotation,
        const Eigen::Vector3f& translation,
        std::span<Eigen::Vector3f> output) const = 0;

    virtual void ComputeSquaredDistances(
        std::span<const Eigen::Vector3f> first,
        std::span<const Eigen::Vector3f> second,
        std::span<float> output) const = 0;

    // The default implementation is scalar and is also used by backends that
    // do not provide a specialized point-to-plane implementation.
    virtual void ComputePointPlaneResiduals(
        const PointPlaneBatch& batch, const Eigen::Matrix3f& rotation,
        const Eigen::Vector3f& translation, float match_squared,
        PointPlaneResidualOutput output) const;
};

class ScalarPointKernelBackend final : public IPointKernelBackend {
public:
    void TransformPoints(std::span<const Eigen::Vector3f> input,
                         const Eigen::Matrix3f& rotation,
                         const Eigen::Vector3f& translation,
                         std::span<Eigen::Vector3f> output) const override;

    void ComputeSquaredDistances(std::span<const Eigen::Vector3f> first,
                                 std::span<const Eigen::Vector3f> second,
                                 std::span<float> output) const override;
};

class EigenPointKernelBackend final : public IPointKernelBackend {
public:
    void TransformPoints(std::span<const Eigen::Vector3f> input,
                         const Eigen::Matrix3f& rotation,
                         const Eigen::Vector3f& translation,
                         std::span<Eigen::Vector3f> output) const override;

    void ComputeSquaredDistances(std::span<const Eigen::Vector3f> first,
                                 std::span<const Eigen::Vector3f> second,
                                 std::span<float> output) const override;
};

class X86SimdPointKernelBackend final : public IPointKernelBackend {
public:
    void TransformPoints(std::span<const Eigen::Vector3f> input,
                         const Eigen::Matrix3f& rotation,
                         const Eigen::Vector3f& translation,
                         std::span<Eigen::Vector3f> output) const override;

    void ComputeSquaredDistances(std::span<const Eigen::Vector3f> first,
                                 std::span<const Eigen::Vector3f> second,
                                 std::span<float> output) const override;

    void ComputePointPlaneResiduals(
        const PointPlaneBatch& batch, const Eigen::Matrix3f& rotation,
        const Eigen::Vector3f& translation, float match_squared,
        PointPlaneResidualOutput output) const override;
};

class RvvPointKernelBackend final : public IPointKernelBackend {
public:
    void TransformPoints(std::span<const Eigen::Vector3f> input,
                         const Eigen::Matrix3f& rotation,
                         const Eigen::Vector3f& translation,
                         std::span<Eigen::Vector3f> output) const override;

    void ComputeSquaredDistances(std::span<const Eigen::Vector3f> first,
                                 std::span<const Eigen::Vector3f> second,
                                 std::span<float> output) const override;

    void ComputePointPlaneResiduals(
        const PointPlaneBatch& batch, const Eigen::Matrix3f& rotation,
        const Eigen::Vector3f& translation, float match_squared,
        PointPlaneResidualOutput output) const override;
};

// These report both build support and, for AVX2, runtime CPU support.
bool IsX86SimdPointKernelAvailable();
bool IsRvvPointKernelAvailable();

std::unique_ptr<IPointKernelBackend> CreatePointKernelBackend(
    PointKernelBackend backend);

}  // namespace x86_lio_slam
