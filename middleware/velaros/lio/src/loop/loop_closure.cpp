#include "x86_lio_slam/loop/loop_closure.h"

#include "x86_lio_slam/common/geometry.h"

#include <Eigen/SVD>

#include <algorithm>
#include <cmath>
#include <limits>

namespace x86_lio_slam {

void RadiusLoopDetector::Reset() { history_.clear(); }

std::vector<LoopCandidate> RadiusLoopDetector::Detect(const Keyframe& query) {
    std::vector<LoopCandidate> candidates;
    for (const Keyframe& target : history_) {
        if (query.id <= target.id || query.id - target.id < config_.min_id_separation) {
            continue;
        }
        const double distance =
            (query.T_odom_body.translation - target.T_odom_body.translation).norm();
        if (distance <= config_.search_radius) {
            candidates.push_back(LoopCandidate{query.id, target.id, distance});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const LoopCandidate& lhs, const LoopCandidate& rhs) {
                  return lhs.score < rhs.score;
              });
    if (candidates.size() > config_.max_candidates) {
        candidates.resize(config_.max_candidates);
    }
    history_.push_back(query);
    return candidates;
}

namespace {

struct PairSet {
    std::vector<Eigen::Vector3d> query;
    std::vector<Eigen::Vector3d> target;
};

PairSet FindPairs(const Keyframe& query, const Keyframe& target,
                  const Pose3d& estimate, double max_distance) {
    PairSet pairs;
    const double max_squared = max_distance * max_distance;
    for (const Eigen::Vector3f& query_point : query.cloud_body) {
        const Eigen::Vector3d query_point_double = query_point.cast<double>();
        const Eigen::Vector3d transformed = estimate.Transform(query_point_double);
        double best_squared = max_squared;
        const Eigen::Vector3f* best = nullptr;
        for (const Eigen::Vector3f& target_point : target.cloud_body) {
            const double squared =
                (transformed - target_point.cast<double>()).squaredNorm();
            if (squared < best_squared) {
                best_squared = squared;
                best = &target_point;
            }
        }
        if (best != nullptr) {
            pairs.query.push_back(query_point.cast<double>());
            pairs.target.push_back(best->cast<double>());
        }
    }
    return pairs;
}

Pose3d SolveRigidTransform(const PairSet& pairs) {
    Pose3d result;
    if (pairs.query.empty()) {
        return result;
    }
    Eigen::Vector3d query_centroid = Eigen::Vector3d::Zero();
    Eigen::Vector3d target_centroid = Eigen::Vector3d::Zero();
    for (std::size_t index = 0; index < pairs.query.size(); ++index) {
        query_centroid += pairs.query[index];
        target_centroid += pairs.target[index];
    }
    query_centroid /= static_cast<double>(pairs.query.size());
    target_centroid /= static_cast<double>(pairs.query.size());

    Eigen::Matrix3d covariance = Eigen::Matrix3d::Zero();
    for (std::size_t index = 0; index < pairs.query.size(); ++index) {
        covariance.noalias() +=
            (pairs.target[index] - target_centroid) *
            (pairs.query[index] - query_centroid).transpose();
    }
    const Eigen::JacobiSVD<Eigen::Matrix3d> svd(
        covariance, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Matrix3d reflection = Eigen::Matrix3d::Identity();
    reflection(2, 2) =
        (svd.matrixU() * svd.matrixV().transpose()).determinant();
    result.rotation = svd.matrixU() * reflection * svd.matrixV().transpose();
    result.translation = target_centroid - result.rotation * query_centroid;
    return result;
}

}  // namespace

LoopRegistrationResult CentroidLoopRegistration::Register(
    const Keyframe& query, const Keyframe& target) {
    LoopRegistrationResult result;
    if (query.cloud_body.size() < 3 || target.cloud_body.size() < 3) {
        return result;
    }

    Pose3d estimate = RelativePose(target.T_odom_body, query.T_odom_body);
    PairSet pairs;
    for (int iteration = 0; iteration < iterations_; ++iteration) {
        pairs = FindPairs(query, target, estimate, correspondence_radius_);
        if (pairs.query.size() < 3) {
            return result;
        }
        estimate = SolveRigidTransform(pairs);
    }

    double squared_error = 0.0;
    for (std::size_t index = 0; index < pairs.query.size(); ++index) {
        squared_error +=
            (estimate.Transform(pairs.query[index]) - pairs.target[index]).squaredNorm();
    }
    result.converged = true;
    result.T_target_query = estimate;
    result.inliers = pairs.query.size();
    result.fitness = squared_error / static_cast<double>(pairs.query.size());
    return result;
}

}  // namespace x86_lio_slam
