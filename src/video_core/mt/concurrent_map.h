// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <bit>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "common/common_types.h"
#include "video_core/mt/epoch.h"

namespace VideoCommon::MT {

/// Hash map with one writer and lock-free readers. Open addressing, linear probing. A slot is
/// written once (key and value, then its state is published with release) and never changes
/// again until the table is rebuilt: erase only turns it into a tombstone and an update inserts
/// a fresh slot, so a reader that saw Full with acquire reads a stable key/value. Rebuilds
/// (load incl. tombstones over 1/2) build a new table and publish it; the old one is retired
/// through the EpochManager (freed at once when none is attached: no concurrent readers).
template <class Key, class Value, class Hash = std::hash<Key>, class Equal = std::equal_to<Key>>
class SingleWriterMap {
    static_assert(std::is_trivially_copyable_v<Value>, "readers copy values without locks");

    enum State : u8 { Empty = 0, Full = 1, Tombstone = 2 };

    struct Slot {
        std::atomic<u8> state{Empty};
        Key key{};
        Value value{};
    };

    struct Table {
        explicit Table(size_t capacity_)
            : capacity{capacity_}, slots{std::make_unique<Slot[]>(capacity_)} {}
        size_t capacity;
        std::unique_ptr<Slot[]> slots;
    };

public:
    explicit SingleWriterMap(size_t initial_capacity = 64) {
        table.store(new Table(std::bit_ceil(initial_capacity < 8 ? size_t{8} : initial_capacity)),
                    std::memory_order_release);
    }

    ~SingleWriterMap() {
        delete table.load(std::memory_order_relaxed);
    }

    SingleWriterMap(const SingleWriterMap&) = delete;
    SingleWriterMap& operator=(const SingleWriterMap&) = delete;

    void SetEpochManager(EpochManager* manager) noexcept {
        epoch = manager;
    }

    /// Any thread (inside an EpochGuard when the writer runs concurrently).
    [[nodiscard]] std::optional<Value> Find(const Key& key) const {
        const Table* const current = table.load(std::memory_order_acquire);
        const size_t mask = current->capacity - 1;
        size_t index = Hash{}(key) & mask;
        for (size_t probes = 0; probes < current->capacity; ++probes) {
            const Slot& slot = current->slots[index];
            const u8 state = slot.state.load(std::memory_order_acquire);
            if (state == Empty) {
                return std::nullopt;
            }
            if (state == Full && Equal{}(slot.key, key)) {
                return slot.value;
            }
            index = (index + 1) & mask;
        }
        return std::nullopt;
    }

    /// Writer only. Inserts or replaces.
    void InsertOrAssign(const Key& key, const Value& value) {
        Table* current = table.load(std::memory_order_relaxed);
        if (Slot* const existing = FindSlot(*current, key)) {
            if (std::memcmp(&existing->value, &value, sizeof(Value)) == 0) {
                return;
            }
            existing->state.store(Tombstone, std::memory_order_release);
            --live;
            ++tombstones;
        }
        if ((live + tombstones + 1) * 2 > current->capacity) {
            Rebuild();
            current = table.load(std::memory_order_relaxed);
        }
        Place(*current, key, value);
        ++live;
    }

    /// Writer only.
    bool Erase(const Key& key) {
        Table* const current = table.load(std::memory_order_relaxed);
        Slot* const existing = FindSlot(*current, key);
        if (existing == nullptr) {
            return false;
        }
        existing->state.store(Tombstone, std::memory_order_release);
        --live;
        ++tombstones;
        return true;
    }

    /// Writer only.
    void Clear() {
        Table* const old = table.load(std::memory_order_relaxed);
        table.store(new Table(old->capacity), std::memory_order_release);
        RetireTable(old);
        live = 0;
        tombstones = 0;
    }

    [[nodiscard]] size_t Size() const noexcept {
        return live;
    }

    /// Writer only: every live entry.
    template <typename Func>
    void ForEach(Func&& func) const {
        const Table* const current = table.load(std::memory_order_relaxed);
        for (size_t i = 0; i < current->capacity; ++i) {
            const Slot& slot = current->slots[i];
            if (slot.state.load(std::memory_order_relaxed) == Full) {
                func(slot.key, slot.value);
            }
        }
    }

private:
    Slot* FindSlot(Table& current, const Key& key) {
        const size_t mask = current.capacity - 1;
        size_t index = Hash{}(key) & mask;
        for (size_t probes = 0; probes < current.capacity; ++probes) {
            Slot& slot = current.slots[index];
            const u8 state = slot.state.load(std::memory_order_relaxed);
            if (state == Empty) {
                return nullptr;
            }
            if (state == Full && Equal{}(slot.key, key)) {
                return &slot;
            }
            index = (index + 1) & mask;
        }
        return nullptr;
    }

    static void Place(Table& current, const Key& key, const Value& value) {
        const size_t mask = current.capacity - 1;
        size_t index = Hash{}(key) & mask;
        while (current.slots[index].state.load(std::memory_order_relaxed) != Empty) {
            index = (index + 1) & mask;
        }
        Slot& slot = current.slots[index];
        slot.key = key;
        slot.value = value;
        slot.state.store(Full, std::memory_order_release);
    }

    void Rebuild() {
        Table* const old = table.load(std::memory_order_relaxed);
        size_t capacity = std::bit_ceil((live + 1) * 4);
        capacity = capacity < 8 ? 8 : capacity;
        auto* const rebuilt = new Table(capacity);
        for (size_t i = 0; i < old->capacity; ++i) {
            const Slot& slot = old->slots[i];
            if (slot.state.load(std::memory_order_relaxed) == Full) {
                Place(*rebuilt, slot.key, slot.value);
            }
        }
        tombstones = 0;
        table.store(rebuilt, std::memory_order_release);
        RetireTable(old);
    }

    void RetireTable(Table* old) {
        if (epoch == nullptr) {
            delete old;
            return;
        }
        epoch->Retire([old] { delete old; });
    }

    std::atomic<Table*> table{};
    size_t live = 0;
    size_t tombstones = 0;
    EpochManager* epoch = nullptr;
};

} // namespace VideoCommon::MT
