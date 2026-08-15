#pragma once

#include <Eigen/Core>

#include <cstddef>
#include <span>

namespace x86_lio_slam {

class ILocalMap {
public:
    virtual ~ILocalMap() = default;

    virtual bool AddPoint(const Eigen::Vector3f& point) = 0;

    virtual std::size_t FindNearest(
        const Eigen::Vector3f& query,
        std::span<Eigen::Vector3f> output) const = 0;

    virtual std::size_t Size() const = 0;

    virtual void Clear() = 0;
};

}  // namespace x86_lio_slam
