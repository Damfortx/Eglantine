// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <utility>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2.h"

/// Cycle 10 A: VRAM placement guard (docs/cycle10-plan.md). At 2x on an 8 GB card the process
/// sits at the driver budget; images and device-local buffers were created with only a
/// *preferred* DEVICE_LOCAL flag, so when the device heap was full VMA quietly placed them in
/// system memory for good (no pressure event, no warning): GPU 29 ms/frame waiting on PCIe,
/// 26-32 fps instead of ~50, 3-6 fps at worst. Backend neutral (TextureCache<P> includes it).
namespace VideoCommon::C10Vram {

/// EDEN_NX2_VRAM_GUARD (default on; '0' the old placement): images and device-local buffers
/// require DEVICE_LOCAL memory first (within the budget, then past it); only when that fails
/// they go to host memory (counted, warned about). A failed device-local attempt asks the
/// texture cache for an emergency collection, and the cache moves images that ended up in host
/// memory back to VRAM once there is room. Read once.
[[nodiscard]] inline bool GuardOn() {
    static const bool on = Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_VRAM_GUARD"), true);
    return on;
}

/// EDEN_NX2_VRAM_GUARD_RELOCATE (default 8, 0 off): host-memory images the texture cache evicts
/// per frame (unused this frame) so they are re-created in VRAM, while under critical.
[[nodiscard]] inline u32 RelocateLimit() {
    static const u32 limit = [] {
        const char* value = std::getenv("EDEN_NX2_VRAM_GUARD_RELOCATE");
        if (value == nullptr || value[0] < '0' || value[0] > '9') {
            return 8U;
        }
        return static_cast<u32>(std::strtoul(value, nullptr, 10));
    }();
    return limit;
}

enum class Kind : u8 { Image, Buffer };

struct Counters {
    // Current, for the resources whose owner holds a HostResidency (texture cache images and
    // buffer cache buffers).
    std::atomic<u64> host_images{};
    std::atomic<u64> host_image_bytes{};
    std::atomic<u64> host_buffers{};
    std::atomic<u64> host_buffer_bytes{};
    // Since start (any caller of MemoryAllocator).
    std::atomic<u64> images_placed_host{};
    std::atomic<u64> image_bytes_placed_host{};
    std::atomic<u64> buffers_placed_host{};
    std::atomic<u64> buffer_bytes_placed_host{};
    std::atomic<u64> over_budget{};   ///< Device-local within budget failed (placed past it)
    std::atomic<u64> device_full{};   ///< Device-local failed altogether (host fallback)
    std::atomic<u64> emergencies{};   ///< Emergency collections the cache started
    std::atomic<u64> relocated{};     ///< Host-memory images evicted to be re-created in VRAM
    std::atomic<u64> relocated_bytes{};
};
inline Counters g_counters;

/// Set by the allocator (any thread) when a device-local allocation failed; consumed by the
/// texture cache at its next frame tick.
inline std::atomic<bool> g_emergency_collect{false};

inline void RequestEmergencyCollect() noexcept {
    g_emergency_collect.store(true, std::memory_order_relaxed);
}

[[nodiscard]] inline bool ConsumeEmergencyCollect() noexcept {
    return g_emergency_collect.load(std::memory_order_relaxed) &&
           g_emergency_collect.exchange(false, std::memory_order_relaxed);
}

/// Records a resource the allocator placed in host memory (created-since-start counters).
inline void CountPlacedHost(Kind kind, u64 bytes) noexcept {
    if (kind == Kind::Image) {
        g_counters.images_placed_host.fetch_add(1, std::memory_order_relaxed);
        g_counters.image_bytes_placed_host.fetch_add(bytes, std::memory_order_relaxed);
    } else {
        g_counters.buffers_placed_host.fetch_add(1, std::memory_order_relaxed);
        g_counters.buffer_bytes_placed_host.fetch_add(bytes, std::memory_order_relaxed);
    }
}

/// Held by the owner of a resource in host memory: counts it as currently host resident until
/// destroyed (or Set again). Move-only; zero bytes = device memory / nothing.
class HostResidency {
public:
    HostResidency() noexcept = default;
    ~HostResidency() {
        Reset();
    }
    HostResidency(const HostResidency&) = delete;
    HostResidency& operator=(const HostResidency&) = delete;
    HostResidency(HostResidency&& rhs) noexcept
        : bytes{std::exchange(rhs.bytes, 0)}, kind{rhs.kind} {}
    HostResidency& operator=(HostResidency&& rhs) noexcept {
        if (this != &rhs) {
            Reset();
            bytes = std::exchange(rhs.bytes, 0);
            kind = rhs.kind;
        }
        return *this;
    }

    void Set(Kind kind_, u64 bytes_) noexcept {
        Reset();
        if (bytes_ == 0) {
            return;
        }
        kind = kind_;
        bytes = bytes_;
        Current(1, bytes);
    }

    void Reset() noexcept {
        if (bytes != 0) {
            Current(static_cast<u64>(-1), static_cast<u64>(0) - bytes);
            bytes = 0;
        }
    }

    [[nodiscard]] u64 Bytes() const noexcept {
        return bytes;
    }

private:
    void Current(u64 count, u64 delta) const noexcept {
        // Unsigned wrap-around adds or subtracts.
        auto& c = g_counters;
        (kind == Kind::Image ? c.host_images : c.host_buffers)
            .fetch_add(count, std::memory_order_relaxed);
        (kind == Kind::Image ? c.host_image_bytes : c.host_buffer_bytes)
            .fetch_add(delta, std::memory_order_relaxed);
    }

    u64 bytes{};
    Kind kind{Kind::Image};
};

/// One always-on warning at most every 60 s while resources live in host memory (the slow-mode
/// signature). Call once a frame from one thread.
inline void TickWarning() {
    if (!GuardOn()) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    static u64 last_placed{};
    const Clock::time_point now = Clock::now();
    if (now - last < std::chrono::seconds{60}) {
        return;
    }
    auto& c = g_counters;
    const u64 placed = c.images_placed_host.load(std::memory_order_relaxed) +
                       c.buffers_placed_host.load(std::memory_order_relaxed);
    const u64 images = c.host_images.load(std::memory_order_relaxed);
    const u64 buffers = c.host_buffers.load(std::memory_order_relaxed);
    const u64 bytes = c.host_image_bytes.load(std::memory_order_relaxed) +
                      c.host_buffer_bytes.load(std::memory_order_relaxed);
    if (bytes == 0 && placed == last_placed) {
        return;
    }
    last = now;
    LOG_WARNING(Render_Vulkan,
                "{} images / {} buffers ({} MiB) in system memory ({} placed there in the last "
                "minute): GPU slowed; close other GPU programs or lower resolution",
                images, buffers, bytes >> 20, placed - last_placed);
    last_placed = placed;
}

} // namespace VideoCommon::C10Vram
