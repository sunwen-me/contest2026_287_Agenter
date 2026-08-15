#include "x86_lio_slam/map/super_lio_octvox_map.h"

#include "x86_lio_slam/map/hknn_table.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace x86_lio_slam {

namespace {

// GetFineIndex has already bounded value away from the int32 endpoints. For
// a bounded float, truncation plus one correction is the exact floor and
// avoids a libm floor call on targets without a cheap scalar floor instruction.
std::int32_t FastFloorToInt(float value) {
    const std::int32_t truncated = static_cast<std::int32_t>(value);
    return value < static_cast<float>(truncated) ? truncated - 1 : truncated;
}

struct CandidateHeap {
    enum class Update : std::uint8_t {
        Rejected,
        Inserted,
        Replaced,
    };

    std::array<float, 5> distances{};
    std::array<const OctVoxPoint*, 5> points{};
    std::uint8_t count = 0;
    std::uint8_t worst = 0;
    float max_distance = 0.0F;

    void UpdateWorst() {
        if (count == distances.size()) {
            const float d0 = distances[0];
            const float d1 = distances[1];
            const float d2 = distances[2];
            const float d3 = distances[3];
            const float d4 = distances[4];
            const std::uint8_t index01 = d0 >= d1 ? 0 : 1;
            const float maximum01 = d0 >= d1 ? d0 : d1;
            const std::uint8_t index23 = d2 >= d3 ? 2 : 3;
            const float maximum23 = d2 >= d3 ? d2 : d3;
            const std::uint8_t index0123 =
                maximum01 >= maximum23 ? index01 : index23;
            const float maximum0123 =
                maximum01 >= maximum23 ? maximum01 : maximum23;
            worst = maximum0123 >= d4 ? index0123 : 4;
            max_distance = maximum0123 >= d4 ? maximum0123 : d4;
            return;
        }

        std::uint8_t index = 0;
        float maximum = distances[0];
        for (std::uint8_t i = 1; i < count; ++i) {
            if (distances[i] > maximum) {
                maximum = distances[i];
                index = i;
            }
        }
        worst = index;
        max_distance = maximum;
    }

    // GetFineIndex has already rejected nonfinite query and map points, so the
    // squared distance is finite on this hot path.
    Update TryInsert(float distance, const OctVoxPoint& point) {
        if (count < distances.size()) {
            distances[count] = distance;
            points[count] = &point;
            ++count;
            if (count == distances.size()) {
                UpdateWorst();
            }
            return Update::Inserted;
        }
        if (distance >= max_distance) {
            return Update::Rejected;
        }
        distances[worst] = distance;
        points[worst] = &point;
        UpdateWorst();
        return Update::Replaced;
    }

};

// Eigen's three-element squaredNorm currently evaluates the two later
// components first and then adds the first component. Keep that association
// explicit while keeping the query coordinates in registers across the
// candidate loop.
inline float SquaredDistance(const OctVoxPoint& point, float query_x,
                             float query_y, float query_z) {
    const float dx = point.x - query_x;
    const float dy = point.y - query_y;
    const float dz = point.z - query_z;
    const float dyz = dz * dz + dy * dy;
    return dyz + dx * dx;
}

inline float CoarseAxisLowerBound(float query_relative, int offset,
                                  float coarse_size, float slack) {
    if (offset > 0) {
        return std::max(0.0F,
                        static_cast<float>(offset) * coarse_size -
                            query_relative - slack);
    }
    if (offset < 0) {
        return std::max(
            0.0F, query_relative - static_cast<float>(offset + 1) *
                       coarse_size - slack);
    }
    return 0.0F;
}

inline void BuildCoarseAxisLowerBounds(float query_relative, float coarse_size,
                                       float slack,
                                       std::array<float, 5>& output) {
    for (int offset = -2; offset <= 2; ++offset) {
        output[static_cast<std::size_t>(offset + 2)] =
            CoarseAxisLowerBound(query_relative, offset, coarse_size, slack);
    }
}

inline float CoarseCellSquaredLowerBound(
    const std::array<float, 5>& lower_x,
    const std::array<float, 5>& lower_y,
    const std::array<float, 5>& lower_z, int offset_x, int offset_y,
    int offset_z) {
    const float dx = lower_x[static_cast<std::size_t>(offset_x + 2)];
    const float dy = lower_y[static_cast<std::size_t>(offset_y + 2)];
    const float dz = lower_z[static_cast<std::size_t>(offset_z + 2)];
    const float dyz = dz * dz + dy * dy;
    return dyz + dx * dx;
}

inline std::size_t CoarseCacheIndex(std::uint32_t x, std::uint32_t y,
                                    std::uint32_t z) {
    // The cache is direct-mapped, so preserve spatial locality while mixing
    // the high coordinate bits into the low mask bits used for indexing.
    std::uint32_t hash = x * 0x9E3779B1U;
    hash ^= y * 0x85EBCA77U;
    hash ^= z * 0xC2B2AE3DU;
    hash ^= hash >> 16U;
    return static_cast<std::size_t>(hash) &
           (SuperLioOctVoxMap::QueryCache::kCoarseCapacity - 1U);
}

std::size_t WriteNearestResults(const CandidateHeap& heap,
                                std::span<Eigen::Vector3f> output) {
    const std::size_t result_count = std::min(
        output.size(), static_cast<std::size_t>(heap.count));
    std::array<std::size_t, 5> order{0, 1, 2, 3, 4};
    const auto compare_swap = [&heap, &order](std::size_t lhs,
                                               std::size_t rhs) {
        if (heap.distances[order[rhs]] < heap.distances[order[lhs]]) {
            std::swap(order[lhs], order[rhs]);
        }
    };
    // Fixed-size sorting network for the five returned representatives. This
    // preserves the old nearest-first API without std::sort's iterator and
    // comparator overhead in every point query.
    if (heap.count > 1) {
        compare_swap(0, 1);
    }
    if (heap.count > 2) {
        compare_swap(1, 2);
        compare_swap(0, 1);
    }
    if (heap.count > 3) {
        compare_swap(2, 3);
        compare_swap(1, 2);
        compare_swap(0, 1);
    }
    if (heap.count > 4) {
        compare_swap(3, 4);
        compare_swap(2, 3);
        compare_swap(1, 2);
        compare_swap(0, 1);
    }
    for (std::size_t index = 0; index < result_count; ++index) {
        const OctVoxPoint& point = *heap.points[order[index]];
        output[index] = Eigen::Vector3f(point.x, point.y, point.z);
    }
    return result_count;
}

}  // namespace

void SuperLioOctVoxMap::FlatHashTable::InsertNoGrow(
    const Key& key, std::uint32_t slot) {
    std::size_t index = Hash(key) & (buckets_.size() - 1U);
    std::size_t first_tombstone = buckets_.size();
    for (std::size_t probe = 0; probe < buckets_.size(); ++probe) {
        HashBucket& bucket = buckets_[index];
        if (bucket.slot == kEmptySlot) {
            const std::size_t target = first_tombstone == buckets_.size()
                                           ? index
                                           : first_tombstone;
            HashBucket& destination = buckets_[target];
            destination.key = key;
            destination.slot = slot;
            ++size_;
            if (first_tombstone != buckets_.size()) {
                --tombstones_;
            }
            return;
        }
        if (bucket.slot == kTombstoneSlot) {
            if (first_tombstone == buckets_.size()) {
                first_tombstone = index;
            }
        } else if (bucket.key == key) {
            bucket.slot = slot;
            return;
        }
        index = (index + 1U) & (buckets_.size() - 1U);
    }
    throw std::logic_error("OctVox flat hash table is full");
}

void SuperLioOctVoxMap::FlatHashTable::Rehash(std::size_t bucket_count) {
    std::size_t target = 16;
    while (target < bucket_count) {
        target <<= 1U;
    }

    std::vector<HashBucket> old = std::move(buckets_);
    buckets_.assign(target, HashBucket{});
    bucket_mask_ = target - 1U;
    size_ = 0;
    tombstones_ = 0;
    for (const HashBucket& bucket : old) {
        if (bucket.slot != kEmptySlot && bucket.slot != kTombstoneSlot) {
            InsertNoGrow(bucket.key, bucket.slot);
        }
    }
}

void SuperLioOctVoxMap::FlatHashTable::Reserve(std::size_t expected_size) {
    if (expected_size == 0) {
        return;
    }
    std::size_t target = 16;
    while (target * 7U / 10U < expected_size) {
        target <<= 1U;
    }
    if (target > buckets_.size()) {
        Rehash(target);
    }
}

void SuperLioOctVoxMap::FlatHashTable::Insert(
    const Key& key, std::uint32_t slot) {
    if (buckets_.empty()) {
        Rehash(16);
    }
    if ((size_ + tombstones_ + 1U) * 10U >= buckets_.size() * 7U) {
        Rehash(buckets_.size() * 2U);
    }
    InsertNoGrow(key, slot);
}

void SuperLioOctVoxMap::FlatHashTable::Erase(const Key& key) {
    if (buckets_.empty()) {
        return;
    }

    std::size_t index = Hash(key) & (buckets_.size() - 1U);
    for (std::size_t probe = 0; probe < buckets_.size(); ++probe) {
        HashBucket& bucket = buckets_[index];
        if (bucket.slot == kEmptySlot) {
            return;
        }
        if (bucket.slot != kTombstoneSlot && bucket.key == key) {
            bucket.slot = kTombstoneSlot;
            --size_;
            ++tombstones_;
            if (tombstones_ > buckets_.size() / 4U ||
                tombstones_ > size_) {
                Rehash(buckets_.size());
            }
            return;
        }
        index = (index + 1U) & (buckets_.size() - 1U);
    }
}

void SuperLioOctVoxMap::FlatHashTable::Clear() {
    buckets_.clear();
    bucket_mask_ = 0;
    size_ = 0;
    tombstones_ = 0;
}

SuperLioOctVoxMap::SuperLioOctVoxMap(float resolution, std::size_t capacity)
    : resolution_(resolution), inverse_resolution_(1.0F / resolution),
      sub_inverse_resolution_(2.0F / resolution), capacity_(capacity) {
    if (!(resolution > 0.0F) || !std::isfinite(resolution)) {
        throw std::invalid_argument("SuperLioOctVox resolution must be positive");
    }
    // Start with a bounded pool. Both the pool and hash table grow only as
    // active voxels are inserted, even when the configured capacity is large.
    const std::size_t initial_size = std::min<std::size_t>(capacity_, 65536);
    entries_.reserve(initial_size);
    octants_.reserve(initial_size);
    voxels_.Reserve(initial_size);
}

std::int32_t SuperLioOctVoxMap::FloorDivideByTwo(std::int32_t value) {
    if (value >= 0) {
        return value / 2;
    }
    return -(((-value) + 1) / 2);
}

SuperLioOctVoxMap::FineIndex SuperLioOctVoxMap::GetFineIndex(
    const Eigen::Vector3f& point) const {
    FineIndex result;
    const float scaled_x = point.x() * sub_inverse_resolution_;
    const float scaled_y = point.y() * sub_inverse_resolution_;
    const float scaled_z = point.z() * sub_inverse_resolution_;
    const float minimum =
        static_cast<float>(std::numeric_limits<std::int32_t>::min()) + 4.0F;
    const float maximum =
        static_cast<float>(std::numeric_limits<std::int32_t>::max()) - 4.0F;
    // Ordered comparisons reject NaN as well as out-of-range infinities, so
    // this keeps the safety check without three separate isfinite calls.
    if (!(scaled_x >= minimum && scaled_x <= maximum) ||
        !(scaled_y >= minimum && scaled_y <= maximum) ||
        !(scaled_z >= minimum && scaled_z <= maximum)) {
        return result;
    }
    const std::int32_t fine_x = FastFloorToInt(scaled_x);
    const std::int32_t fine_y = FastFloorToInt(scaled_y);
    const std::int32_t fine_z = FastFloorToInt(scaled_z);

    result.coarse = Key{FloorDivideByTwo(fine_x), FloorDivideByTwo(fine_y),
                         FloorDivideByTwo(fine_z)};
    const int dx = static_cast<int>(fine_x - 2 * result.coarse.x);
    const int dy = static_cast<int>(fine_y - 2 * result.coarse.y);
    const int dz = static_cast<int>(fine_z - 2 * result.coarse.z);
    if (dx < 0 || dx > 1 || dy < 0 || dy > 1 || dz < 0 || dz > 1) {
        return FineIndex{};
    }
    result.local = static_cast<std::uint8_t>((dz << 2) | (dy << 1) | dx);
    result.valid = true;
    return result;
}

bool SuperLioOctVoxMap::AddPoint(const Eigen::Vector3f& point) {
    if (capacity_ == 0) {
        return false;
    }

    const FineIndex index = GetFineIndex(point);
    if (!index.valid) {
        return false;
    }
    std::uint32_t slot = kInvalidSlot;
    if (!voxels_.Find(index.coarse, slot)) {
        if (active_count_ >= capacity_) {
            slot = EvictBack();
        } else {
            if (entries_.size() >= kInvalidSlot) {
                throw std::overflow_error("OctVox slot index overflow");
            }
            slot = static_cast<std::uint32_t>(entries_.size());
            entries_.emplace_back();
            octants_.emplace_back();
            ++active_count_;
        }

        Entry& entry = entries_[slot];
        Octant& octant = octants_[slot];
        entry.key = index.coarse;
        octant.occupied_mask = 0;
        octant.counts.fill(0);
        octant.points[index.local] =
            OctVoxPoint{point.x(), point.y(), point.z()};
        octant.counts[index.local] = 1;
        octant.occupied_mask =
            static_cast<std::uint8_t>(1U << index.local);
        entry.previous = kInvalidSlot;
        entry.next = kInvalidSlot;
        LinkFront(slot);
        voxels_.Insert(index.coarse, slot);
        return true;
    }

    Octant& octant = octants_[slot];
    std::uint8_t& count = octant.counts[index.local];
    OctVoxPoint& representative = octant.points[index.local];
    if (count == 0) {
        representative = OctVoxPoint{point.x(), point.y(), point.z()};
        count = 1;
        octant.occupied_mask = static_cast<std::uint8_t>(
            octant.occupied_mask | (1U << index.local));
    } else {
        const Eigen::Vector3f representative_vector(
            representative.x, representative.y, representative.z);
        if (count < 20 &&
            (point - representative_vector).squaredNorm() <= 0.01F) {
            const Eigen::Vector3f updated =
                (representative_vector * static_cast<float>(count) + point) /
                static_cast<float>(count + 1);
            representative =
                OctVoxPoint{updated.x(), updated.y(), updated.z()};
            ++count;
        }
    }

    if (active_count_ >= capacity_) {
        Touch(slot);
    }
    return false;
}

void SuperLioOctVoxMap::LinkFront(std::uint32_t slot) {
    Entry& entry = entries_[slot];
    entry.previous = kInvalidSlot;
    entry.next = lru_front_;
    if (lru_front_ == kInvalidSlot) {
        lru_back_ = slot;
    } else {
        entries_[lru_front_].previous = slot;
    }
    lru_front_ = slot;
}

void SuperLioOctVoxMap::Unlink(std::uint32_t slot) {
    Entry& entry = entries_[slot];
    if (entry.previous == kInvalidSlot) {
        lru_front_ = entry.next;
    } else {
        entries_[entry.previous].next = entry.next;
    }
    if (entry.next == kInvalidSlot) {
        lru_back_ = entry.previous;
    } else {
        entries_[entry.next].previous = entry.previous;
    }
    entry.previous = kInvalidSlot;
    entry.next = kInvalidSlot;
}

void SuperLioOctVoxMap::Touch(std::uint32_t slot) {
    if (slot == lru_front_) {
        return;
    }
    Unlink(slot);
    LinkFront(slot);
}

std::uint32_t SuperLioOctVoxMap::EvictBack() {
    if (lru_back_ == kInvalidSlot) {
        throw std::logic_error("OctVox LRU is empty at capacity");
    }
    const std::uint32_t slot = lru_back_;
    voxels_.Erase(entries_[slot].key);
    Unlink(slot);
    return slot;
}

std::size_t SuperLioOctVoxMap::FindNearest(
    const Eigen::Vector3f& query, std::span<Eigen::Vector3f> output) const {
    QueryCache cache;
    return FindNearest(query, output, cache);
}

std::size_t SuperLioOctVoxMap::FindNearest(
    const Eigen::Vector3f& query, std::span<Eigen::Vector3f> output,
    QueryCache& cache) const {
    return FindNearest(query, output, cache, nullptr);
}

std::size_t SuperLioOctVoxMap::FindNearest(
    const Eigen::Vector3f& query, std::span<Eigen::Vector3f> output,
    QueryCache& cache, QueryStats* stats) const {
    if (output.empty()) {
        return 0;
    }

    const FineIndex query_index = GetFineIndex(query);
    if (!query_index.valid) {
        cache.Reset();
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
        if (stats != nullptr) {
            ++stats->queries;
            ++stats->invalid_queries;
        }
#endif
        return 0;
    }
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
    if (stats != nullptr) {
        ++stats->queries;
        ++stats->valid_queries;
    }
#endif
    return FindNearestIndexed(query, query_index, output, cache, stats);
}

std::size_t SuperLioOctVoxMap::FindNearestIndexed(
    const Eigen::Vector3f& query, const FineIndex& query_index,
    std::span<Eigen::Vector3f> output, QueryCache& cache,
    QueryStats* stats) const {
#if !defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
    (void)stats;
#endif
    const float query_x = query.x();
    const float query_y = query.y();
    const float query_z = query.z();
    const float coarse_size = resolution_;
    const float query_relative_x =
        query_x - static_cast<float>(query_index.coarse.x) * coarse_size;
    const float query_relative_y =
        query_y - static_cast<float>(query_index.coarse.y) * coarse_size;
    const float query_relative_z =
        query_z - static_cast<float>(query_index.coarse.z) * coarse_size;
    const float coordinate_scale =
        std::max({std::abs(query_x), std::abs(query_y), std::abs(query_z),
                  coarse_size, 1.0F});
    const float bound_slack =
        32.0F * std::numeric_limits<float>::epsilon() * coordinate_scale;
    std::array<float, 5> coarse_lower_x{};
    std::array<float, 5> coarse_lower_y{};
    std::array<float, 5> coarse_lower_z{};
    BuildCoarseAxisLowerBounds(query_relative_x, coarse_size, bound_slack,
                               coarse_lower_x);
    BuildCoarseAxisLowerBounds(query_relative_y, coarse_size, bound_slack,
                               coarse_lower_y);
    BuildCoarseAxisLowerBounds(query_relative_z, coarse_size, bound_slack,
                               coarse_lower_z);
    const std::uint8_t* local_search_order =
        detail::kHknnSearchOrderByLocal[query_index.local].data();
    const auto& local_query_runs =
        detail::kHknnQueryRunsByLocal[query_index.local];

    // The exact-query cache keeps the original low-overhead fast path for
    // repeated points in one local subvoxel.
    const std::size_t cache_slot =
        (static_cast<std::uint32_t>(query_index.coarse.x) ^
         7U * static_cast<std::uint32_t>(query_index.coarse.y) ^
         5U * static_cast<std::uint32_t>(query_index.coarse.z) ^
         static_cast<std::uint32_t>(query_index.local)) &
        (QueryCache::kQueryCapacity - 1U);
    QueryCache::QueryEntry& query_cache =
        cache.queries[cache_slot];
    const bool cache_hit =
        query_cache.valid && query_cache.coarse_x == query_index.coarse.x &&
        query_cache.coarse_y == query_index.coarse.y &&
        query_cache.coarse_z == query_index.coarse.z &&
        query_cache.local == query_index.local;
    if (!cache_hit) {
        query_cache.valid = true;
        query_cache.coarse_x = query_index.coarse.x;
        query_cache.coarse_y = query_index.coarse.y;
        query_cache.coarse_z = query_index.coarse.z;
        query_cache.local = query_index.local;
        query_cache.octants.fill(QueryCache::kUnresolvedSlot);
    }
    const auto resolve_octant = [&](const detail::HknnQueryRun& run) {
        std::uint32_t& cached_slot = query_cache.octants[run.neighbor];
        if (cached_slot ==
            QueryCache::kUnresolvedSlot) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
            if (stats != nullptr) {
                ++stats->hash_lookups;
            }
#endif
            const Key key{query_index.coarse.x + run.offset_x,
                          query_index.coarse.y + run.offset_y,
                          query_index.coarse.z + run.offset_z};
            std::uint32_t slot = kInvalidSlot;
            const bool found = voxels_.Find(key, slot);
            cached_slot = found ? slot : QueryCache::kMissingSlot;
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
            if (stats != nullptr) {
                if (found) {
                    ++stats->hash_hits;
                } else {
                    ++stats->hash_misses;
                }
            }
#endif
        }
        return cached_slot;
    };

    CandidateHeap heap;
    // Keep the direct path lazy: sparse queries often terminate before all 60
    // HKNN neighbors are needed, so do not pre-resolve the complete table.
    for (std::size_t group = 0;
         group + 1 < detail::kHknnRunTable.group_run_offsets.size(); ++group) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
        if (stats != nullptr) {
            ++stats->groups_examined;
        }
#endif
        const std::uint16_t begin =
            detail::kHknnRunTable.group_run_offsets[group];
        const std::uint16_t end =
            detail::kHknnRunTable.group_run_offsets[group + 1];
        for (std::uint16_t run_index = begin; run_index < end; ++run_index) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
            if (stats != nullptr) {
                ++stats->runs_examined;
            }
#endif
            const detail::HknnQueryRun& run = local_query_runs[run_index];
            if (heap.count == 5) {
                if (CoarseCellSquaredLowerBound(
                        coarse_lower_x, coarse_lower_y, coarse_lower_z,
                        run.offset_x, run.offset_y,
                        run.offset_z) >= heap.max_distance) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
                    if (stats != nullptr) {
                        ++stats->cell_bound_skips;
                    }
#endif
                    continue;
                }
            }
            const std::uint32_t slot = resolve_octant(run);
            if (slot == QueryCache::kMissingSlot) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
                if (stats != nullptr) {
                    ++stats->missing_octants;
                }
#endif
                continue;
            }
            const Octant& octant = octants_[slot];
            if ((octant.occupied_mask & run.local_mask) == 0) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
                if (stats != nullptr) {
                    ++stats->empty_run_skips;
                }
#endif
                continue;
            }
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
            if (stats != nullptr) {
                ++stats->occupied_octants;
                ++stats->occupied_population[
                    std::popcount(octant.occupied_mask)];
            }
#endif
            const std::uint8_t* local_order =
                local_search_order + run.local_offset;
            for (std::uint8_t local_offset = 0;
                 local_offset < run.local_count; ++local_offset) {
                const std::uint8_t local_index = local_order[local_offset];
                if ((octant.occupied_mask &
                     static_cast<std::uint8_t>(1U << local_index)) == 0) {
                    continue;
                }
                const OctVoxPoint& point = octant.points[local_index];
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
                if (stats != nullptr) {
                    ++stats->candidate_evaluations;
                    const CandidateHeap::Update update = heap.TryInsert(
                        SquaredDistance(point, query_x, query_y, query_z),
                        point);
                    if (update == CandidateHeap::Update::Inserted) {
                        ++stats->candidate_insertions;
                    } else if (update == CandidateHeap::Update::Replaced) {
                        ++stats->candidate_replacements;
                    }
                } else {
                    heap.TryInsert(
                        SquaredDistance(point, query_x, query_y, query_z),
                        point);
                }
#else
                heap.TryInsert(
                    SquaredDistance(point, query_x, query_y, query_z), point);
#endif
            }
        }
        if (heap.count == 5 &&
            heap.max_distance < detail::kHknnMinDistanceSquared[group]) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
            if (stats != nullptr) {
                ++stats->early_terminations;
            }
#endif
            break;
        }
    }
    return WriteNearestResults(heap, output);
}

std::size_t SuperLioOctVoxMap::FindNearestFromNeighborhood(
    const Eigen::Vector3f& query, const Key& coarse, std::uint8_t local,
    std::span<Eigen::Vector3f> output,
    std::array<std::uint32_t, 125>& neighborhood,
    QueryStats* stats) const {
#if !defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
    (void)stats;
#endif
    if (output.empty()) {
        return 0;
    }

    CandidateHeap heap;
    const float query_x = query.x();
    const float query_y = query.y();
    const float query_z = query.z();
    const float coarse_size = resolution_;
    const float query_relative_x =
        query_x - static_cast<float>(coarse.x) * coarse_size;
    const float query_relative_y =
        query_y - static_cast<float>(coarse.y) * coarse_size;
    const float query_relative_z =
        query_z - static_cast<float>(coarse.z) * coarse_size;
    const float coordinate_scale =
        std::max({std::abs(query_x), std::abs(query_y), std::abs(query_z),
                  coarse_size, 1.0F});
    const float bound_slack =
        32.0F * std::numeric_limits<float>::epsilon() * coordinate_scale;
    std::array<float, 5> coarse_lower_x{};
    std::array<float, 5> coarse_lower_y{};
    std::array<float, 5> coarse_lower_z{};
    BuildCoarseAxisLowerBounds(query_relative_x, coarse_size, bound_slack,
                               coarse_lower_x);
    BuildCoarseAxisLowerBounds(query_relative_y, coarse_size, bound_slack,
                               coarse_lower_y);
    BuildCoarseAxisLowerBounds(query_relative_z, coarse_size, bound_slack,
                               coarse_lower_z);
    const std::uint8_t* local_search_order =
        detail::kHknnSearchOrderByLocal[local].data();
    const auto& local_query_runs = detail::kHknnQueryRunsByLocal[local];
    // HKNN visits subvoxels in precomputed distance groups and stops as soon
    // as five candidates are inside the group's geometric bound. Both the
    // run table and local order are decoded before this loop is entered.
    for (std::size_t group = 0;
         group + 1 < detail::kHknnRunTable.group_run_offsets.size(); ++group) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
        if (stats != nullptr) {
            ++stats->groups_examined;
        }
#endif
        const std::uint16_t begin =
            detail::kHknnRunTable.group_run_offsets[group];
        const std::uint16_t end =
            detail::kHknnRunTable.group_run_offsets[group + 1];
        for (std::uint16_t run_index = begin; run_index < end; ++run_index) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
            if (stats != nullptr) {
                ++stats->runs_examined;
            }
#endif
            const detail::HknnQueryRun& run = local_query_runs[run_index];
            if (heap.count == 5) {
                if (CoarseCellSquaredLowerBound(
                        coarse_lower_x, coarse_lower_y, coarse_lower_z,
                        run.offset_x, run.offset_y,
                        run.offset_z) >= heap.max_distance) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
                    if (stats != nullptr) {
                        ++stats->cell_bound_skips;
                    }
#endif
                    continue;
                }
            }
            std::uint32_t& cached_slot = neighborhood[run.table_index];
            if (cached_slot == QueryCache::kUnresolvedSlot) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
                if (stats != nullptr) {
                    ++stats->hash_lookups;
                }
#endif
                const Key key{
                    coarse.x + run.offset_x, coarse.y + run.offset_y,
                    coarse.z + run.offset_z};
                std::uint32_t slot = kInvalidSlot;
                const bool found = voxels_.Find(key, slot);
                cached_slot = found ? slot : QueryCache::kMissingSlot;
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
                if (stats != nullptr) {
                    if (found) {
                        ++stats->hash_hits;
                    } else {
                        ++stats->hash_misses;
                    }
                }
#endif
            }
            if (cached_slot == QueryCache::kMissingSlot) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
                if (stats != nullptr) {
                    ++stats->missing_octants;
                }
#endif
                continue;
            }
            const Octant& octant = octants_[cached_slot];
            if ((octant.occupied_mask & run.local_mask) == 0) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
                if (stats != nullptr) {
                    ++stats->empty_run_skips;
                }
#endif
                continue;
            }
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
            if (stats != nullptr) {
                ++stats->occupied_octants;
                ++stats->occupied_population[
                    std::popcount(octant.occupied_mask)];
            }
#endif
            const std::uint8_t* local_order =
                local_search_order + run.local_offset;
            for (std::uint8_t local_offset = 0;
                 local_offset < run.local_count; ++local_offset) {
                const std::uint8_t local_index = local_order[local_offset];
                if ((octant.occupied_mask &
                     static_cast<std::uint8_t>(1U << local_index)) == 0) {
                    continue;
                }
                const OctVoxPoint& point = octant.points[local_index];
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
                if (stats != nullptr) {
                    ++stats->candidate_evaluations;
                    const CandidateHeap::Update update = heap.TryInsert(
                        SquaredDistance(point, query_x, query_y, query_z),
                        point);
                    if (update == CandidateHeap::Update::Inserted) {
                        ++stats->candidate_insertions;
                    } else if (update == CandidateHeap::Update::Replaced) {
                        ++stats->candidate_replacements;
                    }
                } else {
                    heap.TryInsert(
                        SquaredDistance(point, query_x, query_y, query_z),
                        point);
                }
#else
                heap.TryInsert(
                    SquaredDistance(point, query_x, query_y, query_z), point);
#endif
            }
        }
        if (heap.count == 5 &&
            heap.max_distance < detail::kHknnMinDistanceSquared[group]) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
            if (stats != nullptr) {
                ++stats->early_terminations;
            }
#endif
            break;
        }
    }

    return WriteNearestResults(heap, output);
}

void SuperLioOctVoxMap::FindNearestBatch(
    std::span<const Eigen::Vector3f> queries,
    std::span<std::array<Eigen::Vector3f, 5>> output,
    std::span<std::uint8_t> counts, QueryCache& cache) const {
    FindNearestBatch(queries, output, counts, cache, nullptr);
}

void SuperLioOctVoxMap::FindNearestBatch(
    std::span<const Eigen::Vector3f> queries,
    std::span<std::array<Eigen::Vector3f, 5>> output,
    std::span<std::uint8_t> counts, QueryCache& cache,
    QueryStats* stats) const {
    if (output.size() < queries.size() || counts.size() < queries.size()) {
        throw std::invalid_argument("OctVox batch query spans are too small");
    }
    if (queries.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("OctVox batch query index overflow");
    }
    if (queries.empty()) {
        return;
    }

    for (std::size_t index = 0; index < queries.size(); ++index) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
        if (stats != nullptr) {
            ++stats->queries;
        }
#endif
        const FineIndex fine_index = GetFineIndex(queries[index]);
        if (!fine_index.valid) {
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
            if (stats != nullptr) {
                ++stats->invalid_queries;
            }
#endif
            counts[index] = 0;
            continue;
        }
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
        if (stats != nullptr) {
            ++stats->valid_queries;
        }
#endif
        ++cache.batch_valid_points;
        const std::uint32_t x =
            static_cast<std::uint32_t>(fine_index.coarse.x);
        const std::uint32_t y =
            static_cast<std::uint32_t>(fine_index.coarse.y);
        const std::uint32_t z =
            static_cast<std::uint32_t>(fine_index.coarse.z);
        const std::size_t cache_slot = CoarseCacheIndex(x, y, z);
        QueryCache::CoarseEntry& coarse_cache =
            cache.coarse_entries[cache_slot];
        const bool cache_hit =
            coarse_cache.valid &&
            coarse_cache.coarse_x == fine_index.coarse.x &&
            coarse_cache.coarse_y == fine_index.coarse.y &&
            coarse_cache.coarse_z == fine_index.coarse.z;
        if (cache_hit) {
            ++cache.batch_coarse_hits;
        } else {
            coarse_cache.valid = true;
            coarse_cache.coarse_x = fine_index.coarse.x;
            coarse_cache.coarse_y = fine_index.coarse.y;
            coarse_cache.coarse_z = fine_index.coarse.z;
            coarse_cache.octants.fill(QueryCache::kUnresolvedSlot);
        }
        counts[index] = static_cast<std::uint8_t>(FindNearestFromNeighborhood(
            queries[index], fine_index.coarse, fine_index.local,
            std::span(output[index]), coarse_cache.octants, stats));
    }
}

std::size_t SuperLioOctVoxMap::Size() const {
    return active_count_;
}

void SuperLioOctVoxMap::Clear() {
    voxels_.Clear();
    entries_.clear();
    octants_.clear();
    active_count_ = 0;
    lru_front_ = kInvalidSlot;
    lru_back_ = kInvalidSlot;
}

}  // namespace x86_lio_slam
