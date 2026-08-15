#include "x86_lio_slam/localization/lidar_localizer.h"

#include "x86_lio_slam/common/geometry.h"

#include <Eigen/Cholesky>
#include <Eigen/Eigenvalues>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace x86_lio_slam {

namespace {

constexpr double kTimestampEpsilon = 1e-9;
constexpr double kInformationDamping = 1e-6;
constexpr double kPi = 3.141592653589793238462643383279502884;

LidarLocalizerConfig NormalizeConfig(LidarLocalizerConfig config) {
    if (!(config.map_resolution > 0.0F) ||
        !std::isfinite(config.map_resolution)) {
        config.map_resolution = 1.0F;
    }
    config.max_motion_queue_size =
        std::max<std::size_t>(1, config.max_motion_queue_size);
    config.min_scan_points = std::max<std::size_t>(3, config.min_scan_points);
    config.max_scan_points =
        std::max(config.min_scan_points, config.max_scan_points);
    config.min_correspondences =
        std::max<std::size_t>(3, config.min_correspondences);
    config.max_iterations = std::max<std::size_t>(1, config.max_iterations);
    config.max_interpolation_gap =
        std::max(0.0, config.max_interpolation_gap);
    config.max_correspondence_distance =
        std::max(1e-4, config.max_correspondence_distance);
    config.min_plane_variance = std::max(0.0, config.min_plane_variance);
    config.max_planarity_ratio =
        std::clamp(config.max_planarity_ratio, 1e-6, 1.0);
    config.huber_delta = std::max(1e-6, config.huber_delta);
    config.confidence_sigma = std::max(1e-6, config.confidence_sigma);
    config.min_confidence = std::clamp(config.min_confidence, 0.0, 1.0);
    config.convergence_translation =
        std::max(1e-8, config.convergence_translation);
    config.convergence_rotation =
        std::max(1e-8, config.convergence_rotation);
    config.max_consecutive_failures =
        std::max<std::size_t>(1, config.max_consecutive_failures);
    if (!std::isfinite(config.global_search_translation_radius) ||
        config.global_search_translation_radius < 0.0) {
        config.global_search_translation_radius = 8.0;
    }
    if (!std::isfinite(config.global_search_translation_step) ||
        config.global_search_translation_step <= 0.0) {
        config.global_search_translation_step = 2.0;
    }
    config.global_search_yaw_bins =
        std::max<std::size_t>(4, config.global_search_yaw_bins);
    config.global_search_points =
        std::max<std::size_t>(16, config.global_search_points);
    config.global_search_refine_candidates = std::clamp<std::size_t>(
        config.global_search_refine_candidates, 1, 32);
    config.global_search_min_inliers = std::max<std::size_t>(
        3, std::min(config.global_search_min_inliers,
                    config.min_correspondences));
    config.global_relocalization_failure_threshold = std::max<std::size_t>(
        1, config.global_relocalization_failure_threshold);
    return config;
}

bool IsFiniteInformation(const Information6d& information) {
    if (!information.allFinite()) {
        return false;
    }
    const Information6d symmetric =
        0.5 * (information + information.transpose());
    Eigen::LDLT<Information6d> decomposition(symmetric);
    if (decomposition.info() != Eigen::Success) {
        return false;
    }
    return (decomposition.vectorD().array() > 0.0).all();
}

Pose3d InterpolatePose(const Pose3d& first, const Pose3d& second,
                       double alpha) {
    Pose3d result;
    result.translation = first.translation +
                         alpha * (second.translation - first.translation);
    const Eigen::Vector3d rotation_delta = LogSO3(
        first.rotation.transpose() * second.rotation);
    result.rotation = first.rotation * ExpSO3(alpha * rotation_delta);
    return result;
}

}  // namespace

LidarLocalizer::LidarLocalizer(LidarLocalizerConfig config)
    : config_(NormalizeConfig(std::move(config))),
      map_(config_.map_resolution, config_.map_capacity) {
    if (!IsFiniteInformation(config_.localization_information)) {
        config_.localization_information =
            DefaultLidarLocalizationInformation();
    }
    Reset();
}

void LidarLocalizer::Reset() {
    lidar_odom_queue_.clear();
    dead_reckoning_queue_.clear();
    initialized_ = false;
    last_pose_ = Pose3d::Identity();
    last_lidar_odom_set_ = false;
    last_dead_reckoning_set_ = false;
    match_failures_ = 0;
    lidar_odom_unreliable_count_ = 0;
    lidar_odom_reliable_ = true;
    map_height_ = initial_pose_set_ ? initial_pose_.translation.z() : 0.0;
    last_cloud_timestamp_ = -1.0;
    last_result_ = LidarLocalizationResult{};
    last_result_.information = config_.localization_information;
    stats_ = LidarLocalizerStats{};
}

bool LidarLocalizer::SetMap(std::span<const Eigen::Vector3f> points) {
    map_.Clear();
    map_ready_ = false;
    stats_.map_points_accepted = 0;

    if (points.empty() || config_.map_capacity == 0) {
        Reset();
        return false;
    }

    std::uint64_t accepted_points = 0;
    for (const Eigen::Vector3f& point : points) {
        if (!point.allFinite()) {
            continue;
        }
        (void)map_.AddPoint(point);
        ++accepted_points;
    }
    map_ready_ = map_.Size() > 0;
    Reset();
    stats_.map_points_accepted = accepted_points;
    return map_ready_;
}

void LidarLocalizer::SetInitialPose(const Pose3d& pose) {
    if (!IsFinitePose(pose)) {
        return;
    }
    initial_pose_ = pose;
    initial_pose_set_ = true;
    initialized_ = false;
    last_pose_ = pose;
    map_height_ = pose.translation.z();
    match_failures_ = 0;
    lidar_odom_unreliable_count_ = 0;
    lidar_odom_reliable_ = true;
    last_lidar_odom_set_ = false;
    last_dead_reckoning_set_ = false;
    last_cloud_timestamp_ = -1.0;
}

void LidarLocalizer::ResetLastPose(const Pose3d& pose) {
    if (!IsFinitePose(pose)) {
        return;
    }
    initial_pose_ = pose;
    initial_pose_set_ = true;
    initialized_ = true;
    last_pose_ = pose;
    map_height_ = pose.translation.z();
    match_failures_ = 0;
    lidar_odom_unreliable_count_ = 0;
    lidar_odom_reliable_ = true;
    last_lidar_odom_set_ = false;
    last_dead_reckoning_set_ = false;
    last_cloud_timestamp_ = -1.0;
}

bool LidarLocalizer::AddMotionObservation(
    std::deque<TimedMotionObservation>& queue,
    const TimedMotionObservation& observation) {
    if (!std::isfinite(observation.timestamp) ||
        !IsFinitePose(observation.pose)) {
        return false;
    }

    if (!queue.empty()) {
        const double delta = observation.timestamp - queue.back().timestamp;
        if (delta < -kTimestampEpsilon) {
            ++stats_.rejected_timestamps;
            return false;
        }
        if (std::abs(delta) <= kTimestampEpsilon) {
            queue.back() = observation;
            return true;
        }
    }

    queue.push_back(observation);
    while (queue.size() > config_.max_motion_queue_size) {
        queue.pop_front();
    }
    return true;
}

bool LidarLocalizer::AddLidarOdomObservation(
    const LidarOdomObservation& observation) {
    return AddMotionObservation(lidar_odom_queue_, observation);
}

bool LidarLocalizer::AddDeadReckoningObservation(
    const DeadReckoningObservation& observation) {
    return AddMotionObservation(dead_reckoning_queue_, observation);
}

bool LidarLocalizer::InterpolateMotionObservation(
    const std::deque<TimedMotionObservation>& queue, double timestamp,
    TimedMotionObservation& result) const {
    if (queue.empty() || !std::isfinite(timestamp)) {
        return false;
    }

    if (queue.size() == 1) {
        if (std::abs(timestamp - queue.front().timestamp) >
            config_.max_interpolation_gap) {
            return false;
        }
        result = queue.front();
        result.timestamp = timestamp;
        return true;
    }

    std::size_t left_index = 0;
    std::size_t right_index = 1;
    if (timestamp <= queue.front().timestamp) {
        if (queue.front().timestamp - timestamp >
            config_.max_interpolation_gap) {
            return false;
        }
    } else if (timestamp >= queue.back().timestamp) {
        if (timestamp - queue.back().timestamp >
            config_.max_interpolation_gap) {
            return false;
        }
        left_index = queue.size() - 2;
        right_index = queue.size() - 1;
    } else {
        const auto upper = std::lower_bound(
            queue.begin(), queue.end(), timestamp,
            [](const TimedMotionObservation& observation, double value) {
                return observation.timestamp < value;
            });
        if (upper == queue.begin() || upper == queue.end()) {
            return false;
        }
        right_index = static_cast<std::size_t>(upper - queue.begin());
        left_index = right_index - 1;
    }

    const TimedMotionObservation& first = queue[left_index];
    const TimedMotionObservation& second = queue[right_index];
    const double dt = second.timestamp - first.timestamp;
    if (!(dt > kTimestampEpsilon)) {
        result = std::abs(timestamp - first.timestamp) <=
                         std::abs(timestamp - second.timestamp)
                     ? first
                     : second;
        result.timestamp = timestamp;
        return true;
    }

    const double alpha = (timestamp - first.timestamp) / dt;
    result = first;
    result.timestamp = timestamp;
    result.pose = InterpolatePose(first.pose, second.pose, alpha);
    result.velocity = first.velocity +
                      alpha * (second.velocity - first.velocity);
    result.valid = first.valid && second.valid;
    result.reliable = first.reliable && second.reliable;
    result.confidence = std::min(first.confidence, second.confidence);
    result.information = alpha <= 0.5 ? first.information : second.information;
    return true;
}

Pose3d LidarLocalizer::PredictPose(
    const Pose3d& fallback, const TimedMotionObservation* lidar_odom,
    const TimedMotionObservation* dead_reckoning, bool& used_lidar_odom,
    bool& used_dead_reckoning) const {
    used_lidar_odom = false;
    used_dead_reckoning = false;
    if (!initialized_) {
        return initial_pose_set_ ? initial_pose_ : fallback;
    }

    if (lidar_odom != nullptr && lidar_odom->valid &&
        last_lidar_odom_set_) {
        used_lidar_odom = true;
        return Compose(last_pose_,
                       RelativePose(last_lidar_odom_.pose, lidar_odom->pose));
    }
    if (dead_reckoning != nullptr && dead_reckoning->valid &&
        last_dead_reckoning_set_) {
        used_dead_reckoning = true;
        return Compose(
            last_pose_,
            RelativePose(last_dead_reckoning_.pose, dead_reckoning->pose));
    }
    return last_pose_;
}

Pose3d LidarLocalizer::ProjectToPlanar(const Pose3d& pose, bool zero_height,
                                       double height) {
    Pose3d result = pose;
    const double yaw = std::atan2(pose.rotation(1, 0), pose.rotation(0, 0));
    result.rotation = ExpSO3(Eigen::Vector3d(0.0, 0.0, yaw));
    result.translation.z() = zero_height ? 0.0 : height;
    return result;
}

bool LidarLocalizer::IsFinitePose(const Pose3d& pose) {
    return pose.rotation.allFinite() && pose.translation.allFinite();
}

bool LidarLocalizer::Match(std::span<const Eigen::Vector3f> scan,
                           const Pose3d& guess, MatchResult& result) const {
    if (scan.empty() || !map_ready_ || !IsFinitePose(guess)) {
        return false;
    }

    using Vector6d = Eigen::Matrix<double, 6, 1>;
    using Matrix6d = Eigen::Matrix<double, 6, 6>;

    auto accumulate = [&](const Pose3d& pose, Matrix6d& hessian,
                          Vector6d& gradient, std::size_t& inliers,
                          double& squared_error) {
        hessian.setZero();
        gradient.setZero();
        inliers = 0;
        squared_error = 0.0;

        SuperLioOctVoxMap::QueryCache cache;
        cache.Reset();
        std::array<Eigen::Vector3f, 5> neighbors{};
        const double max_distance_squared =
            config_.max_correspondence_distance *
            config_.max_correspondence_distance;

        for (const Eigen::Vector3f& point : scan) {
            const Eigen::Vector3f transformed = pose.Transform(point);
            const std::size_t count = map_.FindNearest(
                transformed, std::span<Eigen::Vector3f>(neighbors), cache);
            if (count < 3) {
                continue;
            }

            double nearest_distance_squared =
                std::numeric_limits<double>::infinity();
            Eigen::Vector3d mean = Eigen::Vector3d::Zero();
            for (std::size_t index = 0; index < count; ++index) {
                const Eigen::Vector3d neighbor = neighbors[index].cast<double>();
                mean += neighbor;
                nearest_distance_squared = std::min(
                    nearest_distance_squared,
                    (neighbor - transformed.cast<double>()).squaredNorm());
            }
            if (nearest_distance_squared > max_distance_squared) {
                continue;
            }
            mean /= static_cast<double>(count);

            Eigen::Matrix3d covariance = Eigen::Matrix3d::Zero();
            for (std::size_t index = 0; index < count; ++index) {
                const Eigen::Vector3d centered =
                    neighbors[index].cast<double>() - mean;
                covariance.noalias() += centered * centered.transpose();
            }
            covariance /= static_cast<double>(count);

            Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(covariance);
            if (solver.info() != Eigen::Success) {
                continue;
            }
            const Eigen::Vector3d eigenvalues = solver.eigenvalues();
            const double variance_sum = eigenvalues.sum();
            if (!(variance_sum > config_.min_plane_variance) ||
                eigenvalues[0] / variance_sum > config_.max_planarity_ratio) {
                continue;
            }

            const Eigen::Vector3d normal = solver.eigenvectors().col(0);
            const Eigen::Vector3d transformed_double = transformed.cast<double>();
            const double residual = normal.dot(transformed_double - mean);
            const double absolute_residual = std::abs(residual);
            if (absolute_residual > config_.max_correspondence_distance) {
                continue;
            }

            const double robust_weight =
                absolute_residual <= config_.huber_delta
                    ? 1.0
                    : config_.huber_delta / absolute_residual;
            Vector6d jacobian;
            jacobian.head<3>() = normal;
            jacobian.tail<3>() =
                (-pose.rotation * Hat(point.cast<double>())).transpose() *
                normal;
            hessian.noalias() += robust_weight * jacobian * jacobian.transpose();
            gradient.noalias() += robust_weight * jacobian * residual;
            squared_error += residual * residual;
            ++inliers;
        }
    };

    Pose3d pose = guess;
    if (config_.force_2d) {
        pose = ProjectToPlanar(pose, config_.zero_height, map_height_);
    }

    bool converged = false;
    std::size_t iterations = 0;
    for (std::size_t iteration = 0; iteration < config_.max_iterations;
         ++iteration) {
        Matrix6d hessian;
        Vector6d gradient;
        std::size_t inliers = 0;
        double squared_error = 0.0;
        accumulate(pose, hessian, gradient, inliers, squared_error);
        if (inliers < config_.min_correspondences) {
            return false;
        }

        const double diagonal_scale =
            std::max(1.0, hessian.diagonal().cwiseAbs().maxCoeff());
        hessian.diagonal().array() +=
            kInformationDamping * diagonal_scale;
        Eigen::LDLT<Matrix6d> decomposition(hessian);
        if (decomposition.info() != Eigen::Success) {
            return false;
        }
        Vector6d increment = decomposition.solve(-gradient);
        if (!increment.allFinite()) {
            return false;
        }

        const double translation_norm = increment.head<3>().norm();
        if (translation_norm > 1.0) {
            increment.head<3>() *= 1.0 / translation_norm;
        }
        const double rotation_norm = increment.tail<3>().norm();
        if (rotation_norm > 0.35) {
            increment.tail<3>() *= 0.35 / rotation_norm;
        }
        if (config_.force_2d) {
            increment[2] = 0.0;
            increment[3] = 0.0;
            increment[4] = 0.0;
        }

        pose.translation += increment.head<3>();
        pose.rotation = pose.rotation * ExpSO3(increment.tail<3>());
        if (config_.force_2d) {
            pose = ProjectToPlanar(pose, config_.zero_height, map_height_);
        }
        if (!IsFinitePose(pose)) {
            return false;
        }

        iterations = iteration + 1;
        if (increment.head<3>().norm() <= config_.convergence_translation &&
            increment.tail<3>().norm() <= config_.convergence_rotation) {
            converged = true;
            break;
        }
    }

    Matrix6d final_hessian;
    Vector6d final_gradient;
    std::size_t final_inliers = 0;
    double final_squared_error = 0.0;
    accumulate(pose, final_hessian, final_gradient, final_inliers,
               final_squared_error);
    if (final_inliers < config_.min_correspondences ||
        !std::isfinite(final_squared_error)) {
        return false;
    }

    const double rmse =
        std::sqrt(final_squared_error / static_cast<double>(final_inliers));
    const double inlier_ratio =
        std::min(1.0, static_cast<double>(final_inliers) /
                            static_cast<double>(scan.size()));
    const double confidence =
        inlier_ratio * std::exp(-rmse / config_.confidence_sigma);
    if (!std::isfinite(confidence) || !IsFinitePose(pose)) {
        return false;
    }

    final_hessian.diagonal().array() += kInformationDamping;
    result.pose = pose;
    result.information = 0.5 * (final_hessian + final_hessian.transpose());
    result.confidence = std::clamp(confidence, 0.0, 1.0);
    result.rmse = rmse;
    result.correspondences = final_inliers;
    result.iterations = iterations;
    result.converged = converged;
    return true;
}

bool LidarLocalizer::CoarseScore(std::span<const Eigen::Vector3f> scan,
                                 const Pose3d& candidate,
                                 CoarseCandidate& result) const {
    if (scan.empty() || !map_ready_ || !IsFinitePose(candidate)) {
        return false;
    }

    SuperLioOctVoxMap::QueryCache cache;
    cache.Reset();
    std::array<Eigen::Vector3f, 5> neighbors{};
    const double max_distance_squared =
        config_.max_correspondence_distance *
        config_.max_correspondence_distance;
    const std::size_t stride =
        (scan.size() + config_.global_search_points - 1) /
        config_.global_search_points;
    const std::size_t sample_count =
        (scan.size() + stride - 1) / stride;
    if (sample_count == 0) {
        return false;
    }

    std::size_t inliers = 0;
    double normalized_error = 0.0;
    for (std::size_t index = 0; index < scan.size(); index += stride) {
        const Eigen::Vector3f transformed = candidate.Transform(scan[index]);
        const std::size_t count = map_.FindNearest(
            transformed, std::span<Eigen::Vector3f>(neighbors), cache);
        if (count == 0) {
            continue;
        }
        const double distance_squared =
            (neighbors[0] - transformed).cast<double>().squaredNorm();
        if (distance_squared <= max_distance_squared) {
            ++inliers;
            normalized_error +=
                std::min(1.0, distance_squared / max_distance_squared);
        }
    }

    result.pose = candidate;
    result.inliers = inliers;
    const double inlier_ratio =
        static_cast<double>(inliers) / static_cast<double>(sample_count);
    const double mean_error =
        inliers == 0 ? 1.0 : normalized_error / static_cast<double>(inliers);
    result.score = inlier_ratio - 0.25 * mean_error;
    return inliers >= config_.global_search_min_inliers &&
           std::isfinite(result.score);
}

bool LidarLocalizer::GlobalMatch(std::span<const Eigen::Vector3f> scan,
                                 const Pose3d& seed, MatchResult& result) {
    ++stats_.global_search_attempts;
    if (scan.empty() || !map_ready_ || !IsFinitePose(seed)) {
        return false;
    }

    const Pose3d base = ProjectToPlanar(seed, config_.zero_height,
                                        map_height_);
    const int translation_steps = static_cast<int>(std::ceil(
        config_.global_search_translation_radius /
        config_.global_search_translation_step));
    std::vector<CoarseCandidate> candidates;
    const std::size_t yaw_bins = config_.global_search_yaw_bins;
    const std::size_t side = static_cast<std::size_t>(2 * translation_steps + 1);
    candidates.reserve(side * side * yaw_bins);

    for (std::size_t yaw_index = 0; yaw_index < yaw_bins; ++yaw_index) {
        const double yaw = -kPi +
                           (2.0 * kPi * static_cast<double>(yaw_index)) /
                               static_cast<double>(yaw_bins);
        for (int offset_y = -translation_steps;
             offset_y <= translation_steps; ++offset_y) {
            for (int offset_x = -translation_steps;
                 offset_x <= translation_steps; ++offset_x) {
                Pose3d candidate;
                candidate.rotation =
                    ExpSO3(Eigen::Vector3d(0.0, 0.0, yaw));
                candidate.translation = base.translation;
                candidate.translation.x() +=
                    static_cast<double>(offset_x) *
                    config_.global_search_translation_step;
                candidate.translation.y() +=
                    static_cast<double>(offset_y) *
                    config_.global_search_translation_step;
                candidate = ProjectToPlanar(candidate, config_.zero_height,
                                            map_height_);

                CoarseCandidate scored;
                ++stats_.global_candidates_evaluated;
                if (CoarseScore(scan, candidate, scored)) {
                    candidates.push_back(scored);
                }
            }
        }
    }
    if (candidates.empty()) {
        return false;
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const CoarseCandidate& lhs, const CoarseCandidate& rhs) {
                  if (lhs.score != rhs.score) {
                      return lhs.score > rhs.score;
                  }
                  return lhs.inliers > rhs.inliers;
              });

    MatchResult best_match;
    double best_score = -std::numeric_limits<double>::infinity();
    bool found_match = false;
    const std::size_t refine_count = std::min(
        config_.global_search_refine_candidates, candidates.size());
    for (std::size_t index = 0; index < refine_count; ++index) {
        MatchResult refined;
        if (!Match(scan, candidates[index].pose, refined)) {
            continue;
        }
        const double combined_score =
            0.5 * candidates[index].score + 0.5 * refined.confidence;
        if (!found_match || combined_score > best_score) {
            found_match = true;
            best_score = combined_score;
            best_match = refined;
        }
    }
    if (!found_match || best_match.confidence < config_.min_confidence) {
        return false;
    }

    result = best_match;
    ++stats_.global_search_successes;
    return true;
}

bool LidarLocalizer::ProcessCloud(
    double timestamp, std::span<const PointXYZIT> cloud,
    LidarLocalizationResult& result) {
    std::vector<Eigen::Vector3f> points;
    points.reserve(std::min(cloud.size(), config_.max_scan_points));
    const std::size_t stride =
        cloud.size() > config_.max_scan_points
            ? (cloud.size() + config_.max_scan_points - 1) /
                  config_.max_scan_points
            : 1;
    for (std::size_t index = 0; index < cloud.size(); index += stride) {
        if (IsFinite(cloud[index])) {
            points.push_back(cloud[index].Vector());
        }
    }
    return ProcessCloud(timestamp,
                        std::span<const Eigen::Vector3f>(points), result);
}

bool LidarLocalizer::ProcessCloud(
    double timestamp, std::span<const Eigen::Vector3f> cloud,
    LidarLocalizationResult& result) {
    ++stats_.localization_calls;
    result = LidarLocalizationResult{};
    result.timestamp = timestamp + config_.lidar_time_offset;
    result.information = config_.localization_information;

    auto reject = [&](LidarLocalizationStatus status) {
        result.status = status;
        result.valid = false;
        result.pose = initialized_ ? last_pose_ : initial_pose_;
        ++stats_.failed_matches;
        last_result_ = result;
        return false;
    };

    std::vector<Eigen::Vector3f> finite_cloud;
    finite_cloud.reserve(std::min(cloud.size(), config_.max_scan_points));
    const std::size_t stride =
        cloud.size() > config_.max_scan_points
            ? (cloud.size() + config_.max_scan_points - 1) /
                  config_.max_scan_points
            : 1;
    for (std::size_t index = 0; index < cloud.size(); index += stride) {
        if (cloud[index].allFinite()) {
            finite_cloud.push_back(cloud[index]);
        }
    }

    if (!std::isfinite(timestamp) ||
        finite_cloud.size() < config_.min_scan_points) {
        ++stats_.rejected_scans;
        return reject(initialized_ ? LidarLocalizationStatus::FollowingDeadReckoning
                                   : LidarLocalizationStatus::Initializing);
    }
    if (last_cloud_timestamp_ >= 0.0 &&
        result.timestamp < last_cloud_timestamp_ - kTimestampEpsilon) {
        ++stats_.rejected_timestamps;
        return reject(initialized_ ? LidarLocalizationStatus::FollowingDeadReckoning
                                   : LidarLocalizationStatus::Initializing);
    }
    last_cloud_timestamp_ = result.timestamp;
    if (!map_ready_) {
        ++stats_.rejected_scans;
        return reject(LidarLocalizationStatus::Idle);
    }
    if (!initialized_ && !initial_pose_set_ &&
        !config_.enable_global_relocalization) {
        ++stats_.rejected_scans;
        return reject(LidarLocalizationStatus::Initializing);
    }

    const double current_timestamp = result.timestamp;
    TimedMotionObservation lidar_odom;
    TimedMotionObservation dead_reckoning;
    const bool has_lidar_odom =
        InterpolateMotionObservation(lidar_odom_queue_, current_timestamp,
                                      lidar_odom) &&
        lidar_odom.valid;
    const bool has_dead_reckoning =
        InterpolateMotionObservation(dead_reckoning_queue_, current_timestamp,
                                     dead_reckoning) &&
        dead_reckoning.valid;

    bool used_lidar_odom = false;
    bool used_dead_reckoning = false;
    const Pose3d fallback = initial_pose_set_ ? initial_pose_ : last_pose_;
    Pose3d guess = PredictPose(
        fallback, has_lidar_odom ? &lidar_odom : nullptr,
        has_dead_reckoning ? &dead_reckoning : nullptr, used_lidar_odom,
        used_dead_reckoning);
    if (config_.force_2d) {
        guess = ProjectToPlanar(guess, config_.zero_height, map_height_);
    }

    MatchResult match;
    bool matched = false;
    bool used_global_relocalization = false;
    const std::span<const Eigen::Vector3f> finite_scan(finite_cloud);
    if (config_.enable_global_relocalization && !initialized_) {
        matched = GlobalMatch(finite_scan, guess, match);
        used_global_relocalization = matched;
    } else {
        matched = Match(finite_scan, guess, match);
        const bool weak_local_match =
            !matched || match.confidence < config_.min_confidence;
        if (weak_local_match && config_.enable_global_relocalization &&
            match_failures_ + 1 >=
                config_.global_relocalization_failure_threshold) {
            MatchResult global_match;
            if (GlobalMatch(finite_scan, guess, global_match)) {
                match = global_match;
                matched = true;
                used_global_relocalization = true;
            }
        }
    }
    result.used_lidar_odom_prediction = used_lidar_odom;
    result.used_dead_reckoning_prediction = used_dead_reckoning;
    result.used_global_relocalization = used_global_relocalization;
    result.lidar_odom_reliable = lidar_odom_reliable_;
    result.pose = matched ? match.pose : guess;
    result.confidence = matched ? match.confidence : 0.0;
    result.rmse = matched ? match.rmse : 0.0;
    result.correspondences = matched ? match.correspondences : 0;
    result.iterations = matched ? match.iterations : 0;
    if (matched) {
        result.information = match.information;
    }

    if (!matched || match.confidence < config_.min_confidence) {
        ++match_failures_;
        result.status = initialized_
                            ? (match_failures_ >=
                                       config_.max_consecutive_failures
                                   ? LidarLocalizationStatus::Fail
                                   : LidarLocalizationStatus::FollowingDeadReckoning)
                            : LidarLocalizationStatus::Initializing;
        result.valid = false;
        ++stats_.failed_matches;
        last_result_ = result;
        return false;
    }

    double consistency_error = 0.0;
    bool consistency_normal = true;
    if (has_lidar_odom && last_lidar_odom_set_ && initialized_) {
        const Pose3d lidar_delta =
            RelativePose(last_lidar_odom_.pose, lidar_odom.pose);
        const Pose3d absolute_delta = RelativePose(last_pose_, match.pose);
        consistency_error =
            (lidar_delta.translation - absolute_delta.translation).head<2>().norm();
        consistency_normal =
            consistency_error <= config_.lidar_odom_consistency_threshold;
        if (!consistency_normal) {
            lidar_odom_reliable_ = false;
            lidar_odom_unreliable_count_ = config_.unreliable_recovery_frames;
        } else if (lidar_odom_unreliable_count_ > 0) {
            --lidar_odom_unreliable_count_;
            if (lidar_odom_unreliable_count_ == 0) {
                lidar_odom_reliable_ = true;
            }
        }
    }
    if (has_lidar_odom && !lidar_odom.reliable) {
        lidar_odom_reliable_ = false;
        lidar_odom_unreliable_count_ = config_.unreliable_recovery_frames;
        consistency_normal = false;
    }

    const Pose3d reference_pose = initialized_ ? guess : match.pose;
    const Pose3d correction = RelativePose(reference_pose, match.pose);
    result.smooth = correction.translation.head<2>().norm() < 0.5 &&
                    RotationDistance(correction.rotation) <
                        2.0 * kPi / 180.0;
    result.lidar_odom_consistency_error = consistency_error;
    result.lidar_odom_error_normal = consistency_normal;
    result.lidar_odom_reliable = lidar_odom_reliable_;
    result.valid = true;
    result.status = LidarLocalizationStatus::Good;

    last_pose_ = match.pose;
    initialized_ = true;
    initial_pose_ = match.pose;
    initial_pose_set_ = true;
    map_height_ = match.pose.translation.z();
    match_failures_ = 0;
    if (has_lidar_odom) {
        last_lidar_odom_ = lidar_odom;
        last_lidar_odom_set_ = true;
    }
    if (has_dead_reckoning) {
        last_dead_reckoning_ = dead_reckoning;
        last_dead_reckoning_set_ = true;
    }

    ++stats_.successful_matches;
    stats_.confidence_sum += result.confidence;
    stats_.rmse_sum += result.rmse;
    last_result_ = result;
    return true;
}

}  // namespace x86_lio_slam
