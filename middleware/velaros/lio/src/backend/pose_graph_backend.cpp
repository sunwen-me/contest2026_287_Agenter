#include "x86_lio_slam/backend/pose_graph_backend.h"

#include "x86_lio_slam/common/geometry.h"

#if defined(X86_LIO_SLAM_WITH_BLOCK_PCG)
#include "x86_lio_slam/backend/block_pcg_solver.h"
#endif

#include <Eigen/Cholesky>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace x86_lio_slam {

namespace {

constexpr double kHuberDelta = 1.0;
constexpr double kCauchyDelta = 1.0;
constexpr double kJacobianStep = 1e-6;

Eigen::Matrix<double, 6, 6> SymmetrizeInformation(
    const Eigen::Matrix<double, 6, 6>& information) {
    return 0.5 * (information + information.transpose());
}

Eigen::Matrix<double, 6, 1> PriorResidual(const Pose3d& measurement,
                                          const Pose3d& pose) {
    Eigen::Matrix<double, 6, 1> residual;
    residual.head<3>() = pose.translation - measurement.translation;
    residual.tail<3>() = LogSO3(pose.rotation.transpose() *
                                measurement.rotation);
    return residual;
}

}  // namespace

void DensePoseGraphBackend::Reset() {
    nodes_.clear();
    node_index_.clear();
    factors_.clear();
    last_cost_ = 0.0;
    optimized_factor_count_ = 0;
    loop_factor_count_ = 0;
    optimized_loop_factor_count_ = 0;
    odometry_chain_ = true;
}

void DensePoseGraphBackend::AddKeyframe(const Keyframe& keyframe) {
    if (node_index_.contains(keyframe.id)) {
        throw std::invalid_argument("duplicate keyframe id in pose graph");
    }
    Node node;
    node.id = keyframe.id;
    node.pose = keyframe.T_map_body;
    node.fixed = nodes_.empty();
    node_index_.emplace(node.id, nodes_.size());
    nodes_.push_back(node);
}

DensePoseGraphBackend::Node& DensePoseGraphBackend::NodeFor(
    std::uint32_t id) {
    const auto found = node_index_.find(id);
    if (found == node_index_.end()) {
        throw std::out_of_range("pose graph node does not exist");
    }
    return nodes_[found->second];
}

const DensePoseGraphBackend::Node& DensePoseGraphBackend::NodeFor(
    std::uint32_t id) const {
    const auto found = node_index_.find(id);
    if (found == node_index_.end()) {
        throw std::out_of_range("pose graph node does not exist");
    }
    return nodes_[found->second];
}

void DensePoseGraphBackend::AddOdometryFactor(
    std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
    const Eigen::Matrix<double, 6, 6>& information) {
    (void)NodeFor(from);
    (void)NodeFor(to);
    const auto from_position = node_index_.at(from);
    const auto to_position = node_index_.at(to);
    odometry_chain_ = odometry_chain_ &&
                      from_position + 1 == to_position;
    factors_.push_back(Factor{from, to, measurement,
                              SymmetrizeInformation(information), Kernel::None,
                              FactorType::Relative});
}

void DensePoseGraphBackend::AddDeadReckoningFactor(
    std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
    const Eigen::Matrix<double, 6, 6>& information) {
    // The dense fallback currently uses the same non-robust relative edge
    // for both motion sources; the separate virtual entry keeps the source
    // distinction visible to fusion code and target-native backends.
    AddOdometryFactor(from, to, measurement, information);
}

void DensePoseGraphBackend::AddLocalizationPrior(
    std::uint32_t id, const Pose3d& measurement,
    const Eigen::Matrix<double, 6, 6>& information) {
    (void)NodeFor(id);
    factors_.push_back(Factor{id, 0, measurement,
                              SymmetrizeInformation(information), Kernel::Huber,
                              FactorType::Prior});
}

void DensePoseGraphBackend::AddLoopFactor(
    std::uint32_t from, std::uint32_t to, const Pose3d& measurement,
    const Eigen::Matrix<double, 6, 6>& information) {
    (void)NodeFor(from);
    (void)NodeFor(to);
    factors_.push_back(Factor{from, to, measurement,
                              SymmetrizeInformation(information), Kernel::Cauchy,
                              FactorType::Relative});
    ++loop_factor_count_;
}

Eigen::Matrix<double, 6, 1> DensePoseGraphBackend::Residual(
    const Factor& factor) const {
    if (factor.type == FactorType::Prior) {
        return PriorResidual(factor.measurement, NodeFor(factor.from).pose);
    }
    return PoseResidual(factor.measurement, NodeFor(factor.from).pose,
                        NodeFor(factor.to).pose);
}

bool DensePoseGraphBackend::IsSequentialOdometryFactor(
    const Factor& factor) const {
    if (factor.type != FactorType::Relative || factor.kernel != Kernel::None) {
        return false;
    }
    const auto from = node_index_.find(factor.from);
    const auto to = node_index_.find(factor.to);
    return from != node_index_.end() && to != node_index_.end() &&
           from->second + 1 == to->second;
}

bool DensePoseGraphBackend::ApplyPendingOdometryFastPath() {
    for (std::size_t index = optimized_factor_count_;
         index < factors_.size(); ++index) {
        if (!IsSequentialOdometryFactor(factors_[index])) {
            return false;
        }
    }

    for (std::size_t index = optimized_factor_count_;
         index < factors_.size(); ++index) {
        const Factor& factor = factors_[index];
        Node& from = NodeFor(factor.from);
        Node& to = NodeFor(factor.to);
        if (!to.fixed) {
            to.pose = Compose(from.pose, factor.measurement);
        }
    }
    return true;
}

double DensePoseGraphBackend::RobustWeight(double chi_squared,
                                           Kernel kernel) const {
    if (kernel == Kernel::None || chi_squared <= 0.0) {
        return 1.0;
    }
    if (kernel == Kernel::Huber) {
        return chi_squared <= kHuberDelta * kHuberDelta
                   ? 1.0
                   : kHuberDelta / std::sqrt(chi_squared);
    }
    return 1.0 / (1.0 + chi_squared / (kCauchyDelta * kCauchyDelta));
}

double DensePoseGraphBackend::RobustCost(double chi_squared,
                                         Kernel kernel) const {
    if (kernel == Kernel::None || chi_squared <= kHuberDelta * kHuberDelta) {
        return chi_squared;
    }
    if (kernel == Kernel::Huber) {
        return 2.0 * kHuberDelta * std::sqrt(chi_squared) -
               kHuberDelta * kHuberDelta;
    }
    return kCauchyDelta * kCauchyDelta *
           std::log1p(chi_squared / (kCauchyDelta * kCauchyDelta));
}

double DensePoseGraphBackend::Cost() const {
    double cost = 0.0;
    for (const Factor& factor : factors_) {
        const Eigen::Matrix<double, 6, 1> residual = Residual(factor);
        const double chi_squared =
            residual.dot(factor.information * residual);
        cost += RobustCost(std::max(0.0, chi_squared), factor.kernel);
    }
    return cost;
}

bool DensePoseGraphBackend::Optimize() {
    if (nodes_.size() < 2 || factors_.empty()) {
        last_cost_ = Cost();
        optimized_factor_count_ = factors_.size();
        optimized_loop_factor_count_ = loop_factor_count_;
        return !nodes_.empty();
    }

    if (optimized_factor_count_ == factors_.size()) {
        return !nodes_.empty() && std::isfinite(last_cost_);
    }

    // A newly appended sequential odometry edge only determines the new tail
    // pose. Solving the entire historical graph here creates an O(N^3) cost
    // even though no loop constraint changed.
    if (loop_factor_count_ == optimized_loop_factor_count_ &&
        odometry_chain_ && ApplyPendingOdometryFastPath()) {
        optimized_factor_count_ = factors_.size();
        optimized_loop_factor_count_ = loop_factor_count_;
        last_cost_ = 0.0;
        return true;
    }

    std::vector<int> variable_index(nodes_.size(), -1);
    int variable_count = 0;
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        if (!nodes_[index].fixed) {
            variable_index[index] = variable_count++;
        }
    }
    if (variable_count == 0) {
        last_cost_ = Cost();
        return true;
    }

    double previous_cost = Cost();
    for (int iteration = 0; iteration < iterations_; ++iteration) {
        const int dimension = variable_count * 6;
        Eigen::MatrixXd hessian = Eigen::MatrixXd::Zero(dimension, dimension);
        Eigen::VectorXd gradient = Eigen::VectorXd::Zero(dimension);

        for (const Factor& factor : factors_) {
            const auto from_iterator = node_index_.at(factor.from);
            const bool relative = factor.type == FactorType::Relative;
            const auto to_iterator =
                relative ? node_index_.at(factor.to) : from_iterator;
            const Eigen::Matrix<double, 6, 1> residual = Residual(factor);
            const double chi_squared = std::max(
                0.0, residual.dot(factor.information * residual));
            const double weight = RobustWeight(chi_squared, factor.kernel);
            const Eigen::Matrix<double, 6, 6> weighted_information =
                weight * factor.information;

            Eigen::Matrix<double, 6, 6> jacobian_from =
                Eigen::Matrix<double, 6, 6>::Zero();
            Eigen::Matrix<double, 6, 6> jacobian_to =
                Eigen::Matrix<double, 6, 6>::Zero();
            for (int column = 0; column < 6; ++column) {
                Eigen::Matrix<double, 6, 1> delta =
                    Eigen::Matrix<double, 6, 1>::Zero();
                delta[column] = kJacobianStep;
                if (!nodes_[from_iterator].fixed) {
                    const Pose3d original = nodes_[from_iterator].pose;
                    nodes_[from_iterator].pose = ApplyRightIncrement(original, delta);
                    jacobian_from.col(column) = (Residual(factor) - residual) /
                                                kJacobianStep;
                    nodes_[from_iterator].pose = original;
                }
                if (relative && !nodes_[to_iterator].fixed) {
                    const Pose3d original = nodes_[to_iterator].pose;
                    nodes_[to_iterator].pose = ApplyRightIncrement(original, delta);
                    jacobian_to.col(column) = (Residual(factor) - residual) /
                                              kJacobianStep;
                    nodes_[to_iterator].pose = original;
                }
            }

            const auto accumulate = [&](std::size_t node_position,
                                        const Eigen::Matrix<double, 6, 6>& jacobian) {
                if (nodes_[node_position].fixed) {
                    return;
                }
                const int offset = variable_index[node_position] * 6;
                hessian.block<6, 6>(offset, offset).noalias() +=
                    jacobian.transpose() * weighted_information * jacobian;
                gradient.segment<6>(offset).noalias() +=
                    jacobian.transpose() * weighted_information * residual;
            };
            const auto accumulate_cross = [&](std::size_t lhs_position,
                                              const Eigen::Matrix<double, 6, 6>& lhs,
                                              std::size_t rhs_position,
                                              const Eigen::Matrix<double, 6, 6>& rhs) {
                if (nodes_[lhs_position].fixed || nodes_[rhs_position].fixed) {
                    return;
                }
                const int lhs_offset = variable_index[lhs_position] * 6;
                const int rhs_offset = variable_index[rhs_position] * 6;
                hessian.block<6, 6>(lhs_offset, rhs_offset).noalias() +=
                    lhs.transpose() * weighted_information * rhs;
            };

            accumulate(from_iterator, jacobian_from);
            if (relative) {
                accumulate(to_iterator, jacobian_to);
                accumulate_cross(from_iterator, jacobian_from, to_iterator,
                                 jacobian_to);
                accumulate_cross(to_iterator, jacobian_to, from_iterator,
                                 jacobian_from);
            }
        }

        hessian = 0.5 * (hessian + hessian.transpose());
        const double damping = 1e-6 *
                               std::max(1.0, hessian.diagonal().cwiseAbs().maxCoeff());
        hessian.diagonal().array() += damping;
        Eigen::VectorXd step;
#if defined(X86_LIO_SLAM_WITH_BLOCK_PCG)
        bool solved_with_pcg = false;
        try {
            ScalarBlockPcgSolver pcg;
            const BlockSparseMatrix6 block_hessian =
                BlockSparseMatrix6::FromDense(hessian);
            const BlockPcgResult result =
                pcg.Solve(block_hessian, -gradient, step);
            solved_with_pcg = result.converged && step.allFinite();
        } catch (const std::exception&) {
            solved_with_pcg = false;
        }
        if (!solved_with_pcg) {
            const Eigen::LDLT<Eigen::MatrixXd> direct(hessian);
            if (direct.info() != Eigen::Success) {
                last_cost_ = previous_cost;
                return false;
            }
            step = direct.solve(-gradient);
        }
#else
        const Eigen::LDLT<Eigen::MatrixXd> direct(hessian);
        if (direct.info() != Eigen::Success) {
            last_cost_ = previous_cost;
            return false;
        }
        step = direct.solve(-gradient);
#endif
        if (!step.allFinite()) {
            last_cost_ = previous_cost;
            return false;
        }
        if (step.norm() < 1e-9) {
            break;
        }
        for (std::size_t index = 0; index < nodes_.size(); ++index) {
            if (!nodes_[index].fixed) {
                nodes_[index].pose = ApplyRightIncrement(
                    nodes_[index].pose,
                    step.segment<6>(variable_index[index] * 6));
            }
        }
        const double current_cost = Cost();
        if (!std::isfinite(current_cost)) {
            return false;
        }
        if (std::abs(previous_cost - current_cost) < 1e-10) {
            previous_cost = current_cost;
            break;
        }
        previous_cost = current_cost;
    }

    last_cost_ = previous_cost;
    optimized_factor_count_ = factors_.size();
    optimized_loop_factor_count_ = loop_factor_count_;
    return std::isfinite(last_cost_);
}

Pose3d DensePoseGraphBackend::GetOptimizedPose(std::uint32_t id) const {
    return NodeFor(id).pose;
}

}  // namespace x86_lio_slam
