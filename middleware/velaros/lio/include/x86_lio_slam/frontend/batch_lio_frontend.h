#pragma once

#include "x86_lio_slam/common/config.h"
#include "x86_lio_slam/frontend/lio_frontend.h"
#include "x86_lio_slam/kernels/point_kernel.h"
#include "x86_lio_slam/map/super_lio_octvox_map.h"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace x86_lio_slam {

// Clean-room Super-LIO-inspired frontend. The implementation keeps the
// standalone ILioFrontend boundary, but follows the upstream data flow:
// IMU propagation, per-point scan undistortion, OctVox-style map matching,
// batched point-to-plane information, and an 18-entry nominal ESKF update.
class BatchLioFrontend final : public ILioFrontend {
public:
    struct BatchTiming {
        std::uint64_t clouds = 0;
        std::uint64_t points = 0;
        std::uint64_t iterations = 0;
        double prediction_seconds = 0.0;
        double undistortion_seconds = 0.0;
        double correspondence_seconds = 0.0;
        double map_query_seconds = 0.0;
        double plane_fit_seconds = 0.0;
        double point_kernel_seconds = 0.0;
        double reduction_seconds = 0.0;
        double solver_seconds = 0.0;
        double map_insert_seconds = 0.0;
        std::uint64_t map_batch_valid_points = 0;
        std::uint64_t map_batch_coarse_hits = 0;
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
        std::uint64_t map_query_hash_lookups = 0;
        std::uint64_t map_query_hash_hits = 0;
        std::uint64_t map_query_hash_misses = 0;
        std::uint64_t map_query_groups = 0;
        std::uint64_t map_query_runs = 0;
        std::uint64_t map_query_missing_octants = 0;
        std::uint64_t map_query_empty_run_skips = 0;
        std::uint64_t map_query_occupied_octants = 0;
        std::uint64_t map_query_cell_bound_skips = 0;
        std::uint64_t map_query_candidate_evaluations = 0;
        std::uint64_t map_query_candidate_insertions = 0;
        std::uint64_t map_query_candidate_replacements = 0;
        std::uint64_t map_query_early_terminations = 0;
        std::array<std::uint64_t, 9> map_query_occupied_population{};
#endif
    };

    explicit BatchLioFrontend(
        LioConfig config = LioConfig{},
        std::shared_ptr<ILocalMap> local_map = nullptr);

    void Reset() override;

    void AddImu(const ImuSample& imu) override;

    void AddPointCloud(std::span<const PointXYZIT> cloud) override;

    bool Process(OdometryState& output) override;

    std::uint64_t LastCloudId() const override { return last_cloud_id_; }

    std::span<const Eigen::Vector3f> LastCloudBody() const override {
        return last_cloud_body_;
    }

    std::size_t LocalMapSize() const;

    const Pose3d& Pose() const { return pose_; }

    const LioConfig& Config() const { return config_; }

    const BatchTiming& TimingStats() const { return timing_; }

private:
    struct QueuedCloud {
        double start_time = 0.0;
        double timestamp = 0.0;
        std::uint64_t id = 0;
        std::vector<PointXYZIT> points;
    };

    struct PropagatedState {
        EIGEN_MAKE_ALIGNED_OPERATOR_NEW

        double timestamp = 0.0;
        Eigen::Matrix3d rotation = Eigen::Matrix3d::Identity();
        Eigen::Vector3d position = Eigen::Vector3d::Zero();
        Eigen::Vector3d velocity = Eigen::Vector3d::Zero();
        Eigen::Vector3d angular_velocity = Eigen::Vector3d::Zero();
        Eigen::Vector3d acceleration = Eigen::Vector3d::Zero();
    };

    struct PlaneObservation {
        EIGEN_MAKE_ALIGNED_OPERATOR_NEW

        Eigen::Vector3f normal = Eigen::Vector3f::Zero();
        float offset = 0.0F;
        bool valid = false;
    };

    void InsertImuSorted(const ImuSample& imu);

    void InsertCloudSorted(QueuedCloud cloud);

    bool InitializeFromQueue();

    void InitializeStateFromImu(double timestamp);

    void PredictTo(double timestamp);

    void IntegrateInterval(double timestamp, const ImuSample& previous,
                           const ImuSample& current);

    void AppendPropagationState();

    void UndistortCloud(const QueuedCloud& cloud,
                        std::vector<Eigen::Vector3f>& output) const;

    bool OptimizeCloud(std::span<const Eigen::Vector3f> body_points);

    void InsertCloudIntoMap(std::span<const Eigen::Vector3f> body_points);

    void ApplyStateIncrement(
        const Eigen::Matrix<double, 18, 1>& increment);

    Eigen::Vector3d TransformToImu(const Eigen::Vector3f& point) const;

    Eigen::Vector3f TransformToOdom(const Eigen::Vector3f& point) const;

    void FillOutput(OdometryState& output) const;

    LioConfig config_;
    std::shared_ptr<ILocalMap> local_map_;
    std::unique_ptr<IPointKernelBackend> point_kernel_;

    // Nominal state order follows Super-LIO: R, p, v, bg, ba, g.
    Pose3d pose_{};
    Eigen::Vector3d velocity_ = Eigen::Vector3d::Zero();
    Eigen::Vector3d gyro_bias_ = Eigen::Vector3d::Zero();
    Eigen::Vector3d accel_bias_ = Eigen::Vector3d::Zero();
    Eigen::Vector3d gravity_ = Eigen::Vector3d(0.0, 0.0, -9.81);
    Eigen::Matrix<double, 18, 18> covariance_ =
        Eigen::Matrix<double, 18, 18>::Identity();

    std::deque<ImuSample> imu_queue_;
    std::deque<QueuedCloud> cloud_queue_;
    std::vector<PropagatedState> propagated_states_;
    std::vector<Eigen::Vector3f> undistorted_body_;
    std::vector<Eigen::Vector3f> odom_points_;
    std::vector<Eigen::Vector3f> world_points_;

    // SoA scratch storage is reused across scans and batch iterations. This
    // also keeps the SIMD backend's input/output spans stable in memory.
    std::vector<float> point_ranges_;
    std::vector<float> body_x_;
    std::vector<float> body_y_;
    std::vector<float> body_z_;
    std::vector<PlaneObservation,
                Eigen::aligned_allocator<PlaneObservation>>
        plane_cache_;
    std::vector<float> plane_normal_x_;
    std::vector<float> plane_normal_y_;
    std::vector<float> plane_normal_z_;
    std::vector<float> plane_offsets_;
    std::vector<std::uint8_t> plane_valid_;
    std::vector<std::array<Eigen::Vector3f, 5>,
                Eigen::aligned_allocator<std::array<Eigen::Vector3f, 5>>>
        nearest_points_;
    std::vector<std::uint8_t> nearest_counts_;
    std::vector<float> point_residuals_;
    std::vector<float> point_jacobians_;
    std::vector<std::uint8_t> accepted_points_;
    std::vector<SuperLioOctVoxMap::QueryCache> query_caches_;

    ImuSample last_imu_{};
    bool has_last_imu_ = false;
    std::vector<Eigen::Vector3f> last_cloud_body_;
    std::uint64_t last_cloud_id_ = 0;
    double current_time_ = 0.0;
    bool initialized_ = false;
    bool has_output_ = false;
    BatchTiming timing_{};
};

}  // namespace x86_lio_slam
