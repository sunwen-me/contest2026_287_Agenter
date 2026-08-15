#pragma once

#include "x86_lio_slam/keyframe/keyframe.h"

#include <Eigen/Core>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace x86_lio_slam {

struct GlobalMapConfig {
    float voxel_size = 0.1F;
    std::size_t max_points = 2000000;
};

class GlobalMapBuilder {
public:
    explicit GlobalMapBuilder(GlobalMapConfig config = GlobalMapConfig{})
        : config_(config) {}

    void Rebuild(std::span<const Keyframe> keyframes);

    void UpdateAffected(std::span<const Keyframe> keyframes,
                        std::span<const std::uint32_t> affected_ids);

    const std::vector<Eigen::Vector3f>& Points() const { return points_; }

    std::uint64_t Version() const { return version_; }

    bool WritePcd(const std::string& path) const;

private:
    bool AddPoint(const Eigen::Vector3f& point);

    GlobalMapConfig config_;
    std::vector<Eigen::Vector3f> points_;
    std::unordered_set<std::uint64_t> occupied_voxels_;
    std::unordered_set<std::uint32_t> indexed_keyframes_;
    bool initialized_ = false;
    std::uint64_t version_ = 0;
};

}  // namespace x86_lio_slam
