// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2.h"
#include "video_core/vulkan_common/vulkan.h"

/// Batch 8 U4: VRAM pressure at 2x (docs/batch8-plan.md). Every piece has its own switch
/// (EDEN_NX2_VRAM_<X>: '0' off, '1' on, 'a' alternating where noted, else the default); the
/// modes are read once.
namespace Vulkan::Lite8Vram {

/// EDEN_NX2_VRAM_TIMING (default on): VMA's vkAllocateMemory / vkFreeMemory go through the
/// counting wrappers below (count, bytes, dedicated, latency; a warning for each allocation
/// that takes >= 50 ms, the first 64). Pure measurement. Decided when the device is created.
[[nodiscard]] inline bool TimingEnabled() {
    static const bool on = Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_VRAM_TIMING"), true);
    return on;
}

/// EDEN_NX2_VRAM_STATS=1 (default off): "VRAM in 10 s" line (per device-local heap driver
/// usage/budget, VMA blocks and slack, allocation counters, texture cache thresholds).
[[nodiscard]] inline bool StatsEnabled() {
    static const bool on = Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_VRAM_STATS"), false);
    return on;
}

/// EDEN_NX2_VRAM_NODEDICATED (default off; 'a' alternates per image): images whose memory
/// requirement is at most EDEN_NX2_VRAM_NODEDICATED_KIB (default 4096) are suballocated from
/// VMA's blocks even when the driver prefers a dedicated allocation (it never requires one for
/// these images). One vkAllocateMemory per 16 MiB block instead of one per image. Risk: the
/// driver may want dedicated memory for render-target compression (bandwidth).
[[nodiscard]] inline char NoDedicatedMode() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_VRAM_NODEDICATED");
    return mode;
}
[[nodiscard]] inline u64 NoDedicatedLimit() {
    static const u64 limit = [] {
        const char* value = std::getenv("EDEN_NX2_VRAM_NODEDICATED_KIB");
        const u64 kib = value != nullptr ? std::strtoull(value, nullptr, 10) : 4096;
        // Over half a 16 MiB block VMA makes it dedicated anyway.
        return (kib != 0 ? kib : 4096) << 10;
    }();
    return limit;
}

/// EDEN_NX2_VRAM_STAGING_KEEP (default '1'): the staging pool's level sweep frees a free
/// buffer only once it has been unused for 600 frames, instead of whenever the sweep reaches
/// it (every 64 frames: the steady-state set was freed and allocated again, each time a driver
/// allocation, dedicated ones over 8 MiB). '1' host-visible pools (Upload/Download: no VRAM),
/// '2' also the device-local pool (BC encoder output, vertex/index repacks: holds VRAM),
/// '0' the old sweep.
[[nodiscard]] inline char StagingKeepMode() {
    static const char mode = [] {
        const char value = Tegra::Nx2EnvMode("EDEN_NX2_VRAM_STAGING_KEEP");
        return value == '0' || value == '1' || value == '2' ? value : '1';
    }();
    return mode;
}
inline constexpr u64 StagingKeepFrames = 600;

/// EDEN_NX2_VRAM_SLOWPRESSURE (default off): a vkAllocateMemory that took >= 20 ms counts as
/// a memory pressure event for the texture cache (like an image that did not fit: it keeps
/// 512 MiB more free, decaying after quiet frames). Slow driver allocations mean the heap is at
/// its budget and the driver is evicting. Risk: more evictions / re-uploads.
[[nodiscard]] inline bool SlowPressureEnabled() {
    static const bool on =
        Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_VRAM_SLOWPRESSURE"), false);
    return on;
}

/// EDEN_NX2_VRAM_DECODE_TRIM (default off): ASTC persistent decode images (one per power-of-
/// two size class, never freed: up to ~350 MiB with every class from 64 to 4096) of 16 MiB or
/// more are freed after 3600 frames unused.
[[nodiscard]] inline bool DecodeTrimEnabled() {
    static const bool on = Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_VRAM_DECODE_TRIM"), false);
    return on;
}
inline constexpr u64 DecodeTrimFrames = 3600;
inline constexpr u64 DecodeTrimMinBytes = u64{16} << 20;

/// Counters of the wrapped driver calls (since start; the stats line prints differences).
struct Counters {
    std::atomic<u64> allocs{};
    std::atomic<u64> alloc_bytes{};
    std::atomic<u64> dedicated{};
    std::atomic<u64> dedicated_bytes{};
    std::atomic<u64> failed{};
    std::atomic<u64> alloc_ns{};
    std::atomic<u64> max_alloc_ns{}; ///< Since the last stats line (exchanged to 0 there)
    std::atomic<u64> slow{};         ///< Allocations >= 20 ms
    std::atomic<u64> frees{};
    std::atomic<u64> free_ns{};
    std::atomic<u64> nodedicated_images{}; ///< Images NODEDICATED suballocated
};
inline Counters g_counters;
inline constexpr u64 SlowAllocNs = 20'000'000;

/// The driver's functions per device (a process may make more than one device).
struct RealFunctions {
    std::atomic<VkDevice> device{};
    PFN_vkAllocateMemory allocate{};
    PFN_vkFreeMemory free{};
};
inline std::array<RealFunctions, 4> g_real{};

[[nodiscard]] inline const RealFunctions* FindReal(VkDevice device) {
    for (const RealFunctions& entry : g_real) {
        if (entry.device.load(std::memory_order_acquire) == device) {
            return &entry;
        }
    }
    return nullptr;
}

inline VKAPI_ATTR VkResult VKAPI_CALL CountingAllocateMemory(VkDevice device,
                                                             const VkMemoryAllocateInfo* info,
                                                             const VkAllocationCallbacks* callbacks,
                                                             VkDeviceMemory* memory) {
    const RealFunctions* real = FindReal(device);
    if (real == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto start = std::chrono::steady_clock::now();
    const VkResult result = real->allocate(device, info, callbacks, memory);
    const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
    g_counters.alloc_ns.fetch_add(ns, std::memory_order_relaxed);
    u64 max = g_counters.max_alloc_ns.load(std::memory_order_relaxed);
    while (ns > max && !g_counters.max_alloc_ns.compare_exchange_weak(max, ns,
                                                                       std::memory_order_relaxed)) {
    }
    if (result != VK_SUCCESS) {
        g_counters.failed.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    g_counters.allocs.fetch_add(1, std::memory_order_relaxed);
    g_counters.alloc_bytes.fetch_add(info->allocationSize, std::memory_order_relaxed);
    bool dedicated = false;
    for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next != nullptr;
         next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
            const auto* dedicated_info = reinterpret_cast<const VkMemoryDedicatedAllocateInfo*>(next);
            dedicated = dedicated_info->image != VK_NULL_HANDLE ||
                        dedicated_info->buffer != VK_NULL_HANDLE;
        }
    }
    if (dedicated) {
        g_counters.dedicated.fetch_add(1, std::memory_order_relaxed);
        g_counters.dedicated_bytes.fetch_add(info->allocationSize, std::memory_order_relaxed);
    }
    if (ns >= SlowAllocNs) {
        g_counters.slow.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<u32> logged{};
        if (ns >= 50'000'000 && logged.fetch_add(1, std::memory_order_relaxed) < 64) {
            LOG_WARNING(Render_Vulkan,
                        "vkAllocateMemory took {} ms ({} MiB, type {}, {})", ns / 1'000'000,
                        info->allocationSize >> 20, info->memoryTypeIndex,
                        dedicated ? "dedicated" : "block");
        }
    }
    return result;
}

inline VKAPI_ATTR void VKAPI_CALL CountingFreeMemory(VkDevice device, VkDeviceMemory memory,
                                                     const VkAllocationCallbacks* callbacks) {
    const RealFunctions* real = FindReal(device);
    if (real == nullptr) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    real->free(device, memory, callbacks);
    g_counters.free_ns.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                      std::chrono::steady_clock::now() - start)
                                                      .count()),
                                 std::memory_order_relaxed);
    if (memory != VK_NULL_HANDLE) {
        g_counters.frees.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Registers the device's real functions; true when the counting wrappers may be handed to VMA
/// (false: no free slot or no functions, keep the real ones).
[[nodiscard]] inline bool InstallCounting(VkDevice device, PFN_vkAllocateMemory allocate,
                                          PFN_vkFreeMemory free) {
    if (!TimingEnabled() || device == VK_NULL_HANDLE || allocate == nullptr || free == nullptr) {
        return false;
    }
    RealFunctions* slot = nullptr;
    for (RealFunctions& entry : g_real) {
        const VkDevice current = entry.device.load(std::memory_order_acquire);
        if (current == device || (slot == nullptr && current == VK_NULL_HANDLE)) {
            slot = &entry;
        }
    }
    if (slot == nullptr) {
        // A handle of a destroyed device may be taken over: the oldest slot.
        slot = &g_real[0];
    }
    slot->device.store(VK_NULL_HANDLE, std::memory_order_release);
    slot->allocate = allocate;
    slot->free = free;
    slot->device.store(device, std::memory_order_release);
    return true;
}

/// Allocations that took >= 20 ms so far (EDEN_NX2_VRAM_SLOWPRESSURE reads this).
[[nodiscard]] inline u64 SlowAllocations() noexcept {
    return g_counters.slow.load(std::memory_order_relaxed);
}

} // namespace Vulkan::Lite8Vram
