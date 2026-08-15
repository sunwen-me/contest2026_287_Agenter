#pragma once

#include "x86_lio_slam/common/config.h"
#include "x86_lio_slam/frontend/lio_frontend.h"
#include "x86_lio_slam/kernels/point_kernel.h"
#include "x86_lio_slam/map/local_map.h"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace x86_lio_slam {

struct State30 {
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    using Vector3 = Eigen::Vector3d;
    using Vector30 = Eigen::Matrix<double, 30, 1>;

    static constexpr int kDimension = 30;
    static constexpr int kPositionIndex = 0;
    static constexpr int kRotationIndex = 3;
    static constexpr int kExtrinsicRotationIndex = 6;
    static constexpr int kExtrinsicTranslationIndex = 9;
    static constexpr int kVelocityIndex = 12;
    static constexpr int kOmegaIndex = 15;
    static constexpr int kAccelerationIndex = 18;
    static constexpr int kGravityIndex = 21;
    static constexpr int kGyroBiasIndex = 24;
    static constexpr int kAccelBiasIndex = 27;

    Vector3 position = Vector3::Zero();
    Eigen::Matrix3d rotation = Eigen::Matrix3d::Identity();
    Eigen::Matrix3d extrinsic_rotation = Eigen::Matrix3d::Identity();
    Vector3 extrinsic_translation = Vector3::Zero();
    Vector3 velocity = Vector3::Zero();
    Vector3 omega = Vector3::Zero();
    Vector3 acceleration = Vector3::Zero();
    Vector3 gravity = Vector3::Zero();
    Vector3 gyro_bias = Vector3::Zero();
    Vector3 accel_bias = Vector3::Zero();

    void Plus(const Vector30& increment);
};

class SmallPointLioFrontend final : public ILioFrontend {
public:
    explicit SmallPointLioFrontend(
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

    std::span<const Eigen::Vector3f> LastCloudOdom() const {
        return last_cloud_odom_;
    }

    std::size_t LocalMapSize() const;

    const State30& State() const { return state_; }

    const Eigen::Matrix<double, 30, 30>& Covariance() const { return covariance_; }

    bool Initialized() const { return initialized_; }

    const LioConfig& Config() const { return config_; }

private:
    struct QueuedPoint {
        PointXYZIT point;
        std::uint64_t cloud_id = 0;
    };

    void InsertPointSorted(const QueuedPoint& point);

    void InsertImuSorted(const ImuSample& imu);

    bool InitializeFromQueues();

    void ProcessPoint(const QueuedPoint& point);

    void ProcessImu(const ImuSample& imu);

    void PredictState(double timestamp);

    void PredictCovariance(double timestamp);

    bool UpdatePoint(const Eigen::Vector3f& point_lidar,
                     Eigen::Vector3f& point_odom);

    bool UpdateImu(const ImuSample& imu);

    Eigen::Matrix<double, 30, 30> ProcessNoise() const;

    void FillOutput(OdometryState& output) const;

    LioConfig config_;
    std::shared_ptr<ILocalMap> local_map_;
    std::unique_ptr<IPointKernelBackend> point_kernel_;

    State30 state_;
    Eigen::Matrix<double, 30, 30> covariance_ =
        Eigen::Matrix<double, 30, 30>::Identity();

    std::deque<QueuedPoint> point_queue_;
    std::deque<ImuSample> imu_queue_;
    std::vector<Eigen::Vector3f> last_cloud_body_;
    std::vector<Eigen::Vector3f> last_cloud_odom_;
    std::uint64_t last_cloud_id_ = 0;
    std::uint64_t last_processed_cloud_id_ = 0;
    double current_time_ = 0.0;
    bool initialized_ = false;
    bool has_output_ = false;
};

}  // namespace x86_lio_slam
