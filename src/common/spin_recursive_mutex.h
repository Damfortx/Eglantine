// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace Common {

/// EDEN_NX2_LOCK_STATS (set to anything): SpinRecursiveMutex attributes its contended waits to
/// the waiting and the owning thread by name and logs the totals every 10 s. Off, it costs a
/// load of this flag per lock.
inline const bool g_lock_stats_enabled = std::getenv("EDEN_NX2_LOCK_STATS") != nullptr;

/// Records the calling thread's name for the owner lookup (once per thread).
inline void LockStatsRegisterThread();
/// Adds a wait of the calling thread on the thread that owned the lock when it began.
inline void LockStatsRecord(std::thread::id owner, std::chrono::steady_clock::duration wait);

/// A recursive mutex that spins briefly before sleeping. The GPU caches' locks are taken tens of
/// thousands of times a second by the GPU thread, the fencing thread and CPU write traps, and
/// held for microseconds: std::recursive_mutex put a waiting thread to sleep at once, and its
/// wake-up took longer than the critical section (title 1's GPU thread spent ~8% asleep there).
class SpinRecursiveMutex {
public:
    void lock() {
        const std::thread::id self = std::this_thread::get_id();
        if (owner.load(std::memory_order_relaxed) == self) {
            ++depth;
            return;
        }
        std::uint32_t expected = 0;
        if (!state.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
            LockContended();
        }
        owner.store(self, std::memory_order_relaxed);
        depth = 1;
        if (g_lock_stats_enabled) [[unlikely]] {
            thread_local bool registered = false;
            if (!registered) {
                registered = true;
                LockStatsRegisterThread();
            }
        }
    }

    bool try_lock() {
        const std::thread::id self = std::this_thread::get_id();
        if (owner.load(std::memory_order_relaxed) == self) {
            ++depth;
            return true;
        }
        std::uint32_t expected = 0;
        if (!state.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
            return false;
        }
        owner.store(self, std::memory_order_relaxed);
        depth = 1;
        return true;
    }

    void unlock() {
        if (--depth != 0) {
            return;
        }
        owner.store(std::thread::id{}, std::memory_order_relaxed);
        if (state.exchange(0, std::memory_order_release) == 2) {
            state.notify_one();
        }
    }

private:
    // About 10 microseconds of spinning before sleeping.
    static constexpr int SpinCount = 1024;

    static void Pause() {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
        _mm_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }

    void LockContended() {
        if (g_lock_stats_enabled) [[unlikely]] {
            // The owner may not be stored yet (just taken) or may change while we wait: this is
            // the thread that held the lock when the wait began.
            const std::thread::id holder = owner.load(std::memory_order_relaxed);
            const auto begin = std::chrono::steady_clock::now();
            WaitAndTake();
            LockStatsRecord(holder, std::chrono::steady_clock::now() - begin);
            return;
        }
        WaitAndTake();
    }

    void WaitAndTake() {
        for (int spin = 0; spin < SpinCount; ++spin) {
            Pause();
            if (state.load(std::memory_order_relaxed) == 0) {
                std::uint32_t expected = 0;
                if (state.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
                    return;
                }
            }
        }
        // 2: held with (possible) sleepers; taken this way it stays marked, so its unlock wakes
        // the next sleeper.
        while (state.exchange(2, std::memory_order_acquire) != 0) {
            state.wait(2, std::memory_order_relaxed);
        }
    }

    std::atomic<std::uint32_t> state{0}; ///< 0 free, 1 held, 2 held with sleepers
    std::atomic<std::thread::id> owner{};
    std::uint32_t depth{}; ///< Recursion depth, touched only by the owner
};

} // namespace Common

#include "common/spin_recursive_mutex_stats.h"
