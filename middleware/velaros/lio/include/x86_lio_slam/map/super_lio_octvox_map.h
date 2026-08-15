#pragma once

#include "x86_lio_slam/map/local_map.h"

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace x86_lio_slam {

struct OctVoxPoint {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

// Standalone clean-room OctVox-style map. A coarse voxel owns eight subvoxel
// representatives, which keeps the map compact while retaining local planar
// structure for the five-point correspondence search.
class SuperLioOctVoxMap final : public ILocalMap {
public:
    struct QueryStats {
        std::uint64_t queries = 0;
        std::uint64_t valid_queries = 0;
        std::uint64_t invalid_queries = 0;
        std::uint64_t hash_lookups = 0;
        std::uint64_t hash_hits = 0;
        std::uint64_t hash_misses = 0;
        std::uint64_t groups_examined = 0;
        std::uint64_t runs_examined = 0;
        std::uint64_t missing_octants = 0;
        std::uint64_t empty_run_skips = 0;
        std::uint64_t occupied_octants = 0;
        std::uint64_t cell_bound_skips = 0;
        std::uint64_t candidate_evaluations = 0;
        std::uint64_t candidate_insertions = 0;
        std::uint64_t candidate_replacements = 0;
        std::uint64_t early_terminations = 0;
        std::array<std::uint64_t, 9> occupied_population{};

        void Reset() { *this = QueryStats{}; }

        void Merge(const QueryStats& other) {
            queries += other.queries;
            valid_queries += other.valid_queries;
            invalid_queries += other.invalid_queries;
            hash_lookups += other.hash_lookups;
            hash_hits += other.hash_hits;
            hash_misses += other.hash_misses;
            groups_examined += other.groups_examined;
            runs_examined += other.runs_examined;
            missing_octants += other.missing_octants;
            empty_run_skips += other.empty_run_skips;
            occupied_octants += other.occupied_octants;
            cell_bound_skips += other.cell_bound_skips;
            candidate_evaluations += other.candidate_evaluations;
            candidate_insertions += other.candidate_insertions;
            candidate_replacements += other.candidate_replacements;
            early_terminations += other.early_terminations;
            for (std::size_t index = 0; index < occupied_population.size();
                 ++index) {
                occupied_population[index] += other.occupied_population[index];
            }
        }
    };

    // A cache is valid only while the map is unchanged. Batch LIO resets one
    // cache per worker at the beginning of every optimization frame.
    struct QueryCache {
        static constexpr std::uint32_t kUnresolvedSlot =
            std::numeric_limits<std::uint32_t>::max();
        static constexpr std::uint32_t kMissingSlot = kUnresolvedSlot - 1U;

        struct QueryEntry {
            bool valid = false;
            std::int32_t coarse_x = 0;
            std::int32_t coarse_y = 0;
            std::int32_t coarse_z = 0;
            std::uint8_t local = 0;
            std::array<std::uint32_t, 60> octants{};
        };

        struct CoarseEntry {
            bool valid = false;
            std::int32_t coarse_x = 0;
            std::int32_t coarse_y = 0;
            std::int32_t coarse_z = 0;
            std::array<std::uint32_t, 125> octants{};
        };

        // Exact query entries avoid recomputing neighbor keys for repeated
        // points in one local subvoxel.
        static constexpr std::size_t kQueryCapacity = 32;
        static constexpr std::size_t kCoarseCapacity = 1024;
        std::array<QueryEntry, kQueryCapacity> queries{};
        std::array<CoarseEntry, kCoarseCapacity> coarse_entries{};
        std::uint64_t batch_valid_points = 0;
        std::uint64_t batch_coarse_hits = 0;
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
        QueryStats stats;
#endif

        void Reset() {
            for (QueryEntry& query : queries) {
                query.valid = false;
            }
            for (CoarseEntry& coarse : coarse_entries) {
                coarse.valid = false;
            }
            batch_valid_points = 0;
            batch_coarse_hits = 0;
#if defined(X86_LIO_SLAM_ENABLE_MAP_QUERY_STATS)
            stats.Reset();
#endif
        }
    };

    SuperLioOctVoxMap(float resolution, std::size_t capacity);

    bool AddPoint(const Eigen::Vector3f& point) override;

    std::size_t FindNearest(
        const Eigen::Vector3f& query,
        std::span<Eigen::Vector3f> output) const override;

    std::size_t FindNearest(const Eigen::Vector3f& query,
                            std::span<Eigen::Vector3f> output,
                            QueryCache& cache) const;

    // The stats pointer is intended for diagnostic builds. Passing nullptr
    // keeps the production query path free of counter updates.
    std::size_t FindNearest(const Eigen::Vector3f& query,
                            std::span<Eigen::Vector3f> output,
                            QueryCache& cache, QueryStats* stats) const;

    // Query a contiguous batch while preserving output order. A worker-local
    // coarse neighborhood cache reuses absolute neighbor slots across points
    // and optimization iterations while the map remains unchanged.
    void FindNearestBatch(
        std::span<const Eigen::Vector3f> queries,
        std::span<std::array<Eigen::Vector3f, 5>> output,
        std::span<std::uint8_t> counts, QueryCache& cache) const;

    void FindNearestBatch(
        std::span<const Eigen::Vector3f> queries,
        std::span<std::array<Eigen::Vector3f, 5>> output,
        std::span<std::uint8_t> counts, QueryCache& cache,
        QueryStats* stats) const;

    std::size_t Size() const override;

    void Clear() override;

private:
    static constexpr std::uint32_t kEmptySlot =
        std::numeric_limits<std::uint32_t>::max();
    static constexpr std::uint32_t kInvalidSlot = QueryCache::kUnresolvedSlot;

    struct Key {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;

        bool operator==(const Key& other) const {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    struct Octant {
        std::uint8_t occupied_mask = 0;
        std::array<OctVoxPoint, 8> points{};
        std::array<std::uint8_t, 8> counts{};
    };

    struct Entry {
        Key key;
        std::uint32_t previous = kInvalidSlot;
        std::uint32_t next = kInvalidSlot;
    };

    struct HashBucket {
        Key key;
        std::uint32_t slot = kEmptySlot;
    };

    class FlatHashTable {
    public:
        bool Find(const Key& key, std::uint32_t& slot) const {
            if (bucket_mask_ == 0) {
                return false;
            }

            std::size_t index = Hash(key) & bucket_mask_;
            for (std::size_t probe = 0; probe <= bucket_mask_; ++probe) {
                const HashBucket& bucket = buckets_[index];
                if (bucket.slot == kEmptySlot) {
                    return false;
                }
                if (bucket.slot != kTombstoneSlot && bucket.key == key) {
                    slot = bucket.slot;
                    return true;
                }
                index = (index + 1U) & bucket_mask_;
            }
            return false;
        }

        void Insert(const Key& key, std::uint32_t slot);

        void Erase(const Key& key);

        void Clear();

        void Reserve(std::size_t expected_size);

    private:
        static constexpr std::uint32_t kTombstoneSlot = kEmptySlot - 1U;

        static std::size_t Hash(const Key& key) {
            std::uint64_t value =
                (static_cast<std::uint64_t>(
                     static_cast<std::uint32_t>(key.x))
                 << 32U) |
                static_cast<std::uint32_t>(key.y);
            value ^= static_cast<std::uint64_t>(
                         static_cast<std::uint32_t>(key.z)) *
                     0x9e3779b97f4a7c15ULL;
            value ^= value >> 30U;
            value *= 0xbf58476d1ce4e5b9ULL;
            value ^= value >> 27U;
            value ^= value >> 31U;
            return static_cast<std::size_t>(value);
        }

        void Rehash(std::size_t bucket_count);

        void InsertNoGrow(const Key& key, std::uint32_t slot);

        std::vector<HashBucket> buckets_;
        std::size_t bucket_mask_ = 0;
        std::size_t size_ = 0;
        std::size_t tombstones_ = 0;
    };

    struct FineIndex {
        Key coarse;
        std::uint8_t local = 0;
        bool valid = false;
    };

    FineIndex GetFineIndex(const Eigen::Vector3f& point) const;

    std::size_t FindNearestIndexed(
        const Eigen::Vector3f& query, const FineIndex& query_index,
        std::span<Eigen::Vector3f> output, QueryCache& cache,
        QueryStats* stats) const;

    std::size_t FindNearestFromNeighborhood(
        const Eigen::Vector3f& query, const Key& coarse, std::uint8_t local,
        std::span<Eigen::Vector3f> output,
        std::array<std::uint32_t, 125>& neighborhood,
        QueryStats* stats) const;

    static std::int32_t FloorDivideByTwo(std::int32_t value);

    void LinkFront(std::uint32_t slot);

    void Unlink(std::uint32_t slot);

    void Touch(std::uint32_t slot);

    std::uint32_t EvictBack();

    float resolution_ = 0.5F;
    float inverse_resolution_ = 2.0F;
    float sub_inverse_resolution_ = 4.0F;
    std::size_t capacity_ = 0;

    std::vector<Entry, Eigen::aligned_allocator<Entry>> entries_;
    std::vector<Octant, Eigen::aligned_allocator<Octant>> octants_;
    FlatHashTable voxels_;
    std::size_t active_count_ = 0;
    std::uint32_t lru_front_ = kInvalidSlot;
    std::uint32_t lru_back_ = kInvalidSlot;
};

}  // namespace x86_lio_slam
