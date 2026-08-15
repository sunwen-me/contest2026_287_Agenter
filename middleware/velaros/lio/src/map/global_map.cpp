#include "x86_lio_slam/map/global_map.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <unordered_map>

namespace x86_lio_slam {

namespace {

std::uint64_t PackVoxel(const Eigen::Vector3f& point, float inverse_size) {
    constexpr std::int64_t offset = 1LL << 20;
    constexpr std::int64_t mask = (1LL << 21) - 1;
    std::uint64_t key = 0;
    for (int axis = 0; axis < 3; ++axis) {
        const auto coordinate = static_cast<std::int64_t>(std::floor(
            static_cast<double>(point[axis]) * inverse_size));
        const auto shifted = coordinate + offset;
        if (shifted < 0 || shifted > mask) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        key |= static_cast<std::uint64_t>(shifted & mask) << (21 * axis);
    }
    return key;
}

}  // namespace

void GlobalMapBuilder::Rebuild(std::span<const Keyframe> keyframes) {
    ++version_;
    if (version_ == 0) {
        version_ = 1;
    }
    points_.clear();
    occupied_voxels_.clear();
    indexed_keyframes_.clear();
    initialized_ = true;
    indexed_keyframes_.reserve(keyframes.size());
    for (const Keyframe& keyframe : keyframes) {
        indexed_keyframes_.insert(keyframe.id);
    }

    if (config_.max_points == 0 || config_.voxel_size <= 0.0F) {
        return;
    }

    points_.reserve(config_.max_points);
    occupied_voxels_.reserve(config_.max_points);
    for (const Keyframe& keyframe : keyframes) {
        for (const Eigen::Vector3f& point : keyframe.cloud_body) {
            const Eigen::Vector3f transformed =
                keyframe.T_map_body.Transform(point);
            if (!AddPoint(transformed)) {
                if (points_.size() >= config_.max_points) {
                    return;
                }
            }
        }
    }
}

void GlobalMapBuilder::UpdateAffected(
    std::span<const Keyframe> keyframes,
    std::span<const std::uint32_t> affected_ids) {
    if (!initialized_) {
        Rebuild(keyframes);
        return;
    }
    if (keyframes.empty()) {
        Rebuild(keyframes);
        return;
    }

    std::size_t known_keyframes = 0;
    for (const Keyframe& keyframe : keyframes) {
        if (indexed_keyframes_.contains(keyframe.id)) {
            ++known_keyframes;
        }
    }

    bool historical_pose_changed = false;
    for (const std::uint32_t id : affected_ids) {
        if (indexed_keyframes_.contains(id)) {
            historical_pose_changed = true;
            break;
        }
    }

    // A removed keyframe or an id replacement cannot be represented by an
    // append-only update, so fall back to the exact full rebuild.
    if (historical_pose_changed || known_keyframes != indexed_keyframes_.size()) {
        Rebuild(keyframes);
        return;
    }

    bool changed = false;
    for (const Keyframe& keyframe : keyframes) {
        if (!indexed_keyframes_.emplace(keyframe.id).second) {
            continue;
        }
        changed = true;
        if (config_.max_points == 0 || config_.voxel_size <= 0.0F ||
            points_.size() >= config_.max_points) {
            continue;
        }
        for (const Eigen::Vector3f& point : keyframe.cloud_body) {
            const Eigen::Vector3f transformed =
                keyframe.T_map_body.Transform(point);
            const bool added = AddPoint(transformed);
            changed = added || changed;
            if (!added && points_.size() >= config_.max_points) {
                break;
            }
        }
    }
    if (changed) {
        ++version_;
        if (version_ == 0) {
            version_ = 1;
        }
    }
}

bool GlobalMapBuilder::AddPoint(const Eigen::Vector3f& point) {
    if (config_.max_points == 0 || config_.voxel_size <= 0.0F ||
        points_.size() >= config_.max_points || !point.allFinite()) {
        return false;
    }
    const std::uint64_t key = PackVoxel(point, 1.0F / config_.voxel_size);
    if (key == std::numeric_limits<std::uint64_t>::max() ||
        !occupied_voxels_.emplace(key).second) {
        return false;
    }
    points_.push_back(point);
    return true;
}

bool GlobalMapBuilder::WritePcd(const std::string& path) const {
    std::ofstream stream(path);
    if (!stream) {
        return false;
    }
    stream << "# .PCD v0.7 - Point Cloud Data file format\n"
           << "VERSION 0.7\n"
           << "FIELDS x y z\n"
           << "SIZE 4 4 4\n"
           << "TYPE F F F\n"
           << "COUNT 1 1 1\n"
           << "WIDTH " << points_.size() << "\n"
           << "HEIGHT 1\n"
           << "VIEWPOINT 0 0 0 1 0 0 0\n"
           << "POINTS " << points_.size() << "\n"
           << "DATA ascii\n";
    for (const Eigen::Vector3f& point : points_) {
        stream << point.x() << ' ' << point.y() << ' ' << point.z() << '\n';
    }
    return static_cast<bool>(stream);
}

}  // namespace x86_lio_slam
