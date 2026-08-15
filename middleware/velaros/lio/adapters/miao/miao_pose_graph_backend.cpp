#include "miao_pose_graph_backend.h"

#include <cmath>
#include <deque>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(X86_LIO_SLAM_WITH_MIAO)

#include "common/eigen_types.h"
#include "common/std_types.h"
#include "core/common/config.h"
#include "core/graph/optimizer.h"
#include "core/opti_algo/algo_select.h"
#include "core/robust_kernel/robust_kernel_all.h"
#include "core/types/edge_se3.h"
#include "core/types/edge_se3_prior.h"
#include "core/types/vertex_se3.h"

namespace x86_lio_slam {

namespace {

lightning::SE3 ToMiaoPose(const Pose3d& pose) {
    // The core pose is propagated with matrix products. Rebuild through a
    // normalized quaternion before crossing into Sophus, whose matrix
    // constructor checks orthogonality strictly.
    Eigen::Quaterniond quaternion(pose.rotation);
    quaternion.normalize();
    return lightning::SE3(lightning::SO3(quaternion), pose.translation);
}

Pose3d FromMiaoPose(const lightning::SE3& pose) {
    Pose3d result;
    result.rotation = pose.so3().matrix();
    result.translation = pose.translation();
    return result;
}

}  // namespace

struct MiaoPoseGraphBackend::Impl {
    using Optimizer = lightning::miao::Optimizer;
    using Vertex = lightning::miao::VertexSE3;

    static constexpr std::size_t kWindowSize = 5;
    static constexpr double kLocalizationPriorDelta = 30.0;

    enum class FactorType { Relative, LocalizationPrior };

    struct FactorRecord {
        std::uint32_t from = 0;
        std::uint32_t to = 0;
        Pose3d measurement{};
        Eigen::Matrix<double, 6, 6> information =
            Eigen::Matrix<double, 6, 6>::Identity();
        bool loop = false;
        FactorType type = FactorType::Relative;
    };

    std::shared_ptr<Optimizer> optimizer;

    // The key is the external keyframe ID. The vertex's miao ID is allowed to
    // change when incremental mode replaces the oldest slot.
    std::unordered_map<std::uint32_t, std::shared_ptr<Vertex>> active_vertices;
    std::deque<std::uint32_t> active_ids;
    std::vector<std::uint32_t> keyframe_order;
    std::unordered_map<std::uint32_t, Pose3d> pose_cache;
    std::vector<FactorRecord> factors;

    Eigen::Matrix<double, 6, 6> marginal_prior_information =
        Eigen::Matrix<double, 6, 6>::Identity() * 100.0;
    bool incremental_pending = false;
    bool global_optimization_pending = false;

    Impl() : optimizer(MakeOptimizer(true)) {}

    static std::shared_ptr<Optimizer> MakeOptimizer(bool incremental) {
        lightning::miao::OptimizerConfig config(
            lightning::miao::AlgorithmType::LEVENBERG_MARQUARDT,
            lightning::miao::LinearSolverType::LINEAR_SOLVER_SPARSE_EIGEN,
            false);
        config.incremental_mode_ = incremental;
        config.max_vertex_size_ =
            incremental ? static_cast<int>(kWindowSize) : -1;
        // The active PGO window has only five pose blocks. Parallel STL/TBB
        // dispatch costs more than the solve at this size; keep full graph
        // loop optimization parallel.
        config.parallel_ = !incremental;
        auto result = lightning::miao::SetupOptimizer<6, 3>(config);
        result->SetVerbose(false);
        return result;
    }

    static Eigen::Matrix<double, 6, 6> SymmetrizeInformation(
        const Eigen::Matrix<double, 6, 6>& information) {
        return 0.5 * (information + information.transpose());
    }

    bool IsActive(std::uint32_t id) const {
        return active_vertices.contains(id);
    }

    void AddRelativeEdge(
        const std::shared_ptr<Optimizer>& target,
        const std::unordered_map<std::uint32_t, std::shared_ptr<Vertex>>& vertices,
        const FactorRecord& factor) const;

    void AddLocalizationEdge(
        const std::shared_ptr<Optimizer>& target,
        const std::unordered_map<std::uint32_t, std::shared_ptr<Vertex>>& vertices,
        const FactorRecord& factor) const;

    void AddMarginalizationPrior(std::uint32_t id, const Pose3d& pose);

    void AddKeyframe(const Keyframe& keyframe);

    void AddRelativeFactor(std::uint32_t from, std::uint32_t to,
                           const Pose3d& measurement,
                           const Eigen::Matrix<double, 6, 6>& information,
                           bool loop);

    void AddLocalizationPrior(
        std::uint32_t id, const Pose3d& measurement,
        const Eigen::Matrix<double, 6, 6>& information);

    bool OptimizeIncremental();

    bool OptimizeGlobal();

    bool RebuildIncrementalWindow();
};

void MiaoPoseGraphBackend::Impl::AddRelativeEdge(
    const std::shared_ptr<Optimizer>& target,
    const std::unordered_map<std::uint32_t, std::shared_ptr<Vertex>>& vertices,
    const FactorRecord& factor) const {
    const auto from = vertices.find(factor.from);
    const auto to = vertices.find(factor.to);
    if (from == vertices.end() || to == vertices.end()) {
        throw std::out_of_range(
            "miao relative factor references an unknown vertex");
    }

    auto edge = std::make_shared<lightning::miao::EdgeSE3>();
    edge->SetVertex(0, from->second);
    edge->SetVertex(1, to->second);
    edge->SetMeasurement(ToMiaoPose(factor.measurement));
    edge->SetInformation(factor.information);
    if (factor.loop) {
        edge->SetRobustKernel(
            std::make_shared<lightning::miao::RobustKernelCauchy>());
    }
    if (!target->AddEdge(edge)) {
        throw std::runtime_error("miao rejected pose graph factor");
    }
}

void MiaoPoseGraphBackend::Impl::AddLocalizationEdge(
    const std::shared_ptr<Optimizer>& target,
    const std::unordered_map<std::uint32_t, std::shared_ptr<Vertex>>& vertices,
    const FactorRecord& factor) const {
    const auto found = vertices.find(factor.from);
    if (found == vertices.end()) {
        throw std::out_of_range(
            "miao localization prior references an unknown vertex");
    }

    auto edge = std::make_shared<lightning::miao::EdgeSE3Prior>();
    edge->SetVertex(0, found->second);
    edge->SetMeasurement(ToMiaoPose(factor.measurement));
    edge->SetInformation(factor.information);
    auto robust_kernel =
        std::make_shared<lightning::miao::RobustKernelHuber>();
    robust_kernel->SetDelta(kLocalizationPriorDelta);
    edge->SetRobustKernel(std::move(robust_kernel));
    if (!target->AddEdge(edge)) {
        throw std::runtime_error("miao rejected localization prior");
    }
}

void MiaoPoseGraphBackend::Impl::AddMarginalizationPrior(
    std::uint32_t id, const Pose3d& pose) {
    FactorRecord prior;
    prior.from = id;
    prior.measurement = pose;
    prior.information = marginal_prior_information;
    prior.type = FactorType::LocalizationPrior;
    AddLocalizationEdge(optimizer, active_vertices, prior);
    incremental_pending = true;
}

void MiaoPoseGraphBackend::Impl::AddKeyframe(const Keyframe& keyframe) {
    if (pose_cache.contains(keyframe.id)) {
        throw std::invalid_argument("duplicate miao keyframe id");
    }

    const bool replacing_oldest = active_ids.size() == kWindowSize;
    std::uint32_t prior_id = 0;
    Pose3d prior_pose = Pose3d::Identity();
    if (replacing_oldest) {
        prior_id = *(active_ids.begin() + 1);
        prior_pose = FromMiaoPose(active_vertices.at(prior_id)->Estimate());
    }

    auto vertex = std::make_shared<Vertex>();
    vertex->SetId(static_cast<int>(keyframe.id));
    vertex->SetEstimate(ToMiaoPose(keyframe.T_map_body));
    if (!optimizer->AddVertex(vertex)) {
        throw std::runtime_error("miao rejected pose graph vertex");
    }

    pose_cache.emplace(keyframe.id, keyframe.T_map_body);
    keyframe_order.push_back(keyframe.id);

    if (replacing_oldest) {
        active_vertices.erase(active_ids.front());
        active_ids.pop_front();
    }
    active_ids.push_back(keyframe.id);
    active_vertices.emplace(keyframe.id, std::move(vertex));

    if (replacing_oldest) {
        // The optimizer removes the old prior with the replaced vertex.
        // Re-anchor the surviving oldest frame before adding new factors.
        AddMarginalizationPrior(prior_id, prior_pose);
    } else if (active_ids.size() == 1) {
        AddMarginalizationPrior(keyframe.id, keyframe.T_map_body);
    }
    incremental_pending = true;
}

void MiaoPoseGraphBackend::Impl::AddRelativeFactor(
    std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
    const Eigen::Matrix<double, 6, 6>& information, bool loop) {
    if (!pose_cache.contains(from) || !pose_cache.contains(to)) {
        throw std::out_of_range("miao factor references an unknown keyframe");
    }

    FactorRecord factor;
    factor.from = from;
    factor.to = to;
    factor.measurement = measurement;
    factor.information = SymmetrizeInformation(information);
    factor.loop = loop;
    factor.type = FactorType::Relative;
    factors.push_back(factor);

    if (!loop && IsActive(from) && IsActive(to)) {
        AddRelativeEdge(optimizer, active_vertices, factor);
        incremental_pending = true;
        marginal_prior_information = factor.information;
        return;
    }

    // A loop or a non-window relative factor must be solved against the full
    // history; the active optimizer intentionally cannot reference evicted
    // vertices.
    global_optimization_pending = true;
    incremental_pending = true;
}

void MiaoPoseGraphBackend::Impl::AddLocalizationPrior(
    std::uint32_t id, const Pose3d& measurement,
    const Eigen::Matrix<double, 6, 6>& information) {
    if (!pose_cache.contains(id)) {
        throw std::out_of_range(
            "miao localization prior references an unknown keyframe");
    }

    FactorRecord factor;
    factor.from = id;
    factor.measurement = measurement;
    factor.information = SymmetrizeInformation(information);
    factor.type = FactorType::LocalizationPrior;
    factors.push_back(factor);

    if (IsActive(id)) {
        AddLocalizationEdge(optimizer, active_vertices, factor);
        incremental_pending = true;
    } else {
        global_optimization_pending = true;
        incremental_pending = true;
    }
}

bool MiaoPoseGraphBackend::Impl::OptimizeIncremental() {
    if (!incremental_pending) {
        return !pose_cache.empty();
    }
    if (active_vertices.empty() || !optimizer->InitializeOptimization()) {
        return false;
    }

    const int result = optimizer->Optimize(5);
    if (result < 0 || !std::isfinite(optimizer->ActiveRobustChi2())) {
        return false;
    }
    for (const auto id : active_ids) {
        pose_cache.at(id) = FromMiaoPose(active_vertices.at(id)->Estimate());
    }
    incremental_pending = false;
    return true;
}

bool MiaoPoseGraphBackend::Impl::RebuildIncrementalWindow() {
    optimizer = MakeOptimizer(true);
    active_vertices.clear();

    for (const auto id : active_ids) {
        auto vertex = std::make_shared<Vertex>();
        vertex->SetId(static_cast<int>(id));
        vertex->SetEstimate(ToMiaoPose(pose_cache.at(id)));
        if (!optimizer->AddVertex(vertex)) {
            return false;
        }
        active_vertices.emplace(id, std::move(vertex));
    }

    if (active_ids.empty()) {
        incremental_pending = false;
        return true;
    }

    AddMarginalizationPrior(active_ids.front(),
                            pose_cache.at(active_ids.front()));
    for (const FactorRecord& factor : factors) {
        if (factor.type == FactorType::LocalizationPrior) {
            if (IsActive(factor.from)) {
                AddLocalizationEdge(optimizer, active_vertices, factor);
            }
        } else if (IsActive(factor.from) && IsActive(factor.to)) {
            AddRelativeEdge(optimizer, active_vertices, factor);
        }
    }
    incremental_pending = true;
    return OptimizeIncremental();
}

bool MiaoPoseGraphBackend::Impl::OptimizeGlobal() {
    auto global_optimizer = MakeOptimizer(false);
    std::unordered_map<std::uint32_t, std::shared_ptr<Vertex>> global_vertices;
    global_vertices.reserve(keyframe_order.size());

    for (const auto id : keyframe_order) {
        auto vertex = std::make_shared<Vertex>();
        vertex->SetId(static_cast<int>(id));
        vertex->SetEstimate(ToMiaoPose(pose_cache.at(id)));
        if (id == keyframe_order.front()) {
            vertex->SetFixed(true);
        }
        if (!global_optimizer->AddVertex(vertex)) {
            return false;
        }
        global_vertices.emplace(id, std::move(vertex));
    }

    for (const FactorRecord& factor : factors) {
        if (factor.type == FactorType::LocalizationPrior) {
            AddLocalizationEdge(global_optimizer, global_vertices, factor);
        } else {
            AddRelativeEdge(global_optimizer, global_vertices, factor);
        }
    }

    if (keyframe_order.size() > 1) {
        if (!global_optimizer->InitializeOptimization()) {
            return false;
        }
        if (!global_optimizer->ActiveVertices().empty()) {
            const int result = global_optimizer->Optimize(20);
            if (result < 0 ||
                !std::isfinite(global_optimizer->ActiveRobustChi2())) {
                return false;
            }
        }
    }

    for (const auto id : keyframe_order) {
        pose_cache.at(id) = FromMiaoPose(global_vertices.at(id)->Estimate());
    }
    global_optimization_pending = false;
    return RebuildIncrementalWindow();
}

MiaoPoseGraphBackend::MiaoPoseGraphBackend()
    : impl_(std::make_unique<Impl>()) {}

MiaoPoseGraphBackend::~MiaoPoseGraphBackend() = default;

void MiaoPoseGraphBackend::Reset() { impl_ = std::make_unique<Impl>(); }

std::unique_ptr<IPoseGraphBackend> CreateDefaultPoseGraphBackend() {
    return std::make_unique<MiaoPoseGraphBackend>();
}

void MiaoPoseGraphBackend::AddKeyframe(const Keyframe& keyframe) {
    impl_->AddKeyframe(keyframe);
}

void MiaoPoseGraphBackend::AddOdometryFactor(
    std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
    const Eigen::Matrix<double, 6, 6>& information) {
    impl_->AddRelativeFactor(from, to, measurement, information, false);
}

void MiaoPoseGraphBackend::AddDeadReckoningFactor(
    std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
    const Eigen::Matrix<double, 6, 6>& information) {
    impl_->AddRelativeFactor(from, to, measurement, information, false);
}

void MiaoPoseGraphBackend::AddLocalizationPrior(
    std::uint32_t id, const Pose3d& measurement,
    const Eigen::Matrix<double, 6, 6>& information) {
    impl_->AddLocalizationPrior(id, measurement, information);
}

void MiaoPoseGraphBackend::AddLoopFactor(
    std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
    const Eigen::Matrix<double, 6, 6>& information) {
    impl_->AddRelativeFactor(from, to, measurement, information, true);
}

bool MiaoPoseGraphBackend::Optimize() {
    if (impl_->global_optimization_pending) {
        return impl_->OptimizeGlobal();
    }
    return impl_->OptimizeIncremental();
}

Pose3d MiaoPoseGraphBackend::GetOptimizedPose(std::uint32_t id) const {
    const auto found = impl_->pose_cache.find(id);
    if (found == impl_->pose_cache.end()) {
        throw std::out_of_range("miao pose graph vertex does not exist");
    }
    return found->second;
}

}  // namespace x86_lio_slam

#else

namespace x86_lio_slam {

MiaoPoseGraphBackend::MiaoPoseGraphBackend() = default;
MiaoPoseGraphBackend::~MiaoPoseGraphBackend() = default;

void MiaoPoseGraphBackend::Reset() { fallback_.Reset(); }

std::unique_ptr<IPoseGraphBackend> CreateDefaultPoseGraphBackend() {
    return std::make_unique<DensePoseGraphBackend>();
}

void MiaoPoseGraphBackend::AddKeyframe(const Keyframe& keyframe) {
    fallback_.AddKeyframe(keyframe);
}

void MiaoPoseGraphBackend::AddOdometryFactor(
    std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
    const Eigen::Matrix<double, 6, 6>& information) {
    fallback_.AddOdometryFactor(from, to, measurement, information);
}

void MiaoPoseGraphBackend::AddDeadReckoningFactor(
    std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
    const Eigen::Matrix<double, 6, 6>& information) {
    fallback_.AddDeadReckoningFactor(from, to, measurement, information);
}

void MiaoPoseGraphBackend::AddLocalizationPrior(
    std::uint32_t id, const Pose3d& measurement,
    const Eigen::Matrix<double, 6, 6>& information) {
    fallback_.AddLocalizationPrior(id, measurement, information);
}

void MiaoPoseGraphBackend::AddLoopFactor(
    std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
    const Eigen::Matrix<double, 6, 6>& information) {
    fallback_.AddLoopFactor(from, to, measurement, information);
}

bool MiaoPoseGraphBackend::Optimize() { return fallback_.Optimize(); }

Pose3d MiaoPoseGraphBackend::GetOptimizedPose(std::uint32_t id) const {
    return fallback_.GetOptimizedPose(id);
}

}  // namespace x86_lio_slam

#endif
