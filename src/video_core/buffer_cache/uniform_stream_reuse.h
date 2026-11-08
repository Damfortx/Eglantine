// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <span>
#include <type_traits>

#include "video_core/nx2.h"
#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon {

/// What a buffer cache runtime offers for stream uniform reuse.
///  - StreamUniformRef: a small trivially copyable description of one stream allocation (the
///    host buffer, its offset and whatever else the runtime needs to add it to a descriptor
///    payload again, e.g. the device address of the descriptor buffer path).
///  - LastStreamUniformRef(): the allocation made by the latest BindMappedUniformBuffer call.
///  - RebindStreamUniform(ref, size): adds `ref` to the descriptor payload exactly as
///    BindMappedUniformBuffer added the allocation the first time.
template <class R>
concept StreamUniformReusable = requires(R& runtime, u32 size) {
    typename R::StreamUniformRef;
    requires std::is_trivially_copyable_v<typename R::StreamUniformRef>;
    { runtime.LastStreamUniformRef() } -> std::same_as<typename R::StreamUniformRef>;
    runtime.RebindStreamUniform(runtime.LastStreamUniformRef(), size);
    { runtime.CurrentTick() } -> std::convertible_to<u64>;
};

/// EDEN_NX2_UBOREUSE=0 turns the reuse off (every small uniform buffer is copied, as before).
[[nodiscard]] inline bool UniformStreamReuseEnabled() noexcept {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_UBOREUSE");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

template <class Ref>
class UniformStreamReuse {
public:
    static constexpr u32 MaxBytes = 4096; // BufferCache's DEFAULT_SKIP_CACHE_SIZE
    static constexpr size_t NumStages = 5;
    static constexpr size_t NumSlots = 18; // Maxwell constant buffers per stage
    /// Consecutive misses after which a slot stops comparing and copies guest memory straight
    /// into the stream buffer (EDEN_NX2_UBOADAPT).
    static constexpr u32 BypassAfterMisses = 8;
    /// Binds in bypass between two probes through the reusing path.
    static constexpr u32 ProbeEvery = 64;

    struct Slot {
        Ref ref{};
        u64 tick{};
        u32 size{};         // 0: nothing streamed yet (or the slot is in bypass)
        u32 miss_streak{};  // consecutive binds that could not reuse the previous allocation
        u32 bypass_binds{}; // binds in bypass since the last probe
        std::array<u8, MaxBytes> bytes{};
    };

    /// EDEN_NX2_UBO_STATS: reuse hits, misses and bypassed binds, logged every 10 s.
    struct Stats {
        u64 hits{};
        u64 misses{};
        u64 bypassed{};
        u64 probes{};
        std::chrono::steady_clock::time_point window_start{std::chrono::steady_clock::now()};
    };

    void Reset() noexcept {
        for (auto& stage : slots) {
            for (Slot& slot : stage) {
                slot.size = 0;
                slot.tick = 0;
                slot.miss_streak = 0;
                slot.bypass_binds = 0;
            }
        }
    }

    std::array<std::array<Slot, NumSlots>, NumStages> slots{};
    std::array<u8, MaxBytes> scratch{};
    Stats stats{};
};

[[nodiscard]] inline bool UniformStreamStatsEnabled() noexcept {
    static const bool enabled = std::getenv("EDEN_NX2_UBO_STATS") != nullptr;
    return enabled;
}

/// Logs and restarts the EDEN_NX2_UBO_STATS window once 10 s have passed.
template <class Stats>
inline void LogUniformStreamStats(Stats& stats) {
    const auto now = std::chrono::steady_clock::now();
    if (now - stats.window_start < std::chrono::seconds{10}) {
        return;
    }
    const u64 total = stats.hits + stats.misses + stats.bypassed;
    const double hit_percent =
        total != 0 ? 100.0 * static_cast<double>(stats.hits) / static_cast<double>(total) : 0.0;
    LOG_INFO(Render_Vulkan,
             "Uniform stream reuse: {} binds, {} hits ({:.1f}%), {} misses, {} bypassed, {} probes",
             total, stats.hits, hit_percent, stats.misses, stats.bypassed, stats.probes);
    stats = Stats{};
    stats.window_start = now;
}

template <class Ref>
[[nodiscard]] inline UniformStreamReuse<Ref>& UniformStreamReuseState() noexcept {
    static UniformStreamReuse<Ref> state;
    return state;
}

/// Forgets every slot. Call it when a buffer cache (and so its runtime, scheduler and stream
/// buffer) is created: ticks restart with a new scheduler and the old references are dead.
template <class Runtime>
inline void ResetUniformStreamReuse() noexcept {
    if constexpr (StreamUniformReusable<Runtime>) {
        UniformStreamReuseState<typename Runtime::StreamUniformRef>().Reset();
    }
}

/// Binds the small uniform buffer at `device_addr` for (stage, cbuf `index`) through the stream
/// buffer, reusing the slot's previous allocation when the bytes and the command buffer are the
/// same. Returns false (nothing bound) when the buffer does not qualify, so the caller binds it
/// the classic way.
template <class Runtime, class DeviceMemory>
[[nodiscard]] bool BindSmallUniformReusing(Runtime& runtime, DeviceMemory& device_memory,
                                           size_t stage, u32 index, u32 binding_index,
                                           u64 device_addr, u32 size) {
    using Reuse = UniformStreamReuse<typename Runtime::StreamUniformRef>;
    if (size == 0 || size > Reuse::MaxBytes || stage >= Reuse::NumStages ||
        index >= Reuse::NumSlots) {
        return false;
    }
    Reuse& reuse = UniformStreamReuseState<typename Runtime::StreamUniformRef>();
    typename Reuse::Slot& slot = reuse.slots[stage][index];
    const bool stats = UniformStreamStatsEnabled();
    if (stats) {
        LogUniformStreamStats(reuse.stats);
    }
    // Compared and copied straight from guest memory when it is contiguous there: the copy into
    // scratch was a quarter of the bytes moved for every uniform buffer of every draw.
    // EDEN_NX2_UNIFORM_DIRECT=0 always copies into scratch first (=a alternates).
    static const char direct_mode = Tegra::Nx2EnvMode("EDEN_NX2_UNIFORM_DIRECT");
    const bool direct = Tegra::Nx2Switch(direct_mode, true);

    // Slots whose bytes change on (almost) every bind, e.g. title 1's per-draw constants, never
    // reuse: the compare and the copy into slot.bytes are wasted. After BypassAfterMisses misses
    // in a row the slot copies guest memory once, straight into the stream allocation, and keeps
    // no bytes (size 0) so nothing stale is reused later. Every ProbeEvery binds it goes through
    // the reusing path again to notice when reuse comes back.
    // Off by default: in title 1 it gave 2.8 fewer frames a second, 0.55 us more back time a
    // draw (medians, 35 A/B windows). EDEN_NX2_UBOADAPT=1 turns it on (=a alternates).
    static const char adapt_mode = Tegra::Nx2EnvMode("EDEN_NX2_UBOADAPT");
    if (Tegra::Nx2Switch(adapt_mode, false) && slot.miss_streak >= Reuse::BypassAfterMisses) {
        if (++slot.bypass_binds < Reuse::ProbeEvery) {
            slot.size = 0;
            const std::span<u8> span = runtime.BindMappedUniformBuffer(stage, binding_index, size);
            const u8* const guest = direct ? device_memory.GetSpan(device_addr, size) : nullptr;
            if (guest != nullptr) {
                std::memcpy(span.data(), guest, size);
            } else {
                device_memory.ReadBlockUnsafe(device_addr, span.data(), size);
            }
            if (stats) {
                ++reuse.stats.bypassed;
            }
            return true;
        }
        // Probe: this bind misses (the slot keeps no bytes) and stores them, the next one compares
        // against them: a hit there leaves bypass, a miss goes back to it.
        slot.bypass_binds = 0;
        slot.miss_streak = Reuse::BypassAfterMisses - 2;
        if (stats) {
            ++reuse.stats.probes;
        }
    }

    const u8* scratch = direct ? device_memory.GetSpan(device_addr, size) : nullptr;
    if (scratch == nullptr) {
        device_memory.ReadBlockUnsafe(device_addr, reuse.scratch.data(), size);
        scratch = reuse.scratch.data();
    }

    const bool same_bytes =
        slot.size == size && std::memcmp(slot.bytes.data(), scratch, size) == 0;
    if (same_bytes && slot.tick == runtime.CurrentTick()) {
        runtime.RebindStreamUniform(slot.ref, size);
        slot.miss_streak = 0;
        if (stats) {
            ++reuse.stats.hits;
        }
        return true;
    }
    if (slot.miss_streak < Reuse::BypassAfterMisses) {
        ++slot.miss_streak;
    }
    if (stats) {
        ++reuse.stats.misses;
    }
    // The slot's copy is what gets uploaded: guest memory may change between the reads.
    if (!same_bytes) {
        slot.size = size;
        std::memcpy(slot.bytes.data(), scratch, size);
    }
    const std::span<u8> span = runtime.BindMappedUniformBuffer(stage, binding_index, size);
    std::memcpy(span.data(), slot.bytes.data(), size);
    slot.ref = runtime.LastStreamUniformRef();
    // After the allocation: the tick the stream region is tagged with is the one current now.
    slot.tick = runtime.CurrentTick();
    return true;
}

} // namespace VideoCommon
