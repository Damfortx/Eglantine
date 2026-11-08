// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/slot_vector.h"
#include "video_core/mt/epoch.h"

namespace VideoCommon::MT {

/// Drop-in for Common::SlotVector whose objects never move: storage is a fixed table of
/// 1024-entry chunks, so another thread may hold a T& across inserts. Each slot carries a
/// generation (odd while live) that readers use to validate a SlotId they resolved earlier.
/// With an EpochManager attached, erase defers the destructor and the index reuse until no
/// reader can still be looking at the object.
template <class T>
class StableSlotVector {
    static constexpr u32 ChunkShift = 10;
    static constexpr u32 ChunkSize = 1u << ChunkShift;
    static constexpr u32 ChunkMask = ChunkSize - 1;
    static constexpr u32 MaxChunks = 4096; // 4M slots

    struct Entry {
        Entry() noexcept {}
        ~Entry() noexcept {}
        union {
            T object;
        };
        std::atomic<u32> generation{0};
    };

    struct Chunk {
        Entry entries[ChunkSize];
    };

public:
    class Iterator {
        friend StableSlotVector<T>;

    public:
        constexpr Iterator() = default;

        Iterator& operator++() noexcept {
            const u64* const bitset = slot_vector->stored_bitset.data();
            const u32 size = static_cast<u32>(slot_vector->stored_bitset.size()) * 64;
            if (id.Value() < size) {
                u32 index = id.Value() + 1;
                while (index < size) {
                    const u64 word = bitset[index / 64] >> (index % 64);
                    if (word != 0) {
                        index += static_cast<u32>(std::countr_zero(word));
                        break;
                    }
                    index = (index / 64 + 1) * 64;
                }
                id = index < size ? Common::SlotId{index} : Common::SlotId{};
            }
            return *this;
        }

        Iterator operator++(int) noexcept {
            const Iterator copy{*this};
            ++*this;
            return copy;
        }

        bool operator==(const Iterator& other) const noexcept {
            return id.index == other.id.index;
        }

        bool operator!=(const Iterator& other) const noexcept {
            return id.index != other.id.index;
        }

        std::pair<Common::SlotId, T*> operator*() const noexcept {
            return {id, std::addressof((*slot_vector)[id])};
        }

        T* operator->() const noexcept {
            return std::addressof((*slot_vector)[id]);
        }

    private:
        Iterator(StableSlotVector<T>* slot_vector_, Common::SlotId id_) noexcept
            : slot_vector{slot_vector_}, id{id_} {}

        StableSlotVector<T>* slot_vector{};
        Common::SlotId id;
    };

    StableSlotVector() : chunks{std::make_unique<std::atomic<Chunk*>[]>(MaxChunks)} {}

    ~StableSlotVector() noexcept {
        if (epoch) {
            epoch->Collect();
        }
        size_t index = 0;
        for (u64 bits : stored_bitset) {
            for (size_t bit = 0; bits; ++bit, bits >>= 1) {
                if ((bits & 1) != 0) {
                    EntryAt(static_cast<u32>(index + bit)).object.~T();
                }
            }
            index += 64;
        }
        for (u32 i = 0; i < num_chunks; ++i) {
            delete chunks[i].load(std::memory_order_relaxed);
        }
    }

    StableSlotVector(const StableSlotVector&) = delete;
    StableSlotVector& operator=(const StableSlotVector&) = delete;

    /// Defer erase through `manager` (nullptr: destroy at erase, as SlotVector does). The
    /// manager must outlive this vector, or be detached before it dies.
    void SetEpochManager(EpochManager* manager) noexcept {
        epoch = manager;
    }

    [[nodiscard]] T& operator[](Common::SlotId id) noexcept {
        ValidateIndex(id);
        return EntryAt(id.Value()).object;
    }

    [[nodiscard]] const T& operator[](Common::SlotId id) const noexcept {
        ValidateIndex(id);
        return EntryAt(id.Value()).object;
    }

    /// Reader access without the writer-owned bookkeeping checks: the object stays in place
    /// (and, with an EpochManager, alive until the reader's guard ends); validate the result
    /// with Generation/IsCurrent.
    [[nodiscard]] T& UncheckedAt(Common::SlotId id) const noexcept {
        return EntryAt(id.Value()).object;
    }

    /// Generation of the slot (odd while live), safe from any thread.
    [[nodiscard]] u32 Generation(Common::SlotId id) const noexcept {
        const u32 index = id.Value();
        if (index >= num_slots.load(std::memory_order_acquire)) {
            return 0;
        }
        return EntryAt(index).generation.load(std::memory_order_acquire);
    }

    /// Whether `id` still names the object a reader saw with generation `generation`.
    [[nodiscard]] bool IsCurrent(Common::SlotId id, u32 generation) const noexcept {
        return (generation & 1) != 0 && Generation(id) == generation;
    }

    template <typename... Args>
    [[nodiscard]] Common::SlotId insert(Args&&... args) noexcept {
        const u32 index = FreeValueIndex();
        Entry& entry = EntryAt(index);
        new (&entry.object) T(std::forward<Args>(args)...);
        entry.generation.fetch_add(1, std::memory_order_release);
        SetStorageBit(index);
        ++live_count;
        return Common::SlotId{index};
    }

    void erase(Common::SlotId id) noexcept {
        const u32 index = id.Value();
        Entry& entry = EntryAt(index);
        entry.generation.fetch_add(1, std::memory_order_release);
        ResetStorageBit(index);
        --live_count;
        if (epoch == nullptr) {
            entry.object.~T();
            free_list.push_back(index);
            return;
        }
        epoch->Retire([this, index] {
            EntryAt(index).object.~T();
            std::scoped_lock lock{deferred_mutex};
            deferred_free.push_back(index);
        });
    }

    [[nodiscard]] Iterator begin() noexcept {
        const auto it = std::ranges::find_if(stored_bitset, [](u64 value) { return value != 0; });
        if (it == stored_bitset.end()) {
            return end();
        }
        const u32 word_index = static_cast<u32>(std::distance(stored_bitset.begin(), it));
        const Common::SlotId first_id{word_index * 64 + static_cast<u32>(std::countr_zero(*it))};
        return Iterator(this, first_id);
    }

    [[nodiscard]] Iterator end() noexcept {
        return Iterator(this, Common::SlotId{});
    }

    [[nodiscard]] size_t size() const noexcept {
        return live_count;
    }

private:
    [[nodiscard]] Entry& EntryAt(u32 index) const noexcept {
        Chunk* const chunk = chunks[index >> ChunkShift].load(std::memory_order_acquire);
        return chunk->entries[index & ChunkMask];
    }

    void SetStorageBit(u32 index) noexcept {
        stored_bitset[index / 64] |= u64(1) << (index % 64);
    }

    void ResetStorageBit(u32 index) noexcept {
        stored_bitset[index / 64] &= ~(u64(1) << (index % 64));
    }

    void ValidateIndex(Common::SlotId id) const noexcept {
        DEBUG_ASSERT(id);
        DEBUG_ASSERT(id.Value() / 64 < stored_bitset.size());
        DEBUG_ASSERT(((stored_bitset[id.Value() / 64] >> (id.Value() % 64)) & 1) != 0);
    }

    [[nodiscard]] u32 FreeValueIndex() noexcept {
        if (free_list.empty() && epoch != nullptr) {
            std::scoped_lock lock{deferred_mutex};
            free_list.insert(free_list.end(), deferred_free.begin(), deferred_free.end());
            deferred_free.clear();
        }
        if (free_list.empty()) {
            AddChunk();
        }
        const u32 free_index = free_list.back();
        free_list.pop_back();
        return free_index;
    }

    void AddChunk() noexcept {
        ASSERT_MSG(num_chunks < MaxChunks, "StableSlotVector is full");
        chunks[num_chunks].store(new Chunk, std::memory_order_release);
        const u32 base = num_chunks * ChunkSize;
        ++num_chunks;
        num_slots.store(num_chunks * ChunkSize, std::memory_order_release);
        stored_bitset.resize(static_cast<size_t>(num_chunks) * ChunkSize / 64);
        // Hand out low indices first, like SlotVector does after a Reserve.
        for (u32 i = ChunkSize; i-- > 0;) {
            free_list.push_back(base + i);
        }
    }

    std::unique_ptr<std::atomic<Chunk*>[]> chunks;
    u32 num_chunks = 0;
    std::atomic<u32> num_slots{0};
    size_t live_count = 0;
    EpochManager* epoch = nullptr;

    std::vector<u64> stored_bitset;
    std::vector<u32> free_list;
    std::mutex deferred_mutex;
    std::vector<u32> deferred_free;
};

} // namespace VideoCommon::MT
