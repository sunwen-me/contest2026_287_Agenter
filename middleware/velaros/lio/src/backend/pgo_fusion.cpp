#include "x86_lio_slam/backend/pgo_fusion.h"

#include "x86_lio_slam/common/geometry.h"

#include <Eigen/Cholesky>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace x86_lio_slam {

namespace {

constexpr double kTimestampEpsilon = 1e-9;
constexpr double kInformationEpsilon = 1e-12;
constexpr double kPi = 3.141592653589793238462643383279502884;

bool IsFinitePose(const Pose3d& pose) {
    return pose.rotation.allFinite() && pose.translation.allFinite();
}

Information6d SymmetrizeInformation(const Information6d& information) {
    return 0.5 * (information + information.transpose());
}

bool IsUsableInformation(const Information6d& information) {
    if (!information.allFinite() || information.cwiseAbs().maxCoeff() <=
                                         kInformationEpsilon) {
        return false;
    }
    const Information6d symmetric = SymmetrizeInformation(information);
    for (int index = 0; index < 6; ++index) {
        if (!std::isfinite(symmetric(index, index)) ||
            symmetric(index, index) <= kInformationEpsilon) {
            return false;
        }
    }
    const Eigen::LDLT<Information6d> decomposition(symmetric);
    if (decomposition.info() != Eigen::Success) {
        return false;
    }
    return (decomposition.vectorD().array() > kInformationEpsilon).all();
}

Information6d ResolveInformation(
    const std::optional<Information6d>& information,
    const Information6d& fallback) {
    if (information.has_value() && IsUsableInformation(*information)) {
        return SymmetrizeInformation(*information);
    }
    return SymmetrizeInformation(fallback);
}

Information6d ScaleByConfidence(const Information6d& information,
                                double confidence,
                                double minimum_confidence,
                                double information_scale,
                                double confidence_power,
                                double minimum_scale,
                                double maximum_scale) {
    const double bounded_confidence = std::clamp(
        std::isfinite(confidence) ? confidence : minimum_confidence, 0.0, 1.0);
    const double effective_confidence =
        std::max(bounded_confidence, minimum_confidence);
    const double quality_scale = information_scale * std::pow(
        effective_confidence, confidence_power);
    const double bounded_scale = std::clamp(quality_scale, minimum_scale,
                                            maximum_scale);
    return SymmetrizeInformation(information * bounded_scale);
}

Information6d CalibrateLocalizationInformation(
    const Information6d& information, const Information6d& fallback) {
    const Information6d observed = SymmetrizeInformation(information);
    const Information6d reference = SymmetrizeInformation(fallback);
    if (!IsUsableInformation(observed) || !IsUsableInformation(reference)) {
        return reference;
    }

    const double observed_translation =
        std::abs(observed.block<3, 3>(0, 0).trace()) / 3.0;
    const double observed_rotation =
        std::abs(observed.block<3, 3>(3, 3).trace()) / 3.0;
    const double reference_translation =
        std::abs(reference.block<3, 3>(0, 0).trace()) / 3.0;
    const double reference_rotation =
        std::abs(reference.block<3, 3>(3, 3).trace()) / 3.0;
    if (!(observed_translation > kInformationEpsilon) ||
        !(observed_rotation > kInformationEpsilon) ||
        !(reference_translation > kInformationEpsilon) ||
        !(reference_rotation > kInformationEpsilon)) {
        return reference;
    }

    const double translation_scale =
        reference_translation / observed_translation;
    const double rotation_scale = reference_rotation / observed_rotation;
    Information6d calibrated = observed;
    calibrated.block<3, 3>(0, 0) *= translation_scale;
    calibrated.block<3, 3>(3, 3) *= rotation_scale;
    const double cross_scale = std::sqrt(translation_scale * rotation_scale);
    calibrated.block<3, 3>(0, 3) *= cross_scale;
    calibrated.block<3, 3>(3, 0) *= cross_scale;
    if (!IsUsableInformation(calibrated)) {
        return reference;
    }
    return calibrated;
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

template <typename Observation>
bool InterpolateMotionQueue(const std::deque<Observation>& queue,
                            double query_timestamp, double max_gap,
                            Observation& result, double& delta_t) {
    if (queue.empty() || !std::isfinite(query_timestamp)) {
        return false;
    }

    if (queue.size() == 1) {
        if (std::abs(query_timestamp - queue.front().timestamp) > max_gap) {
            return false;
        }
        result = queue.front();
        result.timestamp = query_timestamp;
        delta_t = query_timestamp - queue.front().timestamp;
        return true;
    }

    std::size_t left_index = 0;
    std::size_t right_index = 1;
    if (query_timestamp <= queue.front().timestamp) {
        if (queue.front().timestamp - query_timestamp > max_gap) {
            return false;
        }
    } else if (query_timestamp >= queue.back().timestamp) {
        if (query_timestamp - queue.back().timestamp > max_gap) {
            return false;
        }
        left_index = queue.size() - 2;
        right_index = queue.size() - 1;
    } else {
        const auto upper = std::lower_bound(
            queue.begin(), queue.end(), query_timestamp,
            [](const Observation& observation, double timestamp) {
                return observation.timestamp < timestamp;
            });
        right_index = static_cast<std::size_t>(upper - queue.begin());
        if (right_index == 0 || right_index == queue.size()) {
            return false;
        }
        left_index = right_index - 1;
    }

    const Observation& first = queue[left_index];
    const Observation& second = queue[right_index];
    const double dt = second.timestamp - first.timestamp;
    if (!(dt > kTimestampEpsilon)) {
        const bool use_first = std::abs(query_timestamp - first.timestamp) <=
                               std::abs(query_timestamp - second.timestamp);
        const Observation& nearest = use_first ? first : second;
        result = nearest;
        result.timestamp = query_timestamp;
        delta_t = query_timestamp - nearest.timestamp;
        return true;
    }

    const double alpha = (query_timestamp - first.timestamp) / dt;
    result = first;
    result.timestamp = query_timestamp;
    result.pose = InterpolatePose(first.pose, second.pose, alpha);
    result.velocity = first.velocity + alpha * (second.velocity - first.velocity);
    result.valid = first.valid && second.valid;
    result.reliable = first.reliable && second.reliable;
    result.confidence = std::min(first.confidence, second.confidence);
    result.information = alpha <= 0.5 ? first.information : second.information;

    const double first_delta = std::abs(query_timestamp - first.timestamp);
    const double second_delta = std::abs(query_timestamp - second.timestamp);
    const Observation& nearest = first_delta <= second_delta ? first : second;
    delta_t = query_timestamp - nearest.timestamp;
    return true;
}

Information6d AverageInformation(const Information6d& first,
                                  const Information6d& second) {
    return SymmetrizeInformation(0.5 * (first + second));
}

}  // namespace

Information6d DefaultLidarLocalizationInformation() {
    Information6d information = Information6d::Zero();
    const double angle_noise = 1.0 * kPi / 180.0;
    for (int index = 0; index < 3; ++index) {
        information(index, index) = 1.0 / (0.3 * 0.3);
        information(index + 3, index + 3) =
            1.0 / (angle_noise * angle_noise);
    }
    return information;
}

Information6d DefaultLidarOdomInformation() {
    return DefaultLidarLocalizationInformation();
}

Information6d DefaultDeadReckoningInformation() {
    Information6d information = Information6d::Zero();
    const double angle_noise = 0.5 * kPi / 180.0;
    for (int index = 0; index < 3; ++index) {
        information(index, index) = 1.0;
        information(index + 3, index + 3) =
            1.0 / (angle_noise * angle_noise);
    }
    return information;
}

Information6d InformationFromCovariance(const Information6d& covariance,
                                         const Information6d& fallback) {
    if (!covariance.allFinite()) {
        return SymmetrizeInformation(fallback);
    }

    const Information6d symmetric =
        0.5 * (covariance + covariance.transpose());
    for (int index = 0; index < 6; ++index) {
        if (symmetric(index, index) <= kInformationEpsilon ||
            !std::isfinite(symmetric(index, index))) {
            return SymmetrizeInformation(fallback);
        }
    }

    Eigen::LDLT<Information6d> decomposition(symmetric);
    if (decomposition.info() != Eigen::Success) {
        return SymmetrizeInformation(fallback);
    }
    const Information6d information = decomposition.solve(Information6d::Identity());
    if (!IsUsableInformation(information)) {
        return SymmetrizeInformation(fallback);
    }
    return SymmetrizeInformation(information);
}

PoseGraphFusion::PoseGraphFusion(IPoseGraphBackend& backend,
                                 PGOFusionConfig config)
    : backend_(backend), config_(std::move(config)) {
    config_.window_size = std::max<std::size_t>(1, config_.window_size);
    config_.max_relative_constraints =
        std::max<std::size_t>(1, config_.max_relative_constraints);
    config_.max_motion_queue_size =
        std::max<std::size_t>(1, config_.max_motion_queue_size);
    config_.max_output_history_size =
        std::max<std::size_t>(1, config_.max_output_history_size);
    config_.max_interpolation_gap =
        std::max(0.0, config_.max_interpolation_gap);
    config_.min_localization_confidence = std::clamp(
        config_.min_localization_confidence, 0.01, 1.0);
    if (!std::isfinite(config_.localization_information_scale) ||
        config_.localization_information_scale <= 0.0) {
        config_.localization_information_scale = 1.0;
    }
    if (!std::isfinite(config_.localization_confidence_power) ||
        config_.localization_confidence_power < 0.0) {
        config_.localization_confidence_power = 1.0;
    }
    config_.min_localization_information_scale = std::clamp(
        config_.min_localization_information_scale, 1e-4, 100.0);
    config_.max_localization_information_scale = std::max(
        config_.min_localization_information_scale,
        std::isfinite(config_.max_localization_information_scale)
            ? config_.max_localization_information_scale
            : 4.0);
    config_.smoothing_alpha = std::clamp(config_.smoothing_alpha, 0.0, 1.0);
}

void PoseGraphFusion::Reset() {
    backend_.Reset();
    lidar_odom_queue_.clear();
    dead_reckoning_queue_.clear();
    frames_.clear();
    output_history_.clear();
    frame_ids_.clear();
    stats_ = Stats{};
    next_frame_id_ = 0;
    last_localization_timestamp_ = -1.0;
    smoothed_pose_ = Pose3d::Identity();
    smoothed_pose_valid_ = false;
}

bool PoseGraphFusion::AddMotionObservation(
    std::deque<TimedMotionObservation>& queue,
    TimedMotionObservation observation, const Information6d& default_information,
    std::size_t& count) {
    if (!std::isfinite(observation.timestamp) ||
        !IsFinitePose(observation.pose)) {
        return false;
    }
    observation.information = ResolveInformation(
        observation.information, default_information);
    observation.confidence = std::isfinite(observation.confidence)
                                 ? observation.confidence
                                 : 0.0;

    if (!queue.empty()) {
        const double delta = observation.timestamp - queue.back().timestamp;
        if (delta < -kTimestampEpsilon) {
            ++stats_.rejected_timestamp_count;
            return false;
        }
        if (std::abs(delta) <= kTimestampEpsilon) {
            queue.back() = std::move(observation);
            ++count;
            return true;
        }
    }

    queue.push_back(std::move(observation));
    while (queue.size() > config_.max_motion_queue_size) {
        queue.pop_front();
    }
    ++count;
    return true;
}

bool PoseGraphFusion::AddLidarOdomObservation(
    const LidarOdomObservation& observation) {
    return AddMotionObservation(lidar_odom_queue_, observation,
                                config_.lidar_odom_information,
                                stats_.lidar_odom_observations);
}

bool PoseGraphFusion::AddDeadReckoningObservation(
    const DeadReckoningObservation& observation) {
    return AddMotionObservation(dead_reckoning_queue_, observation,
                                config_.dead_reckoning_information,
                                stats_.dead_reckoning_observations);
}

bool PoseGraphFusion::AlignMotionObservation(
    const std::deque<TimedMotionObservation>& queue, double timestamp,
    TimedMotionObservation& result, double& delta_t) const {
    return InterpolateMotionQueue(queue, timestamp,
                                  config_.max_interpolation_gap, result,
                                  delta_t);
}

PoseGraphFusion::RelativeSource PoseGraphFusion::SelectRelativeSource(
    const PGOFrame& previous, const PGOFrame& current) const {
    const bool lidar_odom_available =
        previous.lidar_odom_valid && current.lidar_odom_valid &&
        previous.lidar_odom_reliable && current.lidar_odom_reliable;
    if (lidar_odom_available) {
        return RelativeSource::LidarOdom;
    }

    const bool dead_reckoning_available =
        previous.dr_valid && current.dr_valid && previous.dr_reliable &&
        current.dr_reliable;
    if (dead_reckoning_available) {
        return RelativeSource::DeadReckoning;
    }

    // A producer may mark LO as unreliable while still providing the only
    // continuous pose. It remains a last-resort source rather than silently
    // discarding the frame.
    if (previous.lidar_odom_valid && current.lidar_odom_valid) {
        return RelativeSource::LidarOdom;
    }
    if (previous.dr_valid && current.dr_valid) {
        return RelativeSource::DeadReckoning;
    }
    return RelativeSource::None;
}

void PoseGraphFusion::UpdateSmoothedPose(const Pose3d& optimized_pose) {
    if (!smoothed_pose_valid_) {
        smoothed_pose_ = optimized_pose;
        smoothed_pose_valid_ = true;
        return;
    }

    const double alpha = config_.smoothing_alpha;
    const Eigen::Quaterniond current_rotation(smoothed_pose_.rotation);
    const Eigen::Quaterniond target_rotation(optimized_pose.rotation);
    smoothed_pose_.rotation =
        current_rotation.slerp(alpha, target_rotation).normalized().toRotationMatrix();
    smoothed_pose_.translation =
        (1.0 - alpha) * smoothed_pose_.translation +
        alpha * optimized_pose.translation;
}

bool PoseGraphFusion::ProcessLidarLocalization(
    const LidarLocalizationObservation& observation) {
    ++stats_.lidar_localization_observations;
    if (!observation.valid || !std::isfinite(observation.timestamp) ||
        !IsFinitePose(observation.pose)) {
        return false;
    }
    if (last_localization_timestamp_ >= 0.0 &&
        observation.timestamp < last_localization_timestamp_ -
                                     kTimestampEpsilon) {
        ++stats_.rejected_timestamp_count;
        return false;
    }
    if (next_frame_id_ == std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    TimedMotionObservation lidar_odom;
    TimedMotionObservation dead_reckoning;
    double lidar_odom_delta_t = 0.0;
    double dead_reckoning_delta_t = 0.0;
    const bool has_lidar_odom = AlignMotionObservation(
        lidar_odom_queue_, observation.timestamp, lidar_odom,
        lidar_odom_delta_t);
    const bool has_dead_reckoning = AlignMotionObservation(
        dead_reckoning_queue_, observation.timestamp, dead_reckoning,
        dead_reckoning_delta_t);
    if (!frames_.empty() && config_.require_relative_after_first &&
        !has_lidar_odom && !has_dead_reckoning) {
        ++stats_.rejected_alignment_count;
        return false;
    }

    PGOFrame frame;
    frame.id = next_frame_id_++;
    frame.timestamp = observation.timestamp;
    frame.opti_pose = observation.pose;
    frame.last_opti_pose = observation.pose;
    frame.lidar_loc_set = true;
    frame.lidar_loc_valid = true;
    frame.lidar_loc_pose = observation.pose;
    Information6d localization_information = ResolveInformation(
        observation.information, config_.lidar_localization_information);
    if (config_.calibrate_localization_information) {
        localization_information = CalibrateLocalizationInformation(
            localization_information, config_.lidar_localization_information);
    }
    frame.lidar_loc_information = ScaleByConfidence(
        localization_information, observation.confidence,
        config_.min_localization_confidence,
        config_.localization_information_scale,
        config_.localization_confidence_power,
        config_.min_localization_information_scale,
        config_.max_localization_information_scale);
    frame.confidence = observation.confidence;

    if (has_lidar_odom) {
        frame.lidar_odom_set = true;
        frame.lidar_odom_valid = lidar_odom.valid;
        frame.lidar_odom_reliable = lidar_odom.reliable;
        frame.lidar_odom_pose = lidar_odom.pose;
        frame.lidar_odom_information = *lidar_odom.information;
        frame.lidar_odom_velocity = lidar_odom.velocity;
        frame.lidar_odom_delta_t = lidar_odom_delta_t;
    }
    if (has_dead_reckoning) {
        frame.dr_set = true;
        frame.dr_valid = dead_reckoning.valid;
        frame.dr_reliable = dead_reckoning.reliable;
        frame.dr_pose = dead_reckoning.pose;
        frame.dr_information = *dead_reckoning.information;
        frame.dr_velocity = dead_reckoning.velocity;
        frame.dr_delta_t = dead_reckoning_delta_t;
    }

    Pose3d odom_pose = observation.pose;
    if (frame.lidar_odom_valid && frame.lidar_odom_reliable) {
        odom_pose = frame.lidar_odom_pose;
    } else if (frame.dr_valid && frame.dr_reliable) {
        odom_pose = frame.dr_pose;
    } else if (frame.lidar_odom_valid) {
        odom_pose = frame.lidar_odom_pose;
    } else if (frame.dr_valid) {
        odom_pose = frame.dr_pose;
    }

    Keyframe keyframe;
    keyframe.id = frame.id;
    keyframe.timestamp = frame.timestamp;
    keyframe.T_odom_body = odom_pose;
    keyframe.T_map_body = frame.opti_pose;
    keyframe.covariance =
        InformationFromCovariance(frame.lidar_loc_information,
                                  config_.lidar_localization_information);
    backend_.AddKeyframe(keyframe);

    std::size_t relative_count = 0;
    for (auto iterator = frames_.rbegin(); iterator != frames_.rend() &&
                                            relative_count <
                                                config_.max_relative_constraints;
         ++iterator) {
        const PGOFrame& previous = *iterator;
        const RelativeSource source = SelectRelativeSource(previous, frame);
        if (source == RelativeSource::LidarOdom) {
            const Pose3d measurement = RelativePose(
                previous.lidar_odom_pose, frame.lidar_odom_pose);
            const Information6d information = AverageInformation(
                previous.lidar_odom_information,
                frame.lidar_odom_information);
            backend_.AddOdometryFactor(previous.id, frame.id, measurement,
                                       information);
            frame.used_lidar_odom_relative = true;
            ++stats_.lidar_odom_relative_factor_count;
        } else if (source == RelativeSource::DeadReckoning) {
            const Pose3d measurement =
                RelativePose(previous.dr_pose, frame.dr_pose);
            const Information6d information =
                AverageInformation(previous.dr_information,
                                   frame.dr_information);
            backend_.AddDeadReckoningFactor(previous.id, frame.id,
                                             measurement, information);
            frame.used_dead_reckoning_relative = true;
            ++stats_.dead_reckoning_relative_factor_count;
        } else {
            continue;
        }
        ++relative_count;
    }
    frame.relative_factor_count = relative_count;
    stats_.relative_factor_count += relative_count;

    backend_.AddLocalizationPrior(frame.id, frame.lidar_loc_pose,
                                  frame.lidar_loc_information);
    ++stats_.localization_prior_count;
    if (!backend_.Optimize()) {
        return false;
    }

    frame.last_opti_pose = frame.opti_pose;
    frame.opti_pose = backend_.GetOptimizedPose(frame.id);
    if (!IsFinitePose(frame.opti_pose)) {
        return false;
    }
    ++frame.optimization_count;
    UpdateSmoothedPose(frame.opti_pose);

    frames_.push_back(frame);
    while (frames_.size() > config_.window_size) {
        frames_.pop_front();
    }
    frame_ids_.push_back(frame.id);
    last_localization_timestamp_ = observation.timestamp;
    ++stats_.accepted_frames;
    ++stats_.optimization_count;

    PGOOutput output;
    output.timestamp = frame.timestamp;
    output.pose = smoothed_pose_;
    output.source = PGOOutputSource::Optimized;
    output.valid = true;
    output_history_.push_back(output);
    while (output_history_.size() > config_.max_output_history_size) {
        output_history_.pop_front();
    }
    return true;
}

bool PoseGraphFusion::KnownFrame(std::uint32_t id) const {
    return std::find(frame_ids_.begin(), frame_ids_.end(), id) !=
           frame_ids_.end();
}

bool PoseGraphFusion::AddLoopFactor(std::uint32_t from, std::uint32_t to,
                                    const Pose3d& measurement,
                                    const Information6d& information) {
    if (from == to || !KnownFrame(from) || !KnownFrame(to) ||
        !IsFinitePose(measurement) || !IsUsableInformation(information)) {
        return false;
    }
    backend_.AddLoopFactor(from, to, measurement,
                           SymmetrizeInformation(information));
    if (!backend_.Optimize()) {
        return false;
    }

    for (PGOFrame& frame : frames_) {
        const Pose3d optimized = backend_.GetOptimizedPose(frame.id);
        frame.last_opti_pose = frame.opti_pose;
        frame.opti_pose = optimized;
        ++frame.optimization_count;
    }
    if (!frames_.empty()) {
        UpdateSmoothedPose(frames_.back().opti_pose);
        PGOOutput output;
        output.timestamp = frames_.back().timestamp;
        output.pose = smoothed_pose_;
        output.source = PGOOutputSource::Optimized;
        output.valid = true;
        output_history_.push_back(output);
        while (output_history_.size() > config_.max_output_history_size) {
            output_history_.pop_front();
        }
    }
    ++stats_.optimization_count;
    return true;
}

bool PoseGraphFusion::QueryMotionDelta(const PGOFrame& frame,
                                       double timestamp, Pose3d& delta,
                                       PGOOutputSource& source) const {
    TimedMotionObservation aligned;
    double ignored_delta_t = 0.0;
    if (frame.lidar_odom_valid && frame.lidar_odom_reliable &&
        AlignMotionObservation(lidar_odom_queue_, timestamp, aligned,
                                ignored_delta_t) &&
        aligned.valid) {
        delta = RelativePose(frame.lidar_odom_pose, aligned.pose);
        source = PGOOutputSource::LidarOdomExtrapolation;
        return true;
    }
    if (frame.dr_valid && frame.dr_reliable &&
        AlignMotionObservation(dead_reckoning_queue_, timestamp, aligned,
                                ignored_delta_t) &&
        aligned.valid) {
        delta = RelativePose(frame.dr_pose, aligned.pose);
        source = PGOOutputSource::DeadReckoningExtrapolation;
        return true;
    }
    if (frame.lidar_odom_valid &&
        AlignMotionObservation(lidar_odom_queue_, timestamp, aligned,
                               ignored_delta_t) &&
        aligned.valid) {
        delta = RelativePose(frame.lidar_odom_pose, aligned.pose);
        source = PGOOutputSource::LidarOdomExtrapolation;
        return true;
    }
    if (frame.dr_valid &&
        AlignMotionObservation(dead_reckoning_queue_, timestamp, aligned,
                               ignored_delta_t) &&
        aligned.valid) {
        delta = RelativePose(frame.dr_pose, aligned.pose);
        source = PGOOutputSource::DeadReckoningExtrapolation;
        return true;
    }
    return false;
}

bool PoseGraphFusion::QueryFusedPose(double timestamp,
                                     PGOOutput& output) const {
    if (!std::isfinite(timestamp) || frames_.empty() ||
        !smoothed_pose_valid_) {
        return false;
    }

    const PGOFrame& frame = frames_.back();
    output = PGOOutput{};
    output.timestamp = timestamp;
    output.valid = true;
    output.pose = smoothed_pose_;
    output.source = PGOOutputSource::Optimized;
    output.extrapolated = false;
    if (std::abs(timestamp - frame.timestamp) <= kTimestampEpsilon) {
        return true;
    }

    Pose3d delta;
    PGOOutputSource source = PGOOutputSource::Optimized;
    if (!QueryMotionDelta(frame, timestamp, delta, source)) {
        return false;
    }
    output.pose = Compose(smoothed_pose_, delta);
    output.source = source;
    output.extrapolated = timestamp > frame.timestamp;
    return IsFinitePose(output.pose);
}

const PGOFrame* PoseGraphFusion::CurrentFrame() const {
    return frames_.empty() ? nullptr : &frames_.back();
}

std::vector<PGOFrame> PoseGraphFusion::WindowSnapshot() const {
    return std::vector<PGOFrame>(frames_.begin(), frames_.end());
}

}  // namespace x86_lio_slam
