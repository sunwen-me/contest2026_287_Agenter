#pragma once

#include "x86_lio_slam/backend/pgo_fusion.h"
#include "x86_lio_slam/common/types.h"
#include "x86_lio_slam/map/super_lio_octvox_map.h"

#include <Eigen/Core>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>

namespace x86_lio_slam {

enum class LidarLocalizationStatus {
    Idle,
    Initializing,
    Good,
    FollowingDeadReckoning,
    Fail,
};

// Result of matching one scan against a map. For a successful match,
// information is the observed point-to-plane Hessian; fallback and invalid
// results retain the configured base information. Confidence is kept separate
// so PGO can down-weight a weak match without discarding the observed shape.
struct LidarLocalizationResult {
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    double timestamp = 0.0;
    Pose3d pose{};
    bool valid = false;
    LidarLocalizationStatus status = LidarLocalizationStatus::Idle;
    double confidence = 0.0;
    Information6d information = DefaultLidarLocalizationInformation();

    bool lidar_odom_reliable = true;
    bool lidar_odom_error_normal = true;
    double lidar_odom_consistency_error = 0.0;
    bool smooth = false;
    bool used_lidar_odom_prediction = false;
    bool used_dead_reckoning_prediction = false;
    bool used_global_relocalization = false;

    std::size_t correspondences = 0;
    double rmse = 0.0;
    std::size_t iterations = 0;

    LidarLocalizationObservation ToObservation() const {
        LidarLocalizationObservation observation;
        observation.timestamp = timestamp;
        observation.pose = pose;
        observation.information = information;
        observation.valid = valid;
        observation.confidence = confidence;
        return observation;
    }
};

struct LidarLocalizerConfig {
    float map_resolution = 1.0F;
    std::size_t map_capacity = 1000000;
    std::size_t max_motion_queue_size = 1000;

    std::size_t min_scan_points = 50;
    std::size_t max_scan_points = 4000;
    std::size_t min_correspondences = 20;
    std::size_t max_iterations = 20;

    double max_interpolation_gap = 5.0;
    double lidar_time_offset = 0.0;
    double max_correspondence_distance = 2.0;
    double min_plane_variance = 1e-5;
    double max_planarity_ratio = 0.25;
    double huber_delta = 0.25;
    double confidence_sigma = 0.25;
    double min_confidence = 0.1;
    double convergence_translation = 1e-3;
    double convergence_rotation = 1e-4;
    double lidar_odom_consistency_threshold = 0.3;
    std::size_t unreliable_recovery_frames = 10;
    std::size_t max_consecutive_failures = 100;

    // The first scan is not assumed to be close to the map origin. The
    // bounded coarse search keeps initialization deterministic and portable;
    // it is not intended to replace a database-scale place-recognition index.
    bool enable_global_relocalization = true;
    double global_search_translation_radius = 8.0;
    double global_search_translation_step = 2.0;
    std::size_t global_search_yaw_bins = 24;
    std::size_t global_search_points = 192;
    std::size_t global_search_refine_candidates = 6;
    std::size_t global_search_min_inliers = 20;
    std::size_t global_relocalization_failure_threshold = 3;

    bool force_2d = true;
    bool zero_height = true;
    Information6d localization_information =
        DefaultLidarLocalizationInformation();
};

struct LidarLocalizerStats {
    std::uint64_t map_points_accepted = 0;
    std::uint64_t localization_calls = 0;
    std::uint64_t successful_matches = 0;
    std::uint64_t failed_matches = 0;
    std::uint64_t rejected_timestamps = 0;
    std::uint64_t rejected_scans = 0;
    std::uint64_t global_search_attempts = 0;
    std::uint64_t global_search_successes = 0;
    std::uint64_t global_candidates_evaluated = 0;
    double confidence_sum = 0.0;
    double rmse_sum = 0.0;
};

// Portable map-based lidar localization. The map is immutable during a match
// and can be rebuilt explicitly with SetMap(). It intentionally exposes the
// source observations separately from PoseGraphFusion so the matcher can be
// used with or without a graph backend.
class LidarLocalizer final {
public:
    explicit LidarLocalizer(
        LidarLocalizerConfig config = LidarLocalizerConfig{});

    void Reset();

    // SetMap copies finite points into the compact OctVox-style index. The
    // caller may retain or reuse the input storage after this call.
    bool SetMap(std::span<const Eigen::Vector3f> points);

    void SetInitialPose(const Pose3d& pose);

    // Re-anchor the tracker after an external reset or global localization.
    void ResetLastPose(const Pose3d& pose);

    bool AddLidarOdomObservation(const LidarOdomObservation& observation);

    bool AddDeadReckoningObservation(
        const DeadReckoningObservation& observation);

    bool ProcessCloud(double timestamp, std::span<const PointXYZIT> cloud,
                      LidarLocalizationResult& result);

    bool ProcessCloud(double timestamp,
                      std::span<const Eigen::Vector3f> cloud,
                      LidarLocalizationResult& result);

    bool IsInitialized() const { return initialized_; }

    bool HasMap() const { return map_ready_; }

    std::size_t MapSize() const { return map_.Size(); }

    const LidarLocalizationResult& LastResult() const { return last_result_; }

    const LidarLocalizerStats& Stats() const { return stats_; }

private:
    struct MatchResult {
        Pose3d pose{};
        Information6d information = Information6d::Identity();
        double confidence = 0.0;
        double rmse = 0.0;
        std::size_t correspondences = 0;
        std::size_t iterations = 0;
        bool converged = false;
    };

    struct CoarseCandidate {
        Pose3d pose{};
        double score = 0.0;
        std::size_t inliers = 0;
    };

    bool AddMotionObservation(
        std::deque<TimedMotionObservation>& queue,
        const TimedMotionObservation& observation);

    bool InterpolateMotionObservation(
        const std::deque<TimedMotionObservation>& queue, double timestamp,
        TimedMotionObservation& result) const;

    Pose3d PredictPose(const Pose3d& fallback,
                       const TimedMotionObservation* lidar_odom,
                       const TimedMotionObservation* dead_reckoning,
                       bool& used_lidar_odom,
                       bool& used_dead_reckoning) const;

    bool Match(std::span<const Eigen::Vector3f> scan, const Pose3d& guess,
               MatchResult& result) const;

    bool CoarseScore(std::span<const Eigen::Vector3f> scan,
                     const Pose3d& candidate,
                     CoarseCandidate& result) const;

    bool GlobalMatch(std::span<const Eigen::Vector3f> scan,
                     const Pose3d& seed, MatchResult& result);

    static Pose3d ProjectToPlanar(const Pose3d& pose, bool zero_height,
                                  double height);

    static bool IsFinitePose(const Pose3d& pose);

    LidarLocalizerConfig config_;
    SuperLioOctVoxMap map_;
    bool map_ready_ = false;

    std::deque<TimedMotionObservation> lidar_odom_queue_;
    std::deque<TimedMotionObservation> dead_reckoning_queue_;

    bool initial_pose_set_ = false;
    Pose3d initial_pose_{};
    bool initialized_ = false;
    Pose3d last_pose_{};
    bool last_lidar_odom_set_ = false;
    TimedMotionObservation last_lidar_odom_{};
    bool last_dead_reckoning_set_ = false;
    TimedMotionObservation last_dead_reckoning_{};
    std::size_t match_failures_ = 0;
    std::size_t lidar_odom_unreliable_count_ = 0;
    bool lidar_odom_reliable_ = true;
    double map_height_ = 0.0;
    double last_cloud_timestamp_ = -1.0;

    LidarLocalizationResult last_result_{};
    LidarLocalizerStats stats_{};
};

}  // namespace x86_lio_slam
