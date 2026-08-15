#include "x86_lio_slam/kernels/point_kernel.h"

#include <algorithm>
#include <stdexcept>

namespace x86_lio_slam {

namespace {

void CheckTransformSpans(std::span<const Eigen::Vector3f> input,
                         std::span<Eigen::Vector3f> output) {
    if (output.size() < input.size()) {
        throw std::invalid_argument("point transform output span is too small");
    }
}

void CheckDistanceSpans(std::span<const Eigen::Vector3f> first,
                        std::span<const Eigen::Vector3f> second,
                        std::span<float> output) {
    if (first.size() != second.size() || output.size() < first.size()) {
        throw std::invalid_argument("point distance spans have incompatible sizes");
    }
}

std::size_t CheckPointPlaneSpans(const PointPlaneBatch& batch,
                                 const PointPlaneResidualOutput& output) {
    const std::size_t count = batch.body_x.size();
    const bool input_sizes_match =
        batch.body_y.size() == count && batch.body_z.size() == count &&
        batch.normal_x.size() == count && batch.normal_y.size() == count &&
        batch.normal_z.size() == count && batch.offsets.size() == count &&
        batch.ranges.size() == count && batch.valid.size() == count;
    if (!input_sizes_match || output.residuals.size() < count ||
        output.accepted.size() < count || count > output.jacobians.size() / 6U) {
        throw std::invalid_argument(
            "point-plane batch spans have incompatible sizes");
    }
    return count;
}

void ZeroPointPlaneOutput(const PointPlaneResidualOutput& output,
                          std::size_t count, std::size_t index) {
    output.residuals[index] = 0.0F;
    output.accepted[index] = 0;
    for (std::size_t component = 0; component < 6; ++component) {
        output.jacobians[component * count + index] = 0.0F;
    }
}

}  // namespace

void ScalarPointKernelBackend::TransformPoints(
    std::span<const Eigen::Vector3f> input, const Eigen::Matrix3f& rotation,
    const Eigen::Vector3f& translation,
    std::span<Eigen::Vector3f> output) const {
    CheckTransformSpans(input, output);
    for (std::size_t index = 0; index < input.size(); ++index) {
        output[index] = rotation * input[index] + translation;
    }
}

void ScalarPointKernelBackend::ComputeSquaredDistances(
    std::span<const Eigen::Vector3f> first,
    std::span<const Eigen::Vector3f> second,
    std::span<float> output) const {
    CheckDistanceSpans(first, second, output);
    for (std::size_t index = 0; index < first.size(); ++index) {
        output[index] = (first[index] - second[index]).squaredNorm();
    }
}

void EigenPointKernelBackend::TransformPoints(
    std::span<const Eigen::Vector3f> input, const Eigen::Matrix3f& rotation,
    const Eigen::Vector3f& translation,
    std::span<Eigen::Vector3f> output) const {
    CheckTransformSpans(input, output);
    for (std::size_t index = 0; index < input.size(); ++index) {
        output[index].noalias() = rotation * input[index] + translation;
    }
}

void EigenPointKernelBackend::ComputeSquaredDistances(
    std::span<const Eigen::Vector3f> first,
    std::span<const Eigen::Vector3f> second,
    std::span<float> output) const {
    CheckDistanceSpans(first, second, output);
    for (std::size_t index = 0; index < first.size(); ++index) {
        const Eigen::Vector3f difference = first[index] - second[index];
        output[index] = difference.dot(difference);
    }
}

void IPointKernelBackend::ComputePointPlaneResiduals(
    const PointPlaneBatch& batch, const Eigen::Matrix3f& rotation,
    const Eigen::Vector3f& translation, float match_squared,
    PointPlaneResidualOutput output) const {
    const std::size_t count = CheckPointPlaneSpans(batch, output);
    for (std::size_t index = 0; index < count; ++index) {
        if (batch.valid[index] == 0) {
            ZeroPointPlaneOutput(output, count, index);
            continue;
        }

        const float body_x = batch.body_x[index];
        const float body_y = batch.body_y[index];
        const float body_z = batch.body_z[index];
        const float world_x = rotation(0, 0) * body_x +
                              rotation(0, 1) * body_y +
                              rotation(0, 2) * body_z + translation.x();
        const float world_y = rotation(1, 0) * body_x +
                              rotation(1, 1) * body_y +
                              rotation(1, 2) * body_z + translation.y();
        const float world_z = rotation(2, 0) * body_x +
                              rotation(2, 1) * body_y +
                              rotation(2, 2) * body_z + translation.z();

        const float normal_x = batch.normal_x[index];
        const float normal_y = batch.normal_y[index];
        const float normal_z = batch.normal_z[index];
        const float residual = normal_x * world_x + normal_y * world_y +
                               normal_z * world_z + batch.offsets[index];
        if (!(batch.ranges[index] > match_squared * residual * residual)) {
            ZeroPointPlaneOutput(output, count, index);
            continue;
        }

        const float normal_body_x = rotation(0, 0) * normal_x +
                                    rotation(1, 0) * normal_y +
                                    rotation(2, 0) * normal_z;
        const float normal_body_y = rotation(0, 1) * normal_x +
                                    rotation(1, 1) * normal_y +
                                    rotation(2, 1) * normal_z;
        const float normal_body_z = rotation(0, 2) * normal_x +
                                    rotation(1, 2) * normal_y +
                                    rotation(2, 2) * normal_z;
        output.residuals[index] = residual;
        output.accepted[index] = 1;
        output.jacobians[index] = body_y * normal_body_z -
                                  body_z * normal_body_y;
        output.jacobians[count + index] = body_z * normal_body_x -
                                          body_x * normal_body_z;
        output.jacobians[2 * count + index] = body_x * normal_body_y -
                                              body_y * normal_body_x;
        output.jacobians[3 * count + index] = normal_x;
        output.jacobians[4 * count + index] = normal_y;
        output.jacobians[5 * count + index] = normal_z;
    }
}

#if !defined(X86_LIO_SLAM_HAS_X86_AVX2)
void X86SimdPointKernelBackend::TransformPoints(
    std::span<const Eigen::Vector3f>, const Eigen::Matrix3f&,
    const Eigen::Vector3f&, std::span<Eigen::Vector3f>) const {
    throw std::invalid_argument(
        "X86SimdPointKernelBackend is unavailable in this build");
}

void X86SimdPointKernelBackend::ComputeSquaredDistances(
    std::span<const Eigen::Vector3f>, std::span<const Eigen::Vector3f>,
    std::span<float>) const {
    throw std::invalid_argument(
        "X86SimdPointKernelBackend is unavailable in this build");
}

void X86SimdPointKernelBackend::ComputePointPlaneResiduals(
    const PointPlaneBatch&, const Eigen::Matrix3f&, const Eigen::Vector3f&,
    float, PointPlaneResidualOutput) const {
    throw std::invalid_argument(
        "X86SimdPointKernelBackend is unavailable in this build");
}
#endif

#if !defined(X86_LIO_SLAM_HAS_RVV)
void RvvPointKernelBackend::TransformPoints(
    std::span<const Eigen::Vector3f>, const Eigen::Matrix3f&,
    const Eigen::Vector3f&, std::span<Eigen::Vector3f>) const {
    throw std::invalid_argument("RvvPointKernelBackend is unavailable in this build");
}

void RvvPointKernelBackend::ComputeSquaredDistances(
    std::span<const Eigen::Vector3f>, std::span<const Eigen::Vector3f>,
    std::span<float>) const {
    throw std::invalid_argument("RvvPointKernelBackend is unavailable in this build");
}

void RvvPointKernelBackend::ComputePointPlaneResiduals(
    const PointPlaneBatch&, const Eigen::Matrix3f&, const Eigen::Vector3f&,
    float, PointPlaneResidualOutput) const {
    throw std::invalid_argument("RvvPointKernelBackend is unavailable in this build");
}
#endif

std::unique_ptr<IPointKernelBackend> CreatePointKernelBackend(
    PointKernelBackend backend) {
    switch (backend) {
    case PointKernelBackend::Scalar:
        return std::make_unique<ScalarPointKernelBackend>();
    case PointKernelBackend::Eigen:
        return std::make_unique<EigenPointKernelBackend>();
    case PointKernelBackend::X86Simd:
        if (!IsX86SimdPointKernelAvailable()) {
            throw std::invalid_argument(
                "X86SimdPointKernelBackend requires an AVX2 build and CPU");
        }
        return std::make_unique<X86SimdPointKernelBackend>();
    case PointKernelBackend::Rvv:
        if (!IsRvvPointKernelAvailable()) {
            throw std::invalid_argument(
                "RvvPointKernelBackend requires a RISC-V Vector build");
        }
        return std::make_unique<RvvPointKernelBackend>();
    }
    throw std::invalid_argument("unknown point kernel backend");
}

bool IsX86SimdPointKernelAvailable() {
#if defined(X86_LIO_SLAM_HAS_X86_AVX2)
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("avx2") != 0;
#else
    return true;
#endif
#else
    return false;
#endif
}

bool IsRvvPointKernelAvailable() {
#if defined(X86_LIO_SLAM_HAS_RVV)
    return true;
#else
    return false;
#endif
}

}  // namespace x86_lio_slam
