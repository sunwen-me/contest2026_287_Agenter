#include "x86_lio_slam/system/slam_system.h"

#include "x86_lio_slam/common/geometry.h"
#include "x86_lio_slam/frontend/small_point_lio_frontend.h"
#if !defined(VELAROS_LIO_TARGET)
#include "x86_lio_slam/navigation/snapshot_publisher.h"
#endif

#include <cmath>
#include <stdexcept>
#include <utility>

namespace x86_lio_slam {

SlamSystem::SlamSystem()
    : owned_frontend_(std::make_unique<SmallPointLioFrontend>()),
      owned_backend_(CreateDefaultPoseGraphBackend()),
      frontend_(owned_frontend_.get()),
      backend_(owned_backend_.get()), keyframe_manager_(config_.keyframes),
      global_map_(config_.global_map) {
    InitializeLoopClosure();
}

SlamSystem::SlamSystem(ILioFrontend& frontend, IPoseGraphBackend& backend,
                       SlamSystemConfig config)
    : frontend_(&frontend), backend_(&backend), config_(std::move(config)),
      keyframe_manager_(config_.keyframes), global_map_(config_.global_map) {
    InitializeLoopClosure();
}

void SlamSystem::Reset() {
    frontend_->Reset();
    backend_->Reset();
    if (pose_graph_fusion_ != nullptr) {
        pose_graph_fusion_->Reset();
    }
    if (lidar_localizer_ != nullptr) {
        lidar_localizer_->Reset();
    }
#if !defined(VELAROS_LIO_TARGET)
    if (navigation_publisher_ != nullptr) {
        navigation_publisher_->Reset();
    }
#endif
    keyframe_manager_.Reset();
    map_odom_.Reset();
    global_map_.Rebuild(std::span<const Keyframe>());
    if (loop_detector_) {
        loop_detector_->Reset();
    }
    accepted_loops_.clear();
    last_seen_cloud_id_ = 0;
}

void SlamSystem::InitializeLoopClosure() {
    loop_detector_ = std::make_unique<RadiusLoopDetector>(config_.loop);
    loop_registration_ = std::make_unique<CentroidLoopRegistration>(
        config_.loop.correspondence_radius,
        config_.loop.registration_iterations);
}

void SlamSystem::AddKeyframeIfNeeded(const OdometryState& state) {
    const std::uint64_t cloud_id = frontend_->LastCloudId();
    if (cloud_id == 0 || cloud_id == last_seen_cloud_id_ ||
        !keyframe_manager_.ShouldCreate(state)) {
        return;
    }

    const Keyframe* previous = keyframe_manager_.Last();
    const std::uint32_t previous_id = previous == nullptr ? 0 : previous->id;
    const Pose3d previous_odom =
        previous == nullptr ? Pose3d::Identity() : previous->T_odom_body;
    Keyframe keyframe =
        keyframe_manager_.Create(state, frontend_->LastCloudBody());
    // New nodes must start in the current map frame after a loop correction;
    // initializing them directly from odom would inject a frame jump into the
    // pose graph on the next keyframe.
    keyframe.T_map_body = map_odom_.MapBody(keyframe.T_odom_body);
    keyframe_manager_.UpdateMapPose(keyframe.id, keyframe.T_map_body);
    backend_->AddKeyframe(keyframe);

    if (previous != nullptr) {
        const Pose3d measurement = RelativePose(previous_odom,
                                                 keyframe.T_odom_body);
        backend_->AddOdometryFactor(previous_id, keyframe.id, measurement,
                                    config_.odometry_information);
        if (!backend_->Optimize()) {
            throw std::runtime_error("pose graph odometry optimization failed");
        }
        SynchronizeGraph();
    } else {
        UpdateCorrectionAndMap();
    }
    last_seen_cloud_id_ = cloud_id;
    TryAutomaticLoop(keyframe.id);
}

bool SlamSystem::Process(OdometryState& output) {
    const bool processed = frontend_->Process(output);
    if (!processed || !output.valid) {
        return false;
    }
    LidarOdomObservation lidar_odom;
    lidar_odom.timestamp = output.timestamp;
    lidar_odom.pose = output.T_odom_body;
    lidar_odom.velocity = output.velocity;
    lidar_odom.information = InformationFromCovariance(
        output.pose_covariance, DefaultLidarOdomInformation());
    lidar_odom.valid = output.valid;
    lidar_odom.reliable = true;
    if (pose_graph_fusion_ != nullptr) {
        (void)pose_graph_fusion_->AddLidarOdomObservation(lidar_odom);
    }
    if (lidar_localizer_ != nullptr) {
        (void)lidar_localizer_->AddLidarOdomObservation(lidar_odom);
    }
    AddKeyframeIfNeeded(output);
#if !defined(VELAROS_LIO_TARGET)
    if (navigation_publisher_ != nullptr) {
        (void)PublishNavigationSnapshot(*navigation_publisher_, output);
    }
#endif
    return true;
}

#if !defined(VELAROS_LIO_TARGET)
bool SlamSystem::PublishNavigationSnapshot(
    NavigationSnapshotPublisher& publisher, const OdometryState& state) const {
    if (!state.valid || !std::isfinite(state.timestamp) ||
        !state.T_odom_body.rotation.allFinite() ||
        !state.T_odom_body.translation.allFinite() ||
        !state.velocity.allFinite() || !state.angular_velocity.allFinite()) {
        return false;
    }
    const Pose3d map_body = map_odom_.MapBody(state.T_odom_body);
    const Pose3d map_odom = map_odom_.T_map_odom();
    PoseSnapshot snapshot;
    snapshot.timestamp = state.timestamp;
    snapshot.frame = publisher.Costmap().Config().frame;
    snapshot.T_map_body = map_body;
    snapshot.planar_velocity = (map_odom.rotation * state.velocity).head<2>();
    snapshot.yaw_rate = state.angular_velocity.z();
    snapshot.covariance = state.pose_covariance.topLeftCorner<3, 3>();
    snapshot.covariance_valid = state.pose_covariance.allFinite();
    snapshot.valid = IsFinite(snapshot);
    return snapshot.valid && publisher.Publish(
                               snapshot,
                               std::span<const Eigen::Vector3f>(
                                   global_map_.Points().data(),
                                   global_map_.Points().size()),
                               global_map_.Version());
}
#endif

bool SlamSystem::AddLidarOdomObservation(
    const LidarOdomObservation& observation) {
    bool accepted = false;
    if (pose_graph_fusion_ != nullptr) {
        accepted = pose_graph_fusion_->AddLidarOdomObservation(observation);
    }
    if (lidar_localizer_ != nullptr) {
        const bool localizer_accepted =
            lidar_localizer_->AddLidarOdomObservation(observation);
        accepted = localizer_accepted || accepted;
    }
    return accepted;
}

bool SlamSystem::AddDeadReckoningObservation(
    const DeadReckoningObservation& observation) {
    bool accepted = false;
    if (pose_graph_fusion_ != nullptr) {
        accepted =
            pose_graph_fusion_->AddDeadReckoningObservation(observation);
    }
    if (lidar_localizer_ != nullptr) {
        const bool localizer_accepted =
            lidar_localizer_->AddDeadReckoningObservation(observation);
        accepted = localizer_accepted || accepted;
    }
    return accepted;
}

bool SlamSystem::ProcessLidarLocalization(
    const LidarLocalizationObservation& observation) {
    return pose_graph_fusion_ != nullptr &&
           pose_graph_fusion_->ProcessLidarLocalization(observation);
}

bool SlamSystem::ProcessLidarCloud(
    double timestamp, std::span<const PointXYZIT> cloud,
    LidarLocalizationResult& result) {
    if (lidar_localizer_ == nullptr) {
        return false;
    }
    if (!lidar_localizer_->ProcessCloud(timestamp, cloud, result)) {
        return false;
    }
    return pose_graph_fusion_ == nullptr ||
           pose_graph_fusion_->ProcessLidarLocalization(
               result.ToObservation());
}

bool SlamSystem::QueryFusedPose(double timestamp, PGOOutput& output) const {
    return pose_graph_fusion_ != nullptr &&
           pose_graph_fusion_->QueryFusedPose(timestamp, output);
}

bool SlamSystem::SynchronizeGraph() {
    std::vector<std::uint32_t> affected_ids;
    for (Keyframe& keyframe : keyframe_manager_.MutableKeyframes()) {
        const Pose3d optimized_pose = backend_->GetOptimizedPose(keyframe.id);
        const Pose3d delta = RelativePose(keyframe.T_map_body, optimized_pose);
        if (delta.translation.norm() > 1e-10 ||
            RotationDistance(delta.rotation) > 1e-10) {
            affected_ids.push_back(keyframe.id);
        }
        keyframe.T_map_body = optimized_pose;
    }
    UpdateCorrectionAndMap(std::span<const std::uint32_t>(
        affected_ids.data(), affected_ids.size()));
    return true;
}

const Keyframe* SlamSystem::FindKeyframe(std::uint32_t id) const {
    for (const Keyframe& keyframe : keyframe_manager_.Keyframes()) {
        if (keyframe.id == id) {
            return &keyframe;
        }
    }
    return nullptr;
}

bool SlamSystem::TryAutomaticLoop(std::uint32_t query_id) {
    if (!config_.loop.enabled || !loop_detector_ || !loop_registration_) {
        return false;
    }

    const Keyframe* query = FindKeyframe(query_id);
    if (query == nullptr) {
        return false;
    }

    const std::vector<LoopCandidate> candidates = loop_detector_->Detect(*query);
    for (const LoopCandidate& candidate : candidates) {
        const std::uint64_t loop_key =
            (static_cast<std::uint64_t>(candidate.target_id) << 32U) |
            static_cast<std::uint64_t>(candidate.query_id);
        if (accepted_loops_.contains(loop_key)) {
            continue;
        }

        const Keyframe* target = FindKeyframe(candidate.target_id);
        if (target == nullptr) {
            continue;
        }
        const LoopRegistrationResult registration =
            loop_registration_->Register(*query, *target);
        if (!registration.converged ||
            !registration.T_target_query.rotation.allFinite() ||
            !registration.T_target_query.translation.allFinite() ||
            !std::isfinite(registration.fitness) ||
            registration.fitness > config_.loop.max_fitness ||
            registration.inliers < config_.loop.min_inliers) {
            continue;
        }

        // Registration returns T_target_query. miao and the dense backend
        // expect the same measurement on an edge target -> query.
        const Pose3d odometry_measurement =
            RelativePose(target->T_odom_body, query->T_odom_body);
        const Pose3d correction =
            RelativePose(odometry_measurement, registration.T_target_query);
        if (correction.translation.norm() >
                config_.loop.max_translation_correction ||
            RotationDistance(correction.rotation) >
                config_.loop.max_rotation_correction) {
            continue;
        }

        backend_->AddLoopFactor(target->id, query->id,
                                registration.T_target_query,
                                config_.loop_information);
        if (!backend_->Optimize()) {
            return false;
        }
        accepted_loops_.insert(loop_key);
        return SynchronizeGraph();
    }
    return false;
}

void SlamSystem::UpdateCorrectionAndMap(
    std::span<const std::uint32_t> affected_ids) {
    const Keyframe* anchor = keyframe_manager_.Last();
    if (anchor != nullptr) {
        map_odom_.Update(anchor->T_map_body, anchor->T_odom_body,
                         config_.map_odom_smoothing);
    }
    const auto& keyframes = keyframe_manager_.Keyframes();
    global_map_.UpdateAffected(
        std::span<const Keyframe>(keyframes.data(), keyframes.size()),
        affected_ids);
}

bool SlamSystem::AddManualLoop(std::uint32_t from, std::uint32_t to,
                               const Pose3d& measurement) {
    if (from == to || FindKeyframe(from) == nullptr ||
        FindKeyframe(to) == nullptr) {
        return false;
    }
    backend_->AddLoopFactor(from, to, measurement, config_.loop_information);
    if (!backend_->Optimize()) {
        return false;
    }
    return SynchronizeGraph();
}

bool SlamSystem::AddLocalizationPrior(
    std::uint32_t id, const Pose3d& measurement,
    const Eigen::Matrix<double, 6, 6>& information) {
    if (FindKeyframe(id) == nullptr) {
        return false;
    }
    backend_->AddLocalizationPrior(id, measurement, information);
    if (!backend_->Optimize()) {
        return false;
    }
    return SynchronizeGraph();
}

}  // namespace x86_lio_slam
