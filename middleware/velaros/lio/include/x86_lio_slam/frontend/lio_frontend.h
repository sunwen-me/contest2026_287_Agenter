#pragma once

#include "x86_lio_slam/common/types.h"

#include <Eigen/Core>

#include <cstdint>
#include <span>
#include <vector>

namespace x86_lio_slam {

class ILioFrontend {
public:
    virtual ~ILioFrontend() = default;

    virtual void Reset() = 0;

    virtual void AddImu(const ImuSample& imu) = 0;

    virtual void AddPointCloud(std::span<const PointXYZIT> cloud) = 0;

    virtual bool Process(OdometryState& output) = 0;

    virtual std::uint64_t LastCloudId() const = 0;

    virtual std::span<const Eigen::Vector3f> LastCloudBody() const = 0;
};

}  // namespace x86_lio_slam
