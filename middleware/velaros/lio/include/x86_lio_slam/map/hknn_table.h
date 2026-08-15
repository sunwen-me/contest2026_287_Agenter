#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace x86_lio_slam::detail {

inline const std::array<float, 6> kHknnMinDistanceSquared = {
    0.062500F, 0.125000F, 0.250000F, 0.312500F, 0.375000F, 100.0F};

inline constexpr std::array<std::array<std::int8_t, 3>, 60>
    kHknnNeighborVoxel = {{
        {0, 0, 0},
        {0, -1, 0},
        {0, 0, -1},
        {-1, 0, 0},
        {-1, 0, -1},
        {0, -1, -1},
        {-1, -1, 0},
        {-1, -1, -1},
        {1, 0, 0},
        {0, 1, 0},
        {0, 0, 1},
        {0, -1, 1},
        {1, -1, 0},
        {1, 0, -1},
        {0, 1, -1},
        {-1, 1, 0},
        {-1, 0, 1},
        {-1, -1, 1},
        {-1, 1, -1},
        {1, -1, -1},
        {0, 0, -2},
        {0, 1, 1},
        {1, 1, 0},
        {1, 0, 1},
        {-2, 0, 0},
        {0, -2, 0},
        {-2, 0, -1},
        {-1, 0, -2},
        {-1, -2, 0},
        {-2, -1, 0},
        {0, -2, -1},
        {1, 1, -1},
        {-1, 1, 1},
        {1, -1, 1},
        {0, -1, -2},
        {-2, -1, -1},
        {-1, -1, -2},
        {-1, -2, -1},
        {-2, 1, 0},
        {1, -2, 0},
        {0, -2, 1},
        {0, 1, -2},
        {1, 0, -2},
        {1, 1, 1},
        {-2, 0, 1},
        {1, -1, -2},
        {1, -2, -1},
        {-2, 1, -1},
        {-2, -1, 1},
        {-1, 1, -2},
        {-1, -2, 1},
        {0, -2, -2},
        {-2, 0, -2},
        {-2, 1, 1},
        {1, 1, -2},
        {1, -2, 1},
        {-2, -2, 0},
        {-2, -1, -2},
        {-2, -2, -1},
        {-1, -2, -2},
    }};

inline constexpr std::array<std::uint16_t, 7> kHknnSearchGroupOffsets = {
    0, 43, 135, 219, 321, 465, 593};

inline constexpr std::array<std::uint8_t, 593> kHknnSearchOrder = {
    // Group 0
    0, 8, 0, 1, 2, 3, 4, 5, 6, 7, 1, 4, 2, 3, 6, 7, 2, 4, 4, 5, 6,
    7, 3, 4, 1, 3, 5, 7, 4, 2, 5, 7, 5, 2, 6, 7, 6, 2, 3, 7, 7, 1, 7,

    // Group 1
    1, 4, 0, 1, 4, 5, 2, 4, 0, 1, 2, 3, 3, 4, 0, 2, 4, 6, 4, 4, 1,
    3, 4, 6, 5, 4, 2, 3, 4, 5, 6, 4, 1, 2, 5, 6, 7, 3, 3, 5, 6, 8, 4,
    0, 2, 4, 6, 9, 4, 0, 1, 4, 5, 10, 4, 0, 1, 2, 3, 11, 2, 2, 3, 12,
    2, 2, 6, 13, 2, 4, 6, 14, 2, 4, 5, 15, 2, 1, 5, 16, 2, 1, 3, 17,
    1, 3, 18, 1, 5, 19, 1, 6,

    // Group 2
    4, 2, 0, 2, 5, 2, 0, 1, 6, 2, 0, 4, 7, 4, 0, 1, 2, 4, 11, 2, 0,
    1, 12, 2, 0, 4, 13, 2, 0, 2, 14, 2, 0, 1, 15, 2, 0, 4, 16, 2, 0,
    2, 17, 3, 0, 1, 2, 18, 3, 0, 1, 4, 19, 3, 0, 2, 4, 21, 2, 0, 1,
    22, 2, 0, 4, 23, 2, 0, 2, 31, 2, 0, 4, 32, 2, 0, 1, 33, 2, 0, 2,
    43, 1, 0,

    // Group 3
    8, 4, 1, 3, 5, 7, 9, 4, 2, 3, 6, 7, 10, 4, 4, 5, 6, 7, 11, 2, 6,
    7, 12, 2, 3, 7, 13, 2, 5, 7, 14, 2, 6, 7, 15, 2, 3, 7, 16, 2, 5,
    7, 17, 1, 7, 18, 1, 7, 19, 1, 7, 20, 4, 4, 5, 6, 7, 24, 4, 1, 3,
    5, 7, 25, 4, 2, 3, 6, 7, 26, 2, 5, 7, 27, 2, 5, 7, 28, 2, 3, 7,
    29, 2, 3, 7, 30, 2, 6, 7, 34, 2, 6, 7, 35, 1, 7, 36, 1, 7, 37, 1,
    7,

    // Group 4
    11, 2, 4, 5, 12, 2, 1, 5, 13, 2, 1, 3, 14, 2, 2, 3, 15, 2, 2, 6,
    16, 2, 4, 6, 17, 2, 5, 6, 18, 2, 3, 6, 19, 2, 3, 5, 21, 4, 2, 3,
    4, 5, 22, 4, 1, 2, 5, 6, 23, 4, 1, 3, 4, 6, 26, 2, 1, 3, 27, 2,
    4, 6, 28, 2, 2, 6, 29, 2, 1, 5, 30, 2, 2, 3, 31, 2, 5, 6, 32, 2,
    3, 5, 33, 2, 3, 6, 34, 2, 4, 5, 35, 2, 3, 5, 36, 2, 5, 6, 37, 2,
    3, 6, 38, 2, 1, 5, 39, 2, 2, 6, 40, 2, 2, 3, 41, 2, 4, 5, 42, 2,
    4, 6, 44, 2, 1, 3, 45, 1, 6, 46, 1, 6, 47, 1, 5, 48, 1, 3, 49, 1,
    5, 50, 1, 3,

    // Group 5
    17, 1, 4, 18, 1, 2, 19, 1, 1, 21, 2, 6, 7, 22, 2, 3, 7, 23, 2,
    5, 7, 31, 3, 1, 2, 7, 32, 3, 2, 4, 7, 33, 3, 1, 4, 7, 35, 1, 1,
    36, 1, 4, 37, 1, 2, 38, 2, 3, 7, 39, 2, 3, 7, 40, 2, 6, 7, 41, 2,
    6, 7, 42, 2, 5, 7, 43, 3, 1, 2, 4, 44, 2, 5, 7, 45, 2, 4, 7, 46,
    2, 2, 7, 47, 2, 1, 7, 48, 2, 1, 7, 49, 2, 4, 7, 50, 2, 2, 7, 51,
    2, 6, 7, 52, 2, 5, 7, 53, 1, 1, 54, 1, 4, 55, 1, 2, 56, 2, 3, 7,
    57, 1, 7, 58, 1, 7, 59, 1, 7,
};

constexpr std::array<std::array<std::uint8_t, kHknnSearchOrder.size()>, 8>
BuildHknnSearchOrderByLocal() {
    std::array<std::array<std::uint8_t, kHknnSearchOrder.size()>, 8> result{};
    for (std::size_t local = 0; local < result.size(); ++local) {
        for (std::size_t index = 0; index < kHknnSearchOrder.size(); ++index) {
            result[local][index] = static_cast<std::uint8_t>(
                kHknnSearchOrder[index] ^ static_cast<std::uint8_t>(local));
        }
    }
    return result;
}

inline constexpr auto kHknnSearchOrderByLocal =
    BuildHknnSearchOrderByLocal();

// A batch query resolves absolute coarse-voxel neighbors once per coarse
// voxel, then reuses them for all eight possible local subvoxels. The table
// index covers the union of the signed HKNN offsets [-2, 2]^3.
constexpr std::array<std::array<std::uint8_t, 60>, 8>
BuildHknnNeighborhoodIndexByLocal() {
    std::array<std::array<std::uint8_t, 60>, 8> result{};
    for (std::size_t local = 0; local < result.size(); ++local) {
        const int dx = static_cast<int>(local & 1U);
        const int dy = static_cast<int>((local >> 1U) & 1U);
        const int dz = static_cast<int>((local >> 2U) & 1U);
        const int mirror_x = 1 - (dx << 1);
        const int mirror_y = 1 - (dy << 1);
        const int mirror_z = 1 - (dz << 1);
        for (std::size_t neighbor = 0; neighbor < 60; ++neighbor) {
            const auto& offset = kHknnNeighborVoxel[neighbor];
            const int x = 2 + mirror_x * static_cast<int>(offset[0]);
            const int y = 2 + mirror_y * static_cast<int>(offset[1]);
            const int z = 2 + mirror_z * static_cast<int>(offset[2]);
            result[local][neighbor] = static_cast<std::uint8_t>(
                (x * 5 + y) * 5 + z);
        }
    }
    return result;
}

inline constexpr auto kHknnNeighborhoodIndexByLocal =
    BuildHknnNeighborhoodIndexByLocal();

struct HknnRun {
    std::uint8_t neighbor = 0;
    std::uint8_t local_count = 0;
    std::uint16_t local_offset = 0;
};

constexpr std::size_t HknnRunCount() {
    std::size_t count = 0;
    for (std::size_t group = 0;
         group + 1 < kHknnSearchGroupOffsets.size(); ++group) {
        std::size_t cursor = kHknnSearchGroupOffsets[group];
        const std::size_t end = kHknnSearchGroupOffsets[group + 1];
        while (cursor < end) {
            ++count;
            cursor += 2U + kHknnSearchOrder[cursor + 1U];
        }
    }
    return count;
}

inline constexpr std::size_t kHknnRunCount = HknnRunCount();

struct HknnRunTable {
    std::array<HknnRun, kHknnRunCount> runs{};
    std::array<std::uint16_t, kHknnSearchGroupOffsets.size()>
        group_run_offsets{};
};

constexpr HknnRunTable BuildHknnRunTable() {
    HknnRunTable table;
    std::size_t run_index = 0;
    for (std::size_t group = 0;
         group + 1 < kHknnSearchGroupOffsets.size(); ++group) {
        table.group_run_offsets[group] =
            static_cast<std::uint16_t>(run_index);
        std::size_t cursor = kHknnSearchGroupOffsets[group];
        const std::size_t end = kHknnSearchGroupOffsets[group + 1];
        while (cursor < end) {
            table.runs[run_index++] = HknnRun{
                kHknnSearchOrder[cursor], kHknnSearchOrder[cursor + 1U],
                static_cast<std::uint16_t>(cursor + 2U)};
            cursor += 2U + kHknnSearchOrder[cursor + 1U];
        }
    }
    table.group_run_offsets.back() = static_cast<std::uint16_t>(run_index);
    return table;
}

inline constexpr HknnRunTable kHknnRunTable = BuildHknnRunTable();

constexpr std::array<std::array<std::uint8_t, kHknnRunCount>, 8>
BuildHknnRunLocalMasks() {
    std::array<std::array<std::uint8_t, kHknnRunCount>, 8> result{};
    for (std::size_t local = 0; local < result.size(); ++local) {
        for (std::size_t run_index = 0; run_index < kHknnRunCount;
             ++run_index) {
            const HknnRun& run = kHknnRunTable.runs[run_index];
            std::uint8_t mask = 0;
            for (std::uint8_t local_offset = 0;
                 local_offset < run.local_count; ++local_offset) {
                mask = static_cast<std::uint8_t>(
                    mask | (1U << kHknnSearchOrderByLocal[local]
                                           [run.local_offset + local_offset]));
            }
            result[local][run_index] = mask;
        }
    }
    return result;
}

inline constexpr auto kHknnRunLocalMasks = BuildHknnRunLocalMasks();

// Per-local query records remove the small decode tables from the hot loop.
// Offsets are already mirrored for the query's local subvoxel, so the caller
// can form a coarse key with three additions.
struct HknnQueryRun {
    std::uint8_t neighbor = 0;
    std::int8_t offset_x = 0;
    std::int8_t offset_y = 0;
    std::int8_t offset_z = 0;
    std::uint8_t table_index = 0;
    std::uint8_t local_mask = 0;
    std::uint8_t local_count = 0;
    std::uint16_t local_offset = 0;
};

constexpr std::array<std::array<HknnQueryRun, kHknnRunCount>, 8>
BuildHknnQueryRunsByLocal() {
    std::array<std::array<HknnQueryRun, kHknnRunCount>, 8> result{};
    for (std::size_t local = 0; local < result.size(); ++local) {
        const int dx = static_cast<int>(local & 1U);
        const int dy = static_cast<int>((local >> 1U) & 1U);
        const int dz = static_cast<int>((local >> 2U) & 1U);
        const int mirror_x = 1 - (dx << 1);
        const int mirror_y = 1 - (dy << 1);
        const int mirror_z = 1 - (dz << 1);
        for (std::size_t run_index = 0; run_index < kHknnRunCount;
             ++run_index) {
            const HknnRun& run = kHknnRunTable.runs[run_index];
            const auto& neighbor = kHknnNeighborVoxel[run.neighbor];
            result[local][run_index] = HknnQueryRun{
                run.neighbor,
                static_cast<std::int8_t>(mirror_x *
                                          static_cast<int>(neighbor[0])),
                static_cast<std::int8_t>(mirror_y *
                                          static_cast<int>(neighbor[1])),
                static_cast<std::int8_t>(mirror_z *
                                          static_cast<int>(neighbor[2])),
                kHknnNeighborhoodIndexByLocal[local][run.neighbor],
                kHknnRunLocalMasks[local][run_index], run.local_count,
                run.local_offset};
        }
    }
    return result;
}

inline constexpr auto kHknnQueryRunsByLocal = BuildHknnQueryRunsByLocal();

}  // namespace x86_lio_slam::detail
