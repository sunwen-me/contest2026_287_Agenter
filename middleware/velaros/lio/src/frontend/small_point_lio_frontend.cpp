#include "x86_lio_slam/frontend/small_point_lio_frontend.h"

#include "x86_lio_slam/common/geometry.h"
#include "x86_lio_slam/map/small_ivox_map.h"

#include <Eigen/Eigenvalues>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace x86_lio_slam {

namespace {

Eigen::Matrix3d LeftJacobianSO3(const Eigen::Vector3d& value) {
    const double theta2 = value.squaredNorm();
    const Eigen::Matrix3d skew = Hat(value);
    if (theta2 < 1e-16) {
        return Eigen::Matrix3d::Identity() + 0.5 * skew +
               (1.0 / 6.0) * skew * skew;
    }
    const double theta = std::sqrt(theta2);
    return Eigen::Matrix3d::Identity() +
           ((1.0 - std::cos(theta)) / theta2) * skew +
           ((theta - std::sin(theta)) / (theta2 * theta)) * skew * skew;
}

std::uint64_t PackDownsampleIndex(const Eigen::Vector3f& point,
                                  float inverse_leaf) {
    constexpr std::int64_t kOffset = 1LL << 20;
    constexpr std::int64_t kMask = (1LL << 21) - 1;
    std::uint64_t result = 0;
    for (int axis = 0; axis < 3; ++axis) {
        const auto coordinate = static_cast<std::int64_t>(std::floor(
            static_cast<double>(point[axis]) * inverse_leaf));
        const auto shifted = coordinate + kOffset;
        if (shifted < 0 || shifted > kMask) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        result |= static_cast<std::uint64_t>(shifted & kMask)
                  << (21 * axis);
    }
    return result;
}

}  // namespace

void State30::Plus(const Vector30& increment) {
    position += increment.segment<3>(kPositionIndex);
    rotation = rotation * ExpSO3(increment.segment<3>(kRotationIndex));
    extrinsic_rotation =
        extrinsic_rotation * ExpSO3(increment.segment<3>(kExtrinsicRotationIndex));
    extrinsic_translation += increment.segment<3>(kExtrinsicTranslationIndex);
    velocity += increment.segment<3>(kVelocityIndex);
    omega += increment.segment<3>(kOmegaIndex);
    acceleration += increment.segment<3>(kAccelerationIndex);
    gravity += increment.segment<3>(kGravityIndex);
    gyro_bias += increment.segment<3>(kGyroBiasIndex);
    accel_bias += increment.segment<3>(kAccelBiasIndex);
}

SmallPointLioFrontend::SmallPointLioFrontend(
    LioConfig config, std::shared_ptr<ILocalMap> local_map)
    : config_(std::move(config)), local_map_(std::move(local_map)),
      point_kernel_(CreatePointKernelBackend(config_.point_kernel_backend)) {
    if (!local_map_) {
        local_map_ = std::make_shared<SmallIVoxMap>(config_.map_resolution,
                                                     config_.map_capacity);
    }
    Reset();
}

void SmallPointLioFrontend::Reset() {
    state_ = State30{};
    state_.extrinsic_rotation = config_.R_imu_lidar;
    state_.extrinsic_translation = config_.t_imu_lidar;
    state_.gravity = config_.gravity;
    state_.acceleration = -state_.gravity;

    covariance_.setIdentity();
    covariance_ *= 0.01;
    covariance_.block<3, 3>(State30::kGravityIndex,
                            State30::kGravityIndex).diagonal().setConstant(0.0001);
    covariance_.block<3, 3>(State30::kGyroBiasIndex,
                            State30::kGyroBiasIndex).diagonal().setConstant(0.001);
    covariance_.block<3, 3>(State30::kAccelBiasIndex,
                            State30::kAccelBiasIndex).diagonal().setConstant(0.001);

    point_queue_.clear();
    imu_queue_.clear();
    last_cloud_body_.clear();
    last_cloud_odom_.clear();
    last_cloud_id_ = 0;
    last_processed_cloud_id_ = 0;
    current_time_ = 0.0;
    initialized_ = false;
    has_output_ = false;
    local_map_->Clear();
}

void SmallPointLioFrontend::InsertImuSorted(const ImuSample& imu) {
    const auto position = std::upper_bound(
        imu_queue_.begin(), imu_queue_.end(), imu.timestamp,
        [](double timestamp, const ImuSample& sample) {
            return timestamp < sample.timestamp;
        });
    imu_queue_.insert(position, imu);
}

void SmallPointLioFrontend::InsertPointSorted(const QueuedPoint& point) {
    const auto position = std::upper_bound(
        point_queue_.begin(), point_queue_.end(), point.point.timestamp,
        [](double timestamp, const QueuedPoint& queued) {
            return timestamp < queued.point.timestamp;
        });
    point_queue_.insert(position, point);
}

void SmallPointLioFrontend::AddImu(const ImuSample& imu) {
    if (!IsFinite(imu)) {
        return;
    }
    InsertImuSorted(imu);
}

void SmallPointLioFrontend::AddPointCloud(std::span<const PointXYZIT> cloud) {
    ++last_cloud_id_;
    last_cloud_body_.clear();
    last_cloud_odom_.clear();

    std::vector<PointXYZIT> filtered;
    filtered.reserve(cloud.size());
    const int filter_step = std::max(1, config_.point_filter_num);
    for (std::size_t index = 0; index < cloud.size(); ++index) {
        const PointXYZIT& point = cloud[index];
        if (!IsFinite(point) || index % static_cast<std::size_t>(filter_step) != 0) {
            continue;
        }
        const float squared_distance = point.x * point.x + point.y * point.y +
                                       point.z * point.z;
        if (squared_distance < config_.min_distance * config_.min_distance ||
            squared_distance > config_.max_distance * config_.max_distance) {
            continue;
        }
        filtered.push_back(point);
    }

    if (config_.space_downsample && !filtered.empty() &&
        config_.space_downsample_leaf_size > 0.0F) {
        std::unordered_map<std::uint64_t, PointXYZIT> representatives;
        representatives.reserve(filtered.size());
        const float inverse_leaf = 1.0F / config_.space_downsample_leaf_size;
        for (const PointXYZIT& point : filtered) {
            const std::uint64_t key = PackDownsampleIndex(point.Vector(), inverse_leaf);
            if (key == std::numeric_limits<std::uint64_t>::max()) {
                continue;
            }
            representatives.emplace(key, point);
        }
        filtered.clear();
        filtered.reserve(representatives.size());
        for (const auto& item : representatives) {
            filtered.push_back(item.second);
        }
    }

    std::sort(filtered.begin(), filtered.end(),
              [](const PointXYZIT& lhs, const PointXYZIT& rhs) {
                  return lhs.timestamp < rhs.timestamp;
              });

    last_cloud_body_.reserve(filtered.size());
    for (const PointXYZIT& point : filtered) {
        last_cloud_body_.push_back(point.Vector());
        InsertPointSorted(QueuedPoint{point, last_cloud_id_});
    }
}

bool SmallPointLioFrontend::InitializeFromQueues() {
    if (point_queue_.size() < config_.init_map_size || point_queue_.empty()) {
        return false;
    }

    if (config_.fix_gravity_direction && imu_queue_.size() >= 200) {
        Eigen::Vector3d mean_acceleration = Eigen::Vector3d::Zero();
        for (const ImuSample& imu : imu_queue_) {
            mean_acceleration += imu.linear_acceleration;
        }
        mean_acceleration /= static_cast<double>(imu_queue_.size());
        if (mean_acceleration.norm() > 1e-9) {
            state_.gravity = -config_.gravity.norm() * mean_acceleration.normalized();
            state_.acceleration = -state_.gravity;
        }
    }

    double latest_time = current_time_;
    for (const QueuedPoint& queued : point_queue_) {
        const Eigen::Vector3d point_imu =
            state_.extrinsic_rotation * queued.point.Vector().cast<double>() +
            state_.extrinsic_translation;
        const Eigen::Vector3f point_odom =
            (state_.rotation * point_imu + state_.position).cast<float>();
        local_map_->AddPoint(point_odom);
        latest_time = std::max(latest_time, queued.point.timestamp);
    }
    for (const ImuSample& imu : imu_queue_) {
        latest_time = std::max(latest_time, imu.timestamp);
    }
    point_queue_.clear();
    imu_queue_.clear();
    current_time_ = latest_time;
    initialized_ = true;
    has_output_ = true;
    last_processed_cloud_id_ = last_cloud_id_;
    return true;
}

void SmallPointLioFrontend::PredictState(double timestamp) {
    const double dt = timestamp - current_time_;
    if (dt <= 0.0 || !std::isfinite(dt)) {
        return;
    }
    state_.position += state_.velocity * dt;
    state_.rotation = state_.rotation * ExpSO3(state_.omega * dt);
    state_.velocity +=
        (state_.rotation * state_.acceleration + state_.gravity) * dt;
    current_time_ = timestamp;
}

void SmallPointLioFrontend::PredictCovariance(double timestamp) {
    const double dt = timestamp - current_time_;
    if (dt <= 0.0 || !std::isfinite(dt)) {
        return;
    }

    Eigen::Matrix<double, 30, 30> transition =
        Eigen::Matrix<double, 30, 30>::Identity();
    const Eigen::Vector3d rotation_increment = -state_.omega * dt;
    transition.block<3, 3>(State30::kPositionIndex,
                           State30::kVelocityIndex).diagonal().setConstant(dt);
    transition.block<3, 3>(State30::kRotationIndex,
                           State30::kRotationIndex) = ExpSO3(rotation_increment);
    transition.block<3, 3>(State30::kRotationIndex, State30::kOmegaIndex) =
        LeftJacobianSO3(rotation_increment) * dt;
    transition.block<3, 3>(State30::kVelocityIndex,
                           State30::kRotationIndex) =
        -state_.rotation * Hat(state_.acceleration);
    transition.block<3, 3>(State30::kVelocityIndex,
                           State30::kAccelerationIndex) = state_.rotation * dt;
    transition.block<3, 3>(State30::kVelocityIndex,
                           State30::kGravityIndex).diagonal().setConstant(dt);
    covariance_ = transition * covariance_ * transition.transpose() +
                  ProcessNoise() * (dt * dt);
    covariance_ = 0.5 * (covariance_ + covariance_.transpose());
}

Eigen::Matrix<double, 30, 30> SmallPointLioFrontend::ProcessNoise() const {
    Eigen::Matrix<double, 30, 30> result =
        Eigen::Matrix<double, 30, 30>::Zero();
    result.block<3, 3>(State30::kVelocityIndex,
                       State30::kVelocityIndex).diagonal().setConstant(
        config_.velocity_cov);
    result.block<3, 3>(State30::kOmegaIndex, State30::kOmegaIndex)
        .diagonal()
        .setConstant(config_.omega_cov);
    result.block<3, 3>(State30::kAccelerationIndex,
                       State30::kAccelerationIndex).diagonal().setConstant(
        config_.acceleration_cov);
    result.block<3, 3>(State30::kGyroBiasIndex,
                       State30::kGyroBiasIndex).diagonal().setConstant(
        config_.gyro_bias_cov);
    result.block<3, 3>(State30::kAccelBiasIndex,
                       State30::kAccelBiasIndex).diagonal().setConstant(
        config_.accel_bias_cov);
    return result;
}

bool SmallPointLioFrontend::UpdatePoint(const Eigen::Vector3f& point_lidar,
                                        Eigen::Vector3f& point_odom) {
    const Eigen::Vector3d point_imu =
        (config_.estimate_extrinsic ? state_.extrinsic_rotation
                                    : config_.R_imu_lidar) *
            point_lidar.cast<double>() +
        (config_.estimate_extrinsic ? state_.extrinsic_translation
                                    : config_.t_imu_lidar);
    point_odom = (state_.rotation * point_imu + state_.position).cast<float>();

    std::array<Eigen::Vector3f, 5> nearest{};
    const std::size_t nearest_count =
        local_map_->FindNearest(point_odom, std::span(nearest));
    if (nearest_count < 5) {
        return false;
    }

    Eigen::Vector3f centroid = Eigen::Vector3f::Zero();
    for (std::size_t index = 0; index < nearest_count; ++index) {
        centroid += nearest[index];
    }
    centroid /= static_cast<float>(nearest_count);
    Eigen::Matrix3f covariance = Eigen::Matrix3f::Zero();
    for (std::size_t index = 0; index < nearest_count; ++index) {
        const Eigen::Vector3f centered = nearest[index] - centroid;
        covariance.noalias() += centered * centered.transpose();
    }
    covariance /= static_cast<float>(nearest_count - 1);
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3f> solver(covariance);
    if (solver.info() != Eigen::Success) {
        return false;
    }
    Eigen::Vector3f normal = solver.eigenvectors().col(0);
    const float normal_norm = normal.norm();
    if (!(normal_norm > 1e-6F)) {
        return false;
    }
    normal /= normal_norm;
    const float plane_offset = -normal.dot(centroid);
    for (std::size_t index = 0; index < nearest_count; ++index) {
        if (std::abs(normal.dot(nearest[index]) + plane_offset) >
            config_.plane_threshold) {
            return false;
        }
    }

    const double residual = static_cast<double>(normal.dot(point_odom) + plane_offset);
    if (point_lidar.squaredNorm() <=
        static_cast<float>(config_.match_squared * residual * residual)) {
        return false;
    }

    Eigen::Matrix<double, 1, 12> jacobian =
        Eigen::Matrix<double, 1, 12>::Zero();
    const Eigen::Vector3d normal_double = normal.cast<double>();
    const Eigen::Vector3d rotated_normal = state_.rotation.transpose() * normal_double;
    jacobian.block<1, 3>(0, 0) = normal_double.transpose();
    jacobian.block<1, 3>(0, 3) = point_imu.cross(rotated_normal).transpose();
    if (config_.estimate_extrinsic) {
        const Eigen::Matrix3d& extrinsic_rotation = state_.extrinsic_rotation;
        const Eigen::Vector3d extrinsic_normal =
            extrinsic_rotation.transpose() * rotated_normal;
        jacobian.block<1, 3>(0, 6) =
            point_lidar.cast<double>().cross(extrinsic_normal).transpose();
        jacobian.block<1, 3>(0, 9) = rotated_normal.transpose();
    }

    const Eigen::Matrix<double, 30, 1> pht =
        covariance_.block<30, 12>(0, 0) * jacobian.transpose();
    const Eigen::Matrix<double, 12, 1> pht_top = pht.topRows<12>();
    double innovation =
        (jacobian * pht_top)(0, 0) + config_.laser_point_cov;
    if (!(innovation > 1e-12) || !std::isfinite(innovation)) {
        innovation = 1e-6;
    }
    const Eigen::Matrix<double, 30, 1> gain = pht / innovation;
    State30::Vector30 increment = State30::Vector30::Zero();
    increment = gain * (-residual);
    state_.Plus(increment);
    covariance_ -= gain * jacobian * covariance_.block<12, 30>(0, 0);
    covariance_ = 0.5 * (covariance_ + covariance_.transpose());
    return true;
}

bool SmallPointLioFrontend::UpdateImu(const ImuSample& imu) {
    Eigen::Matrix<double, 6, 30> jacobian = Eigen::Matrix<double, 6, 30>::Zero();
    // The residual below follows the upstream ESKF convention
    // measurement - nominal - bias, so its covariance cross term uses the
    // positive state-and-bias observation Jacobian.
    jacobian.block<3, 3>(0, State30::kOmegaIndex) = Eigen::Matrix3d::Identity();
    jacobian.block<3, 3>(0, State30::kGyroBiasIndex) = Eigen::Matrix3d::Identity();
    jacobian.block<3, 3>(3, State30::kAccelerationIndex) =
        Eigen::Matrix3d::Identity();
    jacobian.block<3, 3>(3, State30::kAccelBiasIndex) = Eigen::Matrix3d::Identity();

    Eigen::Matrix<double, 6, 1> residual = Eigen::Matrix<double, 6, 1>::Zero();
    residual.head<3>() = imu.angular_velocity - state_.omega - state_.gyro_bias;
    residual.tail<3>() = config_.imu_acceleration_scale * imu.linear_acceleration -
                         state_.acceleration - state_.accel_bias;

    Eigen::Matrix<double, 6, 6> measurement_cov =
        Eigen::Matrix<double, 6, 6>::Zero();
    measurement_cov.topLeftCorner<3, 3>().diagonal().setConstant(
        config_.imu_gyro_cov);
    measurement_cov.bottomRightCorner<3, 3>().diagonal().setConstant(
        config_.imu_acc_cov);

    if (config_.check_saturation) {
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(imu.angular_velocity[axis]) >
                config_.saturation_gyro) {
                jacobian.row(axis).setZero();
                residual[axis] = 0.0;
            }
            if (std::abs(imu.linear_acceleration[axis]) >
                config_.saturation_acceleration) {
                jacobian.row(axis + 3).setZero();
                residual[axis + 3] = 0.0;
            }
        }
    }

    const Eigen::Matrix<double, 30, 6> pht = covariance_ * jacobian.transpose();
    const Eigen::Matrix<double, 6, 6> innovation =
        jacobian * pht + measurement_cov;
    Eigen::LDLT<Eigen::Matrix<double, 6, 6>> ldlt(innovation);
    if (ldlt.info() != Eigen::Success) {
        return false;
    }
    const Eigen::Matrix<double, 30, 6> gain =
        pht * ldlt.solve(Eigen::Matrix<double, 6, 6>::Identity());
    state_.Plus(gain * residual);
    covariance_ -= gain * jacobian * covariance_;
    covariance_ = 0.5 * (covariance_ + covariance_.transpose());
    return true;
}

void SmallPointLioFrontend::ProcessPoint(const QueuedPoint& queued) {
    if (queued.point.timestamp < current_time_) {
        return;
    }
    PredictState(queued.point.timestamp);
    Eigen::Vector3f point_odom = Eigen::Vector3f::Zero();
    UpdatePoint(queued.point.Vector(), point_odom);
    local_map_->AddPoint(point_odom);
    if (queued.cloud_id == last_cloud_id_) {
        last_cloud_odom_.push_back(point_odom);
    }
    last_processed_cloud_id_ = std::max(last_processed_cloud_id_, queued.cloud_id);
    has_output_ = true;
}

void SmallPointLioFrontend::ProcessImu(const ImuSample& imu) {
    if (imu.timestamp < current_time_) {
        return;
    }
    const double previous_time = current_time_;
    PredictState(imu.timestamp);
    if (imu.timestamp > previous_time &&
        imu.timestamp - previous_time <= config_.max_imu_dt) {
        const double before_covariance_time = current_time_;
        current_time_ = previous_time;
        PredictCovariance(imu.timestamp);
        current_time_ = before_covariance_time;
    }
    UpdateImu(imu);
    has_output_ = true;
}

void SmallPointLioFrontend::FillOutput(OdometryState& output) const {
    output.timestamp = current_time_;
    output.T_odom_body.rotation = state_.rotation;
    output.T_odom_body.translation = state_.position;
    output.velocity = state_.velocity;
    output.angular_velocity = state_.omega;
    output.pose_covariance.setZero();
    output.pose_covariance.topLeftCorner<3, 3>() =
        covariance_.block<3, 3>(State30::kPositionIndex, State30::kPositionIndex);
    output.pose_covariance.topRightCorner<3, 3>() =
        covariance_.block<3, 3>(State30::kPositionIndex, State30::kRotationIndex);
    output.pose_covariance.bottomLeftCorner<3, 3>() =
        covariance_.block<3, 3>(State30::kRotationIndex, State30::kPositionIndex);
    output.pose_covariance.bottomRightCorner<3, 3>() =
        covariance_.block<3, 3>(State30::kRotationIndex, State30::kRotationIndex);
    output.valid = initialized_ && has_output_ && output.T_odom_body.rotation.allFinite() &&
                   output.T_odom_body.translation.allFinite();
}

bool SmallPointLioFrontend::Process(OdometryState& output) {
    if (!initialized_ && !InitializeFromQueues()) {
        return false;
    }

    bool processed = false;
    while (!point_queue_.empty() || !imu_queue_.empty()) {
        const bool take_point = imu_queue_.empty() ||
                                (!point_queue_.empty() &&
                                 point_queue_.front().point.timestamp <=
                                     imu_queue_.front().timestamp);
        if (take_point) {
            const QueuedPoint queued = point_queue_.front();
            point_queue_.pop_front();
            ProcessPoint(queued);
        } else {
            const ImuSample imu = imu_queue_.front();
            imu_queue_.pop_front();
            ProcessImu(imu);
        }
        processed = true;
    }

    if (!has_output_) {
        return false;
    }
    FillOutput(output);
    return processed || initialized_;
}

std::size_t SmallPointLioFrontend::LocalMapSize() const {
    return local_map_->Size();
}

}  // namespace x86_lio_slam
