#pragma once

#include "x86_lio_slam/map/local_map.h"

#include <array>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

namespace x86_lio_slam {

class SmallIVoxMap final : public ILocalMap {
public:
    // Keep the compact uint16 key while allowing a local map centered on the
    // sensor trajectory instead of only the positive world octant.
    static constexpr std::int64_t kCoordinateOffset = 1LL << 15;

    struct VoxelIndex {
        std::uint16_t x = 0;
        std::uint16_t y = 0;
        std::uint16_t z = 0;

        bool operator==(const VoxelIndex& other) const {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    SmallIVoxMap(float resolution, std::size_t capacity);

    bool AddPoint(const Eigen::Vector3f& point) override;

    std::size_t FindNearest(const Eigen::Vector3f& query,
                            std::span<Eigen::Vector3f> output) const override;

    std::size_t Size() const override { return voxels_.size(); }

    void Clear() override;

    float Resolution() const { return resolution_; }

    std::size_t Capacity() const { return capacity_; }

    bool GetVoxelIndex(const Eigen::Vector3f& point,
                       VoxelIndex& index) const;

    std::vector<Eigen::Vector3f> Points() const;

private:
    struct VoxelIndexHash {
        std::size_t operator()(const VoxelIndex& index) const;
    };

    struct Entry {
        VoxelIndex index;
        Eigen::Vector3f point;
    };

    using EntryList = std::list<Entry>;
    using EntryIterator = EntryList::iterator;

    static std::uint64_t Pack(const VoxelIndex& index);

    bool GetVoxelIndex(const Eigen::Vector3f& point,
                       std::array<std::int64_t, 3>& index) const;

    float resolution_;
    float inverse_resolution_;
    std::size_t capacity_;
    EntryList entries_;
    std::unordered_map<VoxelIndex, EntryIterator, VoxelIndexHash> voxels_;
};

}  // namespace x86_lio_slam
