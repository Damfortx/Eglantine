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

/// EDEN_NX2_C11_LOCKRUN (cycle 11 W4b; on by default with EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1, '0'
/// off, '1' or 'a' on): the lock's owner check compares a thread_local copy of the calling
/// thread's id instead of calling std::this_thread::get_id() on every lock (the committer locks
/// both cache mutexes a few times per draw, recursively under a run). The run itself is in
/// video_core/mt/c11_commit.h.
inline const bool g_lock_tls_id = [] {
    const char* run = std::getenv("EDEN_NX2_C11_LOCKRUN");
    if (run != nullptr && (run[0] == '0' || run[0] == '1' || run[0] == 'a')) {
        return run[0] != '0';
    }
    const char* mt = std::getenv("EDEN_NX2_MT");
    const char* lite = std::getenv("EDEN_NX2_MT_LITE");
    return mt != nullptr && mt[0] == '1' && lite != nullptr && lite[0] == '1';
}();

/// Records the calling thread's name for the owner lookup (once per thread).
inline void LockStatsRegisterThread();
/// Adds a wait of the calling thread on the thread that owned the lock when it began.
inline void LockStatsRecord(std::thread::id owner, std::chrono::steady_clock::duration wait,
                            const char* lock_name = nullptr);

/// A recursive mutex that spins briefly before sleeping. The GPU caches' locks are taken tens of
/// thousands of times a second by the GPU thread, the fencing thread and CPU write traps, and
/// held for microseconds: std::recursive_mutex put a waiting thread to sleep at once, and its
/// wake-up took longer than the critical section (title 1's GPU thread spent ~8% asleep there).
class SpinRecursiveMutex {
public:
    SpinRecursiveMutex() = default;
    /// `name` tells this lock's waits apart from the other locks' in EDEN_NX2_LOCK_STATS.
    explicit SpinRecursiveMutex(const char* name) : stats_name{name} {}

    void lock() {
        const std::thread::id self = SelfId();
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
        const std::thread::id self = SelfId();
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

    /// Whether the calling thread holds it (diagnostics: EDEN_NX2_MT_LITE7_CAPTURE_CHECK).
    [[nodiscard]] bool HeldByCurrentThread() const noexcept {
        return owner.load(std::memory_order_relaxed) == SelfId();
    }

    /// Whether another thread is waiting for it now (spinning or asleep): a holder that keeps it
    /// across several pieces of work (EDEN_NX2_C11_LOCKRUN) lets it go between them then.
    [[nodiscard]] bool Contended() const noexcept {
        return waiters.load(std::memory_order_relaxed) != 0;
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
    /// The calling thread's id (EDEN_NX2_C11_LOCKRUN: a thread_local copy; zero-initialized, so
    /// reading it needs no TLS guard).
    static std::thread::id SelfId() noexcept {
        if (!g_lock_tls_id) {
            return std::this_thread::get_id();
        }
        thread_local std::thread::id cached{};
        if (cached == std::thread::id{}) {
            cached = std::this_thread::get_id();
        }
        return cached;
    }

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
        waiters.fetch_add(1, std::memory_order_relaxed);
        struct WaiterExit {
            std::atomic<std::uint32_t>& count;
            ~WaiterExit() {
                count.fetch_sub(1, std::memory_order_relaxed);
            }
        } waiter_exit{waiters};
        if (g_lock_stats_enabled) [[unlikely]] {
            // The owner may not be stored yet (just taken) or may change while we wait: this is
            // the thread that held the lock when the wait began.
            const std::thread::id holder = owner.load(std::memory_order_relaxed);
            const auto begin = std::chrono::steady_clock::now();
            WaitAndTake();
            LockStatsRecord(holder, std::chrono::steady_clock::now() - begin, stats_name);
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
    std::atomic<std::uint32_t> waiters{0}; ///< Threads in LockContended (Contended())
    std::uint32_t depth{}; ///< Recursion depth, touched only by the owner
    const char* stats_name{};
};

} // namespace Common

#include "common/spin_recursive_mutex_stats.h"
