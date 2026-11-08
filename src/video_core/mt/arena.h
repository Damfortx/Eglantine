// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <bit>
#include <memory>
#include <span>
#include <thread>

#include "common/assert.h"
#include "common/common_types.h"

namespace VideoCommon::MT {

/// Ring of T with one producer handing out contiguous spans and one releaser freeing them in
/// order (e.g. a resolver writing descriptor entries that the worker releases after recording).
/// Spans never wrap: when the tail of the ring is too short, it is skipped.
template <class T>
class RingArena {
public:
    explicit RingArena(size_t capacity_)
        : capacity{std::bit_ceil(capacity_)}, storage{std::make_unique<T[]>(capacity)} {}

    RingArena(const RingArena&) = delete;
    RingArena& operator=(const RingArena&) = delete;

    /// Producer. Blocks (spins, then yields) while the releaser has not freed enough. Returns the
    /// span and its end position, to pass to Release once the span is no longer read.
    [[nodiscard]] std::span<T> Allocate(size_t count, u64& end_position) {
        ASSERT(count <= capacity);
        u64 start = head;
        const size_t offset = static_cast<size_t>(start & (capacity - 1));
        if (offset + count > capacity) {
            start += capacity - offset; // skip the tail
        }
        const u64 end = start + count;
        for (u32 spins = 0; end - released.load(std::memory_order_acquire) > capacity; ++spins) {
            if (spins > 64) {
                std::this_thread::yield();
            }
        }
        head = end;
        end_position = end;
        return std::span<T>{storage.get() + (start & (capacity - 1)), count};
    }

    /// Producer, non-blocking: an empty span when the releaser has not freed enough yet.
    [[nodiscard]] std::span<T> TryAllocate(size_t count, u64& end_position) {
        if (count > capacity) {
            return {};
        }
        u64 start = head;
        const size_t offset = static_cast<size_t>(start & (capacity - 1));
        if (offset + count > capacity) {
            start += capacity - offset; // skip the tail
        }
        const u64 end = start + count;
        if (end - released.load(std::memory_order_acquire) > capacity) {
            return {};
        }
        head = end;
        end_position = end;
        return std::span<T>{storage.get() + (start & (capacity - 1)), count};
    }

    /// Producer: the position the next allocation starts from (end of everything handed out).
    [[nodiscard]] u64 Head() const noexcept {
        return head;
    }

    /// Releaser: everything allocated up to `end_position` may be reused.
    void Release(u64 end_position) {
        u64 current = released.load(std::memory_order_relaxed);
        while (current < end_position &&
               !released.compare_exchange_weak(current, end_position, std::memory_order_release)) {
        }
    }

    [[nodiscard]] size_t Capacity() const noexcept {
        return capacity;
    }

    [[nodiscard]] u64 InFlight() const noexcept {
        return head - released.load(std::memory_order_acquire);
    }

private:
    const size_t capacity;
    std::unique_ptr<T[]> storage;
    u64 head = 0;
    alignas(64) std::atomic<u64> released{0};
};

/// Bump allocator reset as a whole (per batch scratch).
class BumpArena {
public:
    explicit BumpArena(size_t capacity_) : capacity{capacity_}, storage{std::make_unique<u8[]>(capacity_)} {}

    [[nodiscard]] void* Allocate(size_t size, size_t align = 16) {
        const size_t start = (used + align - 1) & ~(align - 1);
        if (start + size > capacity) {
            return nullptr;
        }
        used = start + size;
        return storage.get() + start;
    }

    template <class T>
    [[nodiscard]] T* AllocateArray(size_t count) {
        return static_cast<T*>(Allocate(sizeof(T) * count, alignof(T)));
    }

    void Reset() noexcept {
        used = 0;
    }

    [[nodiscard]] size_t Used() const noexcept {
        return used;
    }

private:
    size_t capacity;
    std::unique_ptr<u8[]> storage;
    size_t used = 0;
};

} // namespace VideoCommon::MT
