#include "x86_lio_slam/map/small_ivox_map.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace x86_lio_slam {

SmallIVoxMap::SmallIVoxMap(float resolution, std::size_t capacity)
    : resolution_(resolution), inverse_resolution_(1.0F / resolution),
      capacity_(capacity) {
    if (!(resolution > 0.0F) || !std::isfinite(resolution)) {
        throw std::invalid_argument("SmallIVox resolution must be positive");
    }
}

std::size_t SmallIVoxMap::VoxelIndexHash::operator()(
    const VoxelIndex& index) const {
    std::uint64_t value = (static_cast<std::uint64_t>(index.x) << 32U) |
                          (static_cast<std::uint64_t>(index.y) << 16U) |
                          static_cast<std::uint64_t>(index.z);
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return static_cast<std::size_t>(value);
}

std::uint64_t SmallIVoxMap::Pack(const VoxelIndex& index) {
    return (static_cast<std::uint64_t>(index.x) << 32U) |
           (static_cast<std::uint64_t>(index.y) << 16U) |
           static_cast<std::uint64_t>(index.z);
}

bool SmallIVoxMap::GetVoxelIndex(
    const Eigen::Vector3f& point,
    std::array<std::int64_t, 3>& index) const {
    if (!point.allFinite()) {
        return false;
    }
    const Eigen::Array3f scaled = point.array() * inverse_resolution_;
    for (int axis = 0; axis < 3; ++axis) {
        const double value =
            std::floor(static_cast<double>(scaled[axis])) +
            static_cast<double>(kCoordinateOffset);
        if (value < 0.0 || value > 65535.0) {
            return false;
        }
        index[static_cast<std::size_t>(axis)] =
            static_cast<std::int64_t>(value);
    }
    return true;
}

bool SmallIVoxMap::GetVoxelIndex(const Eigen::Vector3f& point,
                                 VoxelIndex& index) const {
    std::array<std::int64_t, 3> integer_index{};
    if (!GetVoxelIndex(point, integer_index)) {
        return false;
    }
    index.x = static_cast<std::uint16_t>(integer_index[0]);
    index.y = static_cast<std::uint16_t>(integer_index[1]);
    index.z = static_cast<std::uint16_t>(integer_index[2]);
    return true;
}

bool SmallIVoxMap::AddPoint(const Eigen::Vector3f& point) {
    if (capacity_ == 0) {
        return false;
    }

    VoxelIndex index;
    if (!GetVoxelIndex(point, index)) {
        return false;
    }

    auto found = voxels_.find(index);
    if (found != voxels_.end()) {
        entries_.splice(entries_.begin(), entries_, found->second);
        found->second = entries_.begin();
        return false;
    }

    if (entries_.size() >= capacity_) {
        voxels_.erase(entries_.back().index);
        entries_.pop_back();
    }

    entries_.push_front(Entry{index, point});
    voxels_.emplace(index, entries_.begin());
    return true;
}

std::size_t SmallIVoxMap::FindNearest(
    const Eigen::Vector3f& query,
    std::span<Eigen::Vector3f> output) const {
    if (output.empty()) {
        return 0;
    }

    VoxelIndex center;
    if (!GetVoxelIndex(query, center)) {
        return 0;
    }

    std::vector<Eigen::Vector3f> candidates;
    candidates.reserve(7);
    const std::array<std::array<int, 3>, 7> offsets = {{
        {{0, 0, 0}}, {{1, 0, 0}}, {{-1, 0, 0}}, {{0, 1, 0}},
        {{0, -1, 0}}, {{0, 0, 1}}, {{0, 0, -1}},
    }};
    for (const auto& offset : offsets) {
        const int x = static_cast<int>(center.x) + offset[0];
        const int y = static_cast<int>(center.y) + offset[1];
        const int z = static_cast<int>(center.z) + offset[2];
        if (x < 0 || y < 0 || z < 0 || x > 65535 || y > 65535 ||
            z > 65535) {
            continue;
        }
        const VoxelIndex index{static_cast<std::uint16_t>(x),
                               static_cast<std::uint16_t>(y),
                               static_cast<std::uint16_t>(z)};
        const auto found = voxels_.find(index);
        if (found != voxels_.end()) {
            candidates.push_back(found->second->point);
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [&query](const Eigen::Vector3f& lhs,
                       const Eigen::Vector3f& rhs) {
                  return (lhs - query).squaredNorm() <
                         (rhs - query).squaredNorm();
              });
    const std::size_t count = std::min(output.size(), candidates.size());
    for (std::size_t index = 0; index < count; ++index) {
        output[index] = candidates[index];
    }
    return count;
}

void SmallIVoxMap::Clear() {
    voxels_.clear();
    entries_.clear();
}

std::vector<Eigen::Vector3f> SmallIVoxMap::Points() const {
    std::vector<Eigen::Vector3f> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) {
        result.push_back(entry.point);
    }
    return result;
}

}  // namespace x86_lio_slam
