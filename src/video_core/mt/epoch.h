// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon::MT {

/// Epoch-based reclamation. Readers announce the global epoch while they hold pointers into
/// shared structures; writers retire objects tagged with the epoch at retirement, and an object
/// is destroyed once every active reader announced a later epoch (nobody can still see it).
class EpochManager {
public:
    static constexpr u32 MaxReaders = 32;
    static constexpr u64 Inactive = (std::numeric_limits<u64>::max)();

    EpochManager() : instance_id{NextInstanceId()} {
        for (auto& slot : slots) {
            slot.announce.store(Inactive, std::memory_order_relaxed);
            slot.used.store(false, std::memory_order_relaxed);
        }
    }

    ~EpochManager() {
        // Nobody reads any more: run whatever is still pending.
        for (auto& entry : retired) {
            entry.second();
        }
    }

    EpochManager(const EpochManager&) = delete;
    EpochManager& operator=(const EpochManager&) = delete;

    /// A reader slot for one thread; -1 when all are taken.
    [[nodiscard]] int RegisterReader() {
        for (u32 i = 0; i < MaxReaders; ++i) {
            bool expected = false;
            if (slots[i].used.compare_exchange_strong(expected, true)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    /// The calling thread's reader slot, registered on first use and kept for the thread's
    /// life (threads that read only now and then, e.g. guest write traps). -1 when all are taken.
    [[nodiscard]] int ThreadReader() {
        struct Cache {
            u64 instance = 0;
            int reader = -1;
        };
        thread_local Cache cache;
        if (cache.instance != instance_id) {
            cache.instance = instance_id;
            cache.reader = RegisterReader();
        }
        return cache.reader;
    }

    void UnregisterReader(int reader) {
        slots[reader].announce.store(Inactive, std::memory_order_release);
        slots[reader].used.store(false, std::memory_order_release);
    }

    /// seq_cst so the announce is visible before any pointer the reader loads afterwards (a
    /// writer that retires after our loads sees us; one that retired before cannot be loaded).
    void Enter(int reader) {
        slots[reader].announce.store(global.load(std::memory_order_seq_cst),
                                     std::memory_order_seq_cst);
    }

    void Exit(int reader) {
        slots[reader].announce.store(Inactive, std::memory_order_release);
    }

    /// Destroy `fn`'s object after the grace period.
    void Retire(std::function<void()> fn) {
        std::scoped_lock lock{retire_mutex};
        retired.emplace_back(global.fetch_add(1, std::memory_order_seq_cst), std::move(fn));
        if (auto_collect && retired.size() >= 64) {
            CollectLocked();
        }
    }

    /// Run retired destructors whose grace period has passed. Returns how many ran.
    size_t Collect() {
        std::scoped_lock lock{retire_mutex};
        return CollectLocked();
    }

    /// Off: reclaim callbacks run only in Collect (they then run on the collecting thread,
    /// under whatever locks it holds, never inside a Retire).
    void SetAutoCollect(bool on) noexcept {
        auto_collect = on;
    }

    [[nodiscard]] u64 CurrentEpoch() const {
        return global.load(std::memory_order_acquire);
    }

    [[nodiscard]] size_t PendingCount() {
        std::scoped_lock lock{retire_mutex};
        return retired.size();
    }

private:
    static u64 NextInstanceId() {
        static std::atomic<u64> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] u64 MinAnnounced() const {
        u64 min_epoch = Inactive;
        for (const auto& slot : slots) {
            const u64 announced = slot.announce.load(std::memory_order_seq_cst);
            min_epoch = announced < min_epoch ? announced : min_epoch;
        }
        return min_epoch;
    }

    size_t CollectLocked() {
        const u64 min_epoch = MinAnnounced();
        size_t freed = 0;
        auto it = retired.begin();
        // Retire epochs grow monotonically: free the prefix that every reader is past.
        while (it != retired.end() && it->first < min_epoch) {
            it->second();
            ++it;
            ++freed;
        }
        retired.erase(retired.begin(), it);
        return freed;
    }

    struct alignas(64) Slot {
        std::atomic<u64> announce;
        std::atomic<bool> used;
    };

    const u64 instance_id;
    std::atomic<u64> global{1};
    std::array<Slot, MaxReaders> slots;
    std::mutex retire_mutex;
    std::vector<std::pair<u64, std::function<void()>>> retired;
    bool auto_collect = true;
};

/// RAII critical section of a registered reader.
class EpochGuard {
public:
    EpochGuard(EpochManager& manager_, int reader_) : manager{manager_}, reader{reader_} {
        manager.Enter(reader);
    }
    ~EpochGuard() {
        manager.Exit(reader);
    }
    EpochGuard(const EpochGuard&) = delete;
    EpochGuard& operator=(const EpochGuard&) = delete;

private:
    EpochManager& manager;
    int reader;
};

} // namespace VideoCommon::MT
