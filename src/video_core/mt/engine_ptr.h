// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "common/assert.h"
#include "common/common_types.h"

namespace VideoCommon::MT {

/// Per thread, per engine type: which real engines this thread sees as a mirror instead.
template <class T>
struct EngineOverrides {
    static constexpr size_t Max = 8;
    struct Pair {
        const T* real;
        T* mirror;
    };
    std::array<Pair, Max> pairs{};
    size_t count = 0;
    /// EDEN_NX2_C11_CHANCMD (mt/c11_serial.h): the committer's current channel's mirror. While
    /// set (and `count` != 0, so a Serial that clears `count` runs against the real engines), any
    /// non-null pointer resolves to it: the sequencer switches the rasterizer's stored pointers
    /// at a channel bind before the committer reaches it, so they may name the next channel.
    T* pinned = nullptr;
};

template <class T>
inline thread_local EngineOverrides<T> g_engine_overrides{};

/// Stands in for a `T*` engine pointer held by caches/pipelines. Behaves like the raw pointer,
/// except that a thread with a ScopedEngineOverride for that engine gets its mirror: the
/// committer and resolvers run the existing cache code against their own register copies.
template <class T>
class ThreadEnginePtr {
public:
    constexpr ThreadEnginePtr() noexcept = default;
    constexpr ThreadEnginePtr(T* ptr_) noexcept : ptr{ptr_} {}
    constexpr ThreadEnginePtr(std::nullptr_t) noexcept {}

    ThreadEnginePtr& operator=(T* ptr_) noexcept {
        ptr = ptr_;
        return *this;
    }

    [[nodiscard]] T* Get() const noexcept {
        const EngineOverrides<T>& overrides = g_engine_overrides<T>;
        if (overrides.count != 0) [[unlikely]] {
            if (overrides.pinned != nullptr && ptr != nullptr) {
                return overrides.pinned;
            }
            for (size_t i = 0; i < overrides.count; ++i) {
                if (overrides.pairs[i].real == ptr) {
                    return overrides.pairs[i].mirror;
                }
            }
        }
        return ptr;
    }

    /// The pointer as stored, ignoring overrides.
    [[nodiscard]] T* Real() const noexcept {
        return ptr;
    }

    T* operator->() const noexcept {
        return Get();
    }
    T& operator*() const noexcept {
        return *Get();
    }
    operator T*() const noexcept {
        return Get();
    }
    explicit operator bool() const noexcept {
        return ptr != nullptr;
    }
    friend bool operator==(const ThreadEnginePtr& lhs, std::nullptr_t) noexcept {
        return lhs.ptr == nullptr;
    }
    friend bool operator==(const ThreadEnginePtr& lhs, const T* rhs) noexcept {
        return lhs.Get() == rhs;
    }

private:
    T* ptr = nullptr;
};

/// EDEN_NX2_C11_CHANCMD: pins this thread's view of every engine of type T (null: unpinned).
template <class T>
inline void PinEngineView(T* mirror) noexcept {
    g_engine_overrides<T>.pinned = mirror;
}

/// While alive, ThreadEnginePtr<T>s pointing at `real` resolve to `mirror` on this thread.
template <class T>
class ScopedEngineOverride {
public:
    ScopedEngineOverride(const T* real, T* mirror) {
        EngineOverrides<T>& overrides = g_engine_overrides<T>;
        ASSERT(overrides.count < EngineOverrides<T>::Max);
        overrides.pairs[overrides.count++] = {real, mirror};
    }
    ~ScopedEngineOverride() {
        --g_engine_overrides<T>.count;
    }
    ScopedEngineOverride(const ScopedEngineOverride&) = delete;
    ScopedEngineOverride& operator=(const ScopedEngineOverride&) = delete;
};

} // namespace VideoCommon::MT
