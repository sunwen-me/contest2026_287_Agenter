#pragma once

#include "x86_lio_slam/backend/pose_graph_backend.h"

#include <Eigen/Core>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace x86_lio_slam {

using Information6d = Eigen::Matrix<double, 6, 6>;

Information6d DefaultLidarLocalizationInformation();
Information6d DefaultLidarOdomInformation();
Information6d DefaultDeadReckoningInformation();

// A timestamped pose from a relative-motion source. The pose is expressed in
// that source's continuous odometry frame. Information is optional so the
// fusion layer can apply its configured source noise when a producer has no
// covariance estimate.
struct TimedMotionObservation {
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    double timestamp = 0.0;
    Pose3d pose{};
    std::optional<Information6d> information;
    Eigen::Vector3d velocity = Eigen::Vector3d::Zero();
    bool valid = true;
    bool reliable = true;
    double confidence = 1.0;
};

using LidarOdomObservation = TimedMotionObservation;
using DeadReckoningObservation = TimedMotionObservation;

struct LidarLocalizationObservation {
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    double timestamp = 0.0;
    Pose3d pose{};
    std::optional<Information6d> information;
    bool valid = true;
    double confidence = 1.0;
};

// The frame is deliberately a value type. It records the aligned source
// observations used to construct one graph vertex, not a pointer into a ROS
// message or an upstream Lightning-LM object.
struct PGOFrame {
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    std::uint32_t id = 0;
    double timestamp = 0.0;
    Pose3d opti_pose{};
    Pose3d last_opti_pose{};
    std::size_t optimization_count = 0;

    bool lidar_loc_set = false;
    bool lidar_loc_valid = false;
    Pose3d lidar_loc_pose{};
    Information6d lidar_loc_information = Information6d::Identity();
    double lidar_loc_delta_t = 0.0;
    double confidence = 0.0;

    bool lidar_odom_set = false;
    bool lidar_odom_valid = false;
    bool lidar_odom_reliable = false;
    Pose3d lidar_odom_pose{};
    Information6d lidar_odom_information = Information6d::Identity();
    Eigen::Vector3d lidar_odom_velocity = Eigen::Vector3d::Zero();
    double lidar_odom_delta_t = 0.0;

    bool dr_set = false;
    bool dr_valid = false;
    bool dr_reliable = false;
    Pose3d dr_pose{};
    Information6d dr_information = Information6d::Identity();
    Eigen::Vector3d dr_velocity = Eigen::Vector3d::Zero();
    double dr_delta_t = 0.0;

    // This flag describes the virtual prior maintained by the graph backend
    // for the active oldest frame. The adapter owns its actual EdgeSE3Prior.
    bool prior_set = false;
    bool prior_valid = false;
    Pose3d prior_pose{};
    Information6d prior_information = Information6d::Identity();

    std::size_t relative_factor_count = 0;
    bool used_lidar_odom_relative = false;
    bool used_dead_reckoning_relative = false;
};

enum class PGOOutputSource {
    Optimized,
    LidarOdomExtrapolation,
    DeadReckoningExtrapolation,
};

struct PGOOutput {
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    double timestamp = 0.0;
    Pose3d pose{};
    PGOOutputSource source = PGOOutputSource::Optimized;
    bool valid = false;
    bool extrapolated = false;
};

struct PGOFusionConfig {
    std::size_t window_size = 5;
    std::size_t max_relative_constraints = 5;
    std::size_t max_motion_queue_size = 10000;
    std::size_t max_output_history_size = 1000;
    double max_interpolation_gap = 0.5;
    double min_localization_confidence = 0.5;
    bool calibrate_localization_information = true;
    double localization_information_scale = 1.0;
    double localization_confidence_power = 1.0;
    double min_localization_information_scale = 0.05;
    double max_localization_information_scale = 4.0;
    double smoothing_alpha = 1.0;
    bool require_relative_after_first = true;

    Information6d lidar_localization_information =
        DefaultLidarLocalizationInformation();
    Information6d lidar_odom_information = DefaultLidarOdomInformation();
    Information6d dead_reckoning_information =
        DefaultDeadReckoningInformation();
};

class PoseGraphFusion final {
public:
    struct Stats {
        std::size_t lidar_odom_observations = 0;
        std::size_t dead_reckoning_observations = 0;
        std::size_t lidar_localization_observations = 0;
        std::size_t accepted_frames = 0;
        std::size_t optimization_count = 0;
        std::size_t relative_factor_count = 0;
        std::size_t lidar_odom_relative_factor_count = 0;
        std::size_t dead_reckoning_relative_factor_count = 0;
        std::size_t localization_prior_count = 0;
        std::size_t rejected_timestamp_count = 0;
        std::size_t rejected_alignment_count = 0;
    };

    explicit PoseGraphFusion(IPoseGraphBackend& backend,
                             PGOFusionConfig config = PGOFusionConfig{});

    void Reset();

    bool AddLidarOdomObservation(const LidarOdomObservation& observation);

    bool AddDeadReckoningObservation(
        const DeadReckoningObservation& observation);

    // A valid localization observation creates a PGOFrame and triggers one
    // graph optimization. Motion observations may arrive before or after it,
    // provided they remain within max_interpolation_gap.
    bool ProcessLidarLocalization(
        const LidarLocalizationObservation& observation);

    bool AddLoopFactor(std::uint32_t from, std::uint32_t to,
                       const Pose3d& measurement,
                       const Information6d& information);

    bool QueryFusedPose(double timestamp, PGOOutput& output) const;

    const PGOFrame* CurrentFrame() const;

    std::vector<PGOFrame> WindowSnapshot() const;

    Stats GetStats() const { return stats_; }

    std::size_t WindowSize() const { return frames_.size(); }

private:
    enum class RelativeSource { None, LidarOdom, DeadReckoning };

    bool AddMotionObservation(std::deque<TimedMotionObservation>& queue,
                              TimedMotionObservation observation,
                              const Information6d& default_information,
                              std::size_t& count);

    bool AlignMotionObservation(
        const std::deque<TimedMotionObservation>& queue, double timestamp,
        TimedMotionObservation& result, double& delta_t) const;

    RelativeSource SelectRelativeSource(const PGOFrame& previous,
                                        const PGOFrame& current) const;

    void UpdateSmoothedPose(const Pose3d& optimized_pose);

    bool QueryMotionDelta(const PGOFrame& frame, double timestamp,
                          Pose3d& delta, PGOOutputSource& source) const;

    bool KnownFrame(std::uint32_t id) const;

    IPoseGraphBackend& backend_;
    PGOFusionConfig config_;
    std::deque<TimedMotionObservation> lidar_odom_queue_;
    std::deque<TimedMotionObservation> dead_reckoning_queue_;
    std::deque<PGOFrame> frames_;
    std::deque<PGOOutput> output_history_;
    std::vector<std::uint32_t> frame_ids_;
    Stats stats_{};
    std::uint32_t next_frame_id_ = 0;
    double last_localization_timestamp_ = -1.0;
    Pose3d smoothed_pose_{};
    bool smoothed_pose_valid_ = false;
};

// Converts a covariance published by the frontend into a bounded information
// matrix suitable for a graph factor. Invalid or singular covariance falls
// back to the supplied diagonal information.
Information6d InformationFromCovariance(const Information6d& covariance,
                                         const Information6d& fallback);

}  // namespace x86_lio_slam
