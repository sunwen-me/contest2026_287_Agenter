#pragma once

#include "velaros/navigation/types.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace velaros_navigation {

// A bounded two-slot publication buffer. The short publication critical
// section closes the race between a reader selecting a slot and a writer
// reusing that slot. Readers then hold a slot pin while planning/control runs.
template <std::size_t CellCapacity = kMaxNavMapCells>
class NavigationSnapshotBuffer {
public:
    static_assert(CellCapacity > 0);
    static_assert(CellCapacity <= kMaxNavMapCells);

    class ReadGuard {
    public:
        ReadGuard() = default;

        ~ReadGuard() { Release(); }

        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;

        ReadGuard(ReadGuard&& other) noexcept
            : owner_(other.owner_), slot_(other.slot_) {
            other.owner_ = nullptr;
        }

        ReadGuard& operator=(ReadGuard&& other) noexcept {
            if (this != &other) {
                Release();
                owner_ = other.owner_;
                slot_ = other.slot_;
                other.owner_ = nullptr;
            }
            return *this;
        }

        bool Valid() const { return owner_ != nullptr; }

        NavigationSnapshotView View() const {
            if (!Valid()) {
                return NavigationSnapshotView{};
            }
            const Slot& slot = owner_->slots_[slot_];
            NavigationSnapshotView view;
            view.pose = slot.pose;
            view.map = slot.map;
            view.map.cells = slot.cells.data();
            view.map.cell_count = slot.cell_count;
            return view;
        }

    private:
        friend class NavigationSnapshotBuffer;

        ReadGuard(const NavigationSnapshotBuffer* owner, std::uint8_t slot)
            : owner_(owner), slot_(slot) {}

        void Release() {
            if (owner_ != nullptr) {
                owner_->slots_[slot_].readers.fetch_sub(
                    1U, std::memory_order_release);
                owner_ = nullptr;
            }
        }

        const NavigationSnapshotBuffer* owner_ = nullptr;
        std::uint8_t slot_ = 0;
    };

    NavigationSnapshotBuffer() = default;

    NavigationSnapshotBuffer(const NavigationSnapshotBuffer&) = delete;
    NavigationSnapshotBuffer& operator=(const NavigationSnapshotBuffer&) = delete;

    bool Publish(const PoseSnapshot& pose, const NavMapView& map) {
        if (!map.IsValid() || map.cell_count > CellCapacity ||
            map.cells == nullptr) {
            return false;
        }

        LockPublication();
        const std::uint8_t current = published_slot_.load(std::memory_order_relaxed);
        const std::uint8_t candidate =
            current == kNoSlot ? 0U : static_cast<std::uint8_t>(1U - current);
        if (slots_[candidate].readers.load(std::memory_order_acquire) != 0U) {
            UnlockPublication();
            return false;
        }

        Slot& slot = slots_[candidate];
        slot.pose = pose;
        slot.map = map;
        slot.cell_count = map.cell_count;
        for (std::size_t index = 0; index < map.cell_count; ++index) {
            slot.cells[index] = map.cells[index];
        }
        slot.map.cells = slot.cells.data();
        slot.map.cell_count = slot.cell_count;
        published_slot_.store(candidate, std::memory_order_release);
        UnlockPublication();
        return true;
    }

    ReadGuard AcquireLatest() const {
        LockPublication();
        const std::uint8_t slot = published_slot_.load(std::memory_order_acquire);
        if (slot == kNoSlot) {
            UnlockPublication();
            return ReadGuard{};
        }
        slots_[slot].readers.fetch_add(1U, std::memory_order_acq_rel);
        UnlockPublication();
        return ReadGuard(this, slot);
    }

    bool Reset() {
        LockPublication();
        if (slots_[0].readers.load(std::memory_order_acquire) != 0U ||
            slots_[1].readers.load(std::memory_order_acquire) != 0U) {
            UnlockPublication();
            return false;
        }
        published_slot_.store(kNoSlot, std::memory_order_release);
        UnlockPublication();
        return true;
    }

    static constexpr std::size_t Capacity() { return CellCapacity; }

private:
    static constexpr std::uint8_t kNoSlot = 2U;

    struct Slot {
        PoseSnapshot pose{};
        NavMapView map{};
        std::array<NavCell, CellCapacity> cells{};
        std::size_t cell_count = 0;
        std::atomic<std::uint32_t> readers{0};
    };

    void LockPublication() const {
        while (publication_lock_.test_and_set(std::memory_order_acquire)) {
        }
    }

    void UnlockPublication() const {
        publication_lock_.clear(std::memory_order_release);
    }

    mutable std::atomic_flag publication_lock_ = ATOMIC_FLAG_INIT;
    mutable std::array<Slot, 2> slots_{};
    std::atomic<std::uint8_t> published_slot_{kNoSlot};
};

}  // namespace velaros_navigation
