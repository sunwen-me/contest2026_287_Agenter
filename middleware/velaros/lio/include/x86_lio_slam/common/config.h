#pragma once

#include <Eigen/Core>

#include <cstddef>
#include <cstdint>

namespace x86_lio_slam {

enum class PointKernelBackend {
    Scalar,
    Eigen,
    X86Simd,
    Rvv,
};

struct LioConfig {
    float min_distance = 0.5F;
    float max_distance = 1000.0F;
    int point_filter_num = 1;
    bool space_downsample = true;
    float space_downsample_leaf_size = 0.5F;

    float map_resolution = 0.5F;
    std::size_t map_capacity = 1000000;
    std::size_t init_map_size = 10;

    Eigen::Vector3d gravity = Eigen::Vector3d(0.0, 0.0, -9.81);
    bool fix_gravity_direction = false;
    double imu_acceleration_scale = 1.0;
    bool check_saturation = true;
    double saturation_acceleration = 30.0;
    double saturation_gyro = 35.0;

    double laser_point_cov = 0.01;
    double imu_acc_cov = 0.01;
    double imu_gyro_cov = 0.01;
    double velocity_cov = 20.0;
    double acceleration_cov = 500.0;
    double omega_cov = 1000.0;
    double gyro_bias_cov = 0.0001;
    double accel_bias_cov = 0.0001;
    double plane_threshold = 0.1;
    double match_squared = 81.0;
    double max_imu_dt = 0.2;
    int batch_iterations = 4;
    double batch_damping = 1e-6;
    double batch_point_information = 1000.0;
    double batch_max_translation_update = 0.75;
    double batch_max_rotation_update = 0.35;
    bool collect_batch_timing = false;

    Eigen::Matrix3d R_imu_lidar = Eigen::Matrix3d::Identity();
    Eigen::Vector3d t_imu_lidar = Eigen::Vector3d::Zero();
    bool estimate_extrinsic = false;

    PointKernelBackend point_kernel_backend = PointKernelBackend::Eigen;
};

struct KeyframeConfig {
    double translation_threshold = 0.25;
    double rotation_threshold = 0.15;
    double time_threshold = 0.5;
    std::size_t max_cloud_points = 4000;
};

struct LoopConfig {
    bool enabled = false;
    double search_radius = 2.0;
    std::uint32_t min_id_separation = 20;
    std::size_t max_candidates = 5;
    double max_fitness = 0.5;
    std::size_t min_inliers = 10;
    double max_translation_correction = 10.0;
    double max_rotation_correction = 1.5;
    double correspondence_radius = 2.0;
    int registration_iterations = 5;
};

}  // namespace x86_lio_slam
