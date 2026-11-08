// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <memory>
#include <new>
#include <thread>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

#include "common/polyfill_thread.h"

namespace Common {

namespace detail {
constexpr size_t DefaultCapacity = 0x1000;

/// CPU hint for spin-wait loops (cheaper for the sibling hyperthread than a plain busy loop).
inline void SpinPause() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}
} // namespace detail

template <typename T, size_t Capacity = detail::DefaultCapacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two.");

public:
    template <typename... Args>
    bool TryEmplace(Args&&... args) noexcept {
        return Emplace<PushMode::Try>(std::forward<Args>(args)...);
    }

    template <typename... Args>
    void EmplaceWait(Args&&... args) noexcept {
        Emplace<PushMode::Wait>(std::forward<Args>(args)...);
    }

    bool TryPop(T& t) noexcept {
        return Pop<PopMode::Try>(t);
    }

    void PopWait(T& t) noexcept {
        Pop<PopMode::Wait>(t);
    }

    void PopWait(T& t, const std::stop_token stop_token) noexcept {
        Pop<PopMode::WaitWithStopToken>(t, stop_token);
    }

    T PopWait() noexcept {
        T t{};
        Pop<PopMode::Wait>(t);
        return t;
    }

    T PopWait(const std::stop_token stop_token) noexcept {
        T t{};
        Pop<PopMode::WaitWithStopToken>(t, stop_token);
        return t;
    }

    // "Fast" variants (used by the GPU thread queue only). The plain push locks the consumer's
    // cv mutex and notifies on every element, and the plain pop does the same to the producer:
    // two mutex round trips plus a possible futex wake per command. The fast variants touch the
    // other side's mutex/cv only when that side has announced it is (about to be) blocked via
    // consumer_waiting / producer_waiting. Handshake (Dekker style, all seq_cst):
    //   publisher: store index; if (other_waiting.load()) { lock; notify; }
    //   waiter:    lock; other_waiting.store(true); recheck index in the cv predicate; wait.
    // Either the waiter's recheck sees the new index, or the publisher sees the flag and
    // notifies under the mutex (which the waiter holds until it is inside wait()), so no wakeup
    // is lost. The plain wait paths also set the flags, so fast and plain calls may be mixed
    // (e.g. a switch that alternates at run time).

    template <typename... Args>
    void EmplaceWaitFast(Args&&... args) noexcept {
        Emplace<PushMode::WaitFast>(std::forward<Args>(args)...);
    }

    bool TryPopFast(T& t) noexcept {
        return Pop<PopMode::TryFast>(t);
    }

    /// Spins (with a pause hint) for up to `spin` checking for an element before sleeping on the
    /// cv: the producer usually pushes again within microseconds, and a futex sleep/wake costs
    /// more than that. Returns false only if stop was requested.
    bool PopSpinWait(T& t, std::stop_token stop_token, std::chrono::nanoseconds spin) noexcept {
        const std::size_t read_index = consumer.index.load(std::memory_order::relaxed);
        if (read_index == producer.index.load(std::memory_order::acquire)) {
            const auto deadline = std::chrono::steady_clock::now() + spin;
            for (unsigned i = 0;; ++i) {
                detail::SpinPause();
                if (read_index != producer.index.load(std::memory_order::acquire)) {
                    break;
                }
                // Read the clock only every 64 pauses (a pause is ~10-140 cycles).
                if ((i & 63) == 63 && std::chrono::steady_clock::now() >= deadline) {
                    break;
                }
            }
        }
        return Pop<PopMode::WaitFast>(t, stop_token);
    }

    /// Blocks until the queue is non-empty, `timeout` elapses or stop is requested, without
    /// popping. Returns whether an element is available. Works with both push variants.
    template <typename Rep, typename Period>
    bool WaitNonEmptyFor(const std::chrono::duration<Rep, Period>& timeout,
                         std::stop_token stop_token) noexcept {
        const std::size_t read_index = consumer.index.load(std::memory_order::relaxed);
        if (read_index != producer.index.load(std::memory_order::acquire)) {
            return true;
        }
        std::unique_lock lock{consumer.cv_mutex};
        consumer_waiting.store(true, std::memory_order::seq_cst);
        const bool ready = consumer.cv.wait_for(lock, stop_token, timeout, [this, read_index] {
            return read_index != producer.index.load(std::memory_order::seq_cst);
        });
        consumer_waiting.store(false, std::memory_order::relaxed);
        return ready;
    }

private:

    enum class PushMode {
        Try,
        Wait,
        WaitFast,
        Count,
    };

    enum class PopMode {
        Try,
        Wait,
        WaitWithStopToken,
        TryFast,
        WaitFast,
        Count,
    };

    template <PushMode Mode, typename... Args>
    bool Emplace(Args&&... args) noexcept {
        const std::size_t write_index = producer.index.load(std::memory_order::relaxed);
        if constexpr (Mode == PushMode::Try) {
            // Check if we have free slots to write to.
            if ((write_index - consumer.index.load(std::memory_order::acquire)) == Capacity) {
                return false;
            }
        } else if constexpr (Mode == PushMode::Wait) {
            // Wait until we have free slots to write to.
            std::unique_lock lock{producer.cv_mutex};
            producer_waiting.store(true, std::memory_order::seq_cst);
            producer.cv.wait(lock, [this, write_index] {
                return (write_index - consumer.index.load(std::memory_order::seq_cst)) < Capacity;
            });
            producer_waiting.store(false, std::memory_order::relaxed);
        } else if constexpr (Mode == PushMode::WaitFast) {
            // Only take the mutex when the queue is full (rare).
            if ((write_index - consumer.index.load(std::memory_order::acquire)) >= Capacity) {
                std::unique_lock lock{producer.cv_mutex};
                producer_waiting.store(true, std::memory_order::seq_cst);
                producer.cv.wait(lock, [this, write_index] {
                    return (write_index - consumer.index.load(std::memory_order::seq_cst)) <
                           Capacity;
                });
                producer_waiting.store(false, std::memory_order::relaxed);
            }
        } else {
            static_assert(Mode < PushMode::Count, "Invalid PushMode.");
        }
        // Determine the position to write to.
        const std::size_t pos = write_index % Capacity;
        // Emplace into the queue.
        std::construct_at(std::addressof(m_data[pos]), std::forward<Args>(args)...);
        // Increment the write index (seq_cst: ordered before the consumer_waiting load below).
        ++producer.index;
        if constexpr (Mode == PushMode::WaitFast) {
            // Wake the consumer only if it announced it is going to sleep.
            if (!consumer_waiting.load(std::memory_order::seq_cst)) {
                return true;
            }
        }
        // Notify the consumer that we have pushed into the queue.
        std::scoped_lock lock{consumer.cv_mutex};
        consumer.cv.notify_one();
        return true;
    }

    template <PopMode Mode>
    bool Pop(T& t, [[maybe_unused]] std::stop_token stop_token = {}) noexcept {
        const std::size_t read_index = consumer.index.load(std::memory_order::relaxed);
        if constexpr (Mode == PopMode::Try || Mode == PopMode::TryFast) {
            // Check if the queue is empty.
            if (read_index == producer.index.load(std::memory_order::acquire)) {
                return false;
            }
        } else if constexpr (Mode == PopMode::Wait) {
            // Wait until the queue is not empty.
            std::unique_lock lock{consumer.cv_mutex};
            consumer_waiting.store(true, std::memory_order::seq_cst);
            consumer.cv.wait(lock, [this, read_index] {
                return read_index != producer.index.load(std::memory_order::seq_cst);
            });
            consumer_waiting.store(false, std::memory_order::relaxed);
        } else if constexpr (Mode == PopMode::WaitWithStopToken || Mode == PopMode::WaitFast) {
            // Wait until the queue is not empty (WaitFast: skip the mutex if it already isn't).
            if (Mode != PopMode::WaitFast ||
                read_index == producer.index.load(std::memory_order::acquire)) {
                std::unique_lock lock{consumer.cv_mutex};
                consumer_waiting.store(true, std::memory_order::seq_cst);
                consumer.cv.wait(lock, stop_token, [this, read_index] {
                    return read_index != producer.index.load(std::memory_order::seq_cst);
                });
                consumer_waiting.store(false, std::memory_order::relaxed);
            }
            if constexpr (Mode == PopMode::WaitWithStopToken) {
                if (stop_token.stop_requested()) {
                    return false;
                }
            } else if (read_index == producer.index.load(std::memory_order::acquire)) {
                return false; // Woken by a stop request with nothing to pop.
            }
        } else {
            static_assert(Mode < PopMode::Count, "Invalid PopMode.");
        }
        // Determine the position to read from.
        const std::size_t pos = read_index % Capacity;
        // Pop the data off the queue, moving it.
        t = std::move(m_data[pos]);
        // Increment the read index (seq_cst: ordered before the producer_waiting load below).
        ++consumer.index;
        if constexpr (Mode == PopMode::TryFast || Mode == PopMode::WaitFast) {
            // Wake the producer only if it is blocked on a full queue.
            if (!producer_waiting.load(std::memory_order::seq_cst)) {
                return true;
            }
        }
        // Notify the producer that we have popped off the queue.
        std::scoped_lock lock{producer.cv_mutex};
        producer.cv.notify_one();
        return true;
    }

    std::array<T, Capacity> m_data;
    alignas(64) struct {
        std::atomic_size_t index{0};
        std::condition_variable_any cv;
        std::mutex cv_mutex;
    } producer;
    alignas(64) struct {
        std::atomic_size_t index{0};
        std::condition_variable_any cv;
        std::mutex cv_mutex;
    } consumer;
    // Set (under the respective cv mutex) while a side is blocked; see the "Fast" variants.
    alignas(64) std::atomic_bool consumer_waiting{false};
    alignas(64) std::atomic_bool producer_waiting{false};
};

template <typename T, size_t Capacity = detail::DefaultCapacity>
class MPSCQueue {
public:
    template <typename... Args>
    bool TryEmplace(Args&&... args) {
        std::scoped_lock lock{write_mutex};
        return spsc_queue.TryEmplace(std::forward<Args>(args)...);
    }

    template <typename... Args>
    void EmplaceWait(Args&&... args) {
        std::scoped_lock lock{write_mutex};
        spsc_queue.EmplaceWait(std::forward<Args>(args)...);
    }

    bool TryPop(T& t) {
        return spsc_queue.TryPop(t);
    }

    void PopWait(T& t) {
        spsc_queue.PopWait(t);
    }

    void PopWait(T& t, std::stop_token stop_token) {
        spsc_queue.PopWait(t, stop_token);
    }

    T PopWait() {
        return spsc_queue.PopWait();
    }

    T PopWait(std::stop_token stop_token) {
        return spsc_queue.PopWait(stop_token);
    }

private:
    SPSCQueue<T, Capacity> spsc_queue;
    std::mutex write_mutex;
};

template <typename T, size_t Capacity = detail::DefaultCapacity>
class MPMCQueue {
public:
    template <typename... Args>
    bool TryEmplace(Args&&... args) {
        std::scoped_lock lock{write_mutex};
        return spsc_queue.TryEmplace(std::forward<Args>(args)...);
    }

    template <typename... Args>
    void EmplaceWait(Args&&... args) {
        std::scoped_lock lock{write_mutex};
        spsc_queue.EmplaceWait(std::forward<Args>(args)...);
    }

    bool TryPop(T& t) {
        std::scoped_lock lock{read_mutex};
        return spsc_queue.TryPop(t);
    }

    void PopWait(T& t) {
        std::scoped_lock lock{read_mutex};
        spsc_queue.PopWait(t);
    }

    void PopWait(T& t, std::stop_token stop_token) {
        std::scoped_lock lock{read_mutex};
        spsc_queue.PopWait(t, stop_token);
    }

    T PopWait() {
        std::scoped_lock lock{read_mutex};
        return spsc_queue.PopWait();
    }

    T PopWait(std::stop_token stop_token) {
        std::scoped_lock lock{read_mutex};
        return spsc_queue.PopWait(stop_token);
    }

private:
    SPSCQueue<T, Capacity> spsc_queue;
    alignas(64) std::mutex write_mutex;
    alignas(64) std::mutex read_mutex;
};

} // namespace Common
