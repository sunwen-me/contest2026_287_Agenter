#pragma once

#include "x86_lio_slam/backend/pgo_fusion.h"
#include "x86_lio_slam/backend/pose_graph_backend.h"
#include "x86_lio_slam/common/config.h"
#include "x86_lio_slam/frontend/lio_frontend.h"
#include "x86_lio_slam/keyframe/keyframe_manager.h"
#include "x86_lio_slam/loop/loop_closure.h"
#include "x86_lio_slam/localization/lidar_localizer.h"
#include "x86_lio_slam/map/global_map.h"
#include "x86_lio_slam/system/map_odom.h"

#include <Eigen/Core>

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_set>
#include <vector>

namespace x86_lio_slam {

class NavigationSnapshotPublisher;

struct SlamSystemConfig {
    KeyframeConfig keyframes{};
    LoopConfig loop{};
    GlobalMapConfig global_map{};
    Eigen::Matrix<double, 6, 6> odometry_information =
        Eigen::Matrix<double, 6, 6>::Identity() * 100.0;
    Eigen::Matrix<double, 6, 6> loop_information =
        Eigen::Matrix<double, 6, 6>::Identity() * 50.0;
    double map_odom_smoothing = 1.0;
};

class SlamSystem {
public:
    SlamSystem();

    SlamSystem(ILioFrontend& frontend, IPoseGraphBackend& backend,
               SlamSystemConfig config = SlamSystemConfig{});

    void Reset();

    void AddImu(const ImuSample& imu) { frontend_->AddImu(imu); }

    void AddPointCloud(std::span<const PointXYZIT> cloud) {
        frontend_->AddPointCloud(cloud);
    }

    bool Process(OdometryState& output);

    // Attach the optional Lightning-LM-style multi-source fusion path. The
    // fusion object owns a separate, exclusive pose-graph backend; the
    // existing keyframe/map backend remains responsible for map correction.
    void AttachPoseGraphFusion(PoseGraphFusion& fusion) {
        pose_graph_fusion_ = &fusion;
    }

    // The localizer owns its immutable map index. SetMap() must be called on
    // the attached object before processing clouds.
    void AttachLidarLocalizer(LidarLocalizer& localizer) {
        lidar_localizer_ = &localizer;
    }

    // Publish a compact, versioned map/pose snapshot after Process(). The
    // publisher owns the bounded costmap and never exposes LIO containers to
    // the navigation thread.
    void AttachNavigationSnapshotPublisher(
        NavigationSnapshotPublisher& publisher) {
        navigation_publisher_ = &publisher;
    }

    bool HasNavigationSnapshotPublisher() const {
        return navigation_publisher_ != nullptr;
    }

    bool PublishNavigationSnapshot(NavigationSnapshotPublisher& publisher,
                                   const OdometryState& state) const;

    bool HasPoseGraphFusion() const { return pose_graph_fusion_ != nullptr; }

    bool HasLidarLocalizer() const { return lidar_localizer_ != nullptr; }

    bool AddLidarOdomObservation(
        const LidarOdomObservation& observation);

    bool AddDeadReckoningObservation(
        const DeadReckoningObservation& observation);

    bool ProcessLidarLocalization(
        const LidarLocalizationObservation& observation);

    // Run map localization and, when attached, submit the valid result to the
    // separate multi-source pose graph.
    bool ProcessLidarCloud(double timestamp,
                           std::span<const PointXYZIT> cloud,
                           LidarLocalizationResult& result);

    bool QueryFusedPose(double timestamp, PGOOutput& output) const;

    bool AddManualLoop(std::uint32_t from, std::uint32_t to,
                       const Pose3d& measurement);

    bool AddLocalizationPrior(
        std::uint32_t id, const Pose3d& measurement,
        const Eigen::Matrix<double, 6, 6>& information);

    Pose3d MapBody(const Pose3d& odom_body) const {
        return map_odom_.MapBody(odom_body);
    }

    Pose3d T_map_odom() const { return map_odom_.T_map_odom(); }

    const std::vector<Keyframe>& Keyframes() const {
        return keyframe_manager_.Keyframes();
    }

    const std::vector<Eigen::Vector3f>& GlobalMap() const {
        return global_map_.Points();
    }

    std::uint64_t GlobalMapVersion() const { return global_map_.Version(); }

    std::size_t KeyframeCount() const {
        return keyframe_manager_.Keyframes().size();
    }

    std::size_t AcceptedLoopCount() const { return accepted_loops_.size(); }

    ILioFrontend& Frontend() { return *frontend_; }

    IPoseGraphBackend& Backend() { return *backend_; }

private:
    void AddKeyframeIfNeeded(const OdometryState& state);

    bool SynchronizeGraph();

    void InitializeLoopClosure();

    bool TryAutomaticLoop(std::uint32_t query_id);

    const Keyframe* FindKeyframe(std::uint32_t id) const;

    void UpdateCorrectionAndMap(
        std::span<const std::uint32_t> affected_ids = {});

    std::unique_ptr<ILioFrontend> owned_frontend_;
    std::unique_ptr<IPoseGraphBackend> owned_backend_;
    ILioFrontend* frontend_ = nullptr;
    IPoseGraphBackend* backend_ = nullptr;
    SlamSystemConfig config_{};
    KeyframeManager keyframe_manager_;
    GlobalMapBuilder global_map_;
    MapOdomCorrection map_odom_;
    std::unique_ptr<ILoopDetector> loop_detector_;
    std::unique_ptr<ILoopRegistration> loop_registration_;
    PoseGraphFusion* pose_graph_fusion_ = nullptr;
    LidarLocalizer* lidar_localizer_ = nullptr;
    NavigationSnapshotPublisher* navigation_publisher_ = nullptr;
    std::unordered_set<std::uint64_t> accepted_loops_;
    std::uint64_t last_seen_cloud_id_ = 0;
};

}  // namespace x86_lio_slam
