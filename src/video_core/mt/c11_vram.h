// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <list>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/mt_switches.h"
#include "video_core/nx2.h"

/// Cycle 11, workstream W3 "VRAM diet" (docs/cycle11-plan.md): stop the evict / re-create loop
/// at the driver budget instead of guarding it. Backend neutral (TextureCache<P> includes it, the
/// Vulkan image and allocator code too): no Vulkan types. Every piece has its own switch ('0' off,
/// '1' on, else the default); all are read once ('a' is not useful here: structural state).
///  V1 EDEN_NX2_C11_DROP1X        1x originals of rescaled render targets are freed
///  V2 EDEN_NX2_C11_DECODE_TRIM   persistent ASTC decode classes freed after 600 idle frames
///  V3 (EDEN_NX2_VRAM_STATS=1)    per-category device memory accounting line every 10 s
///  V4 EDEN_NX2_C11_VRAM_COORD    relocation / emergency collection keep off frame-hot images
///  V5 EDEN_NX2_C11_RAMTIER       system-RAM LRU of transcoded ASTC (BC) bytes, off by default
namespace VideoCommon::C11Vram {

namespace Detail {
[[nodiscard]] inline u64 ReadNumber(const char* name, u64 fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] < '0' || value[0] > '9') {
        return fallback;
    }
    return std::strtoull(value, nullptr, 10);
}
} // namespace Detail

/// V1 EDEN_NX2_C11_DROP1X (default on under EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1, '1' on without,
/// '0' today's code): Image::ScaleUp keeps the 1x image of every rescaled image (+25 % of it at
/// 2x). A rescaled render target that stayed rescaled (no scale-down, upload or download) for
/// EDEN_NX2_C11_DROP1X_AGE frames (default 300) frees its 1x image (once the GPU is done with
/// it); a path that needs it again (ScaleDown: download, upload, blacklisting, copies into a
/// non-rescaled image) re-creates it on demand and blits the scaled contents into it. Refused:
/// MSAA, formats that need the blit helper, sparse / unbacked / trimmed / converted images, images
/// with a non-rescaled or CPU-modified alias, CPU-modified or bad-overlap images.
[[nodiscard]] inline bool Drop1xOn() {
    static const bool on =
        Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_C11_DROP1X"), MT::Lite());
    return on;
}
[[nodiscard]] inline u64 Drop1xAge() {
    static const u64 age = (std::max)(Detail::ReadNumber("EDEN_NX2_C11_DROP1X_AGE", 300), u64{2});
    return age;
}
/// Frames between two DROP1X sweeps over the cache's images.
inline constexpr u64 Drop1xSweepFrames = 30;

/// EDEN_NX2_C11_DROP1X_CHECK=1 (diagnostic, off): every on-demand re-creation of a dropped 1x
/// image is logged (first 128: address, size, format, reason) besides being counted.
[[nodiscard]] inline bool Drop1xCheck() {
    static const bool on = Tegra::Nx2EnvMode("EDEN_NX2_C11_DROP1X_CHECK") == '1';
    return on;
}

/// EDEN_NX2_ASTC_DISKCACHE as the T2 disk cache reads it (default off).
[[nodiscard]] inline bool DiskCacheOn() {
    static const bool on = Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_ASTC_DISKCACHE"), false);
    return on;
}

/// V2 EDEN_NX2_C11_DECODE_TRIM (default on with EDEN_NX2_ASTC_DISKCACHE, else off): the
/// persistent ASTC decode images (one per power-of-two class 64..4096, ~350 MiB with all of them)
/// are only used on disk cache misses (0-2 per 10 s in the race); every class unused for
/// EDEN_NX2_C11_DECODE_TRIM_FRAMES frames (default 600, any size) goes back to the driver once
/// the GPU is done with it. The next miss of that class allocates it again (lazily, as today).
[[nodiscard]] inline bool DecodeTrimOn() {
    static const bool on =
        Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_C11_DECODE_TRIM"), DiskCacheOn());
    return on;
}
[[nodiscard]] inline u64 DecodeTrimFrames() {
    static const u64 frames =
        (std::max)(Detail::ReadNumber("EDEN_NX2_C11_DECODE_TRIM_FRAMES", 600), u64{1});
    return frames;
}
/// Frames between two trim sweeps.
inline constexpr u64 DecodeTrimSweepFrames = 60;

/// V4 EDEN_NX2_C11_VRAM_COORD (default on under MT lite, '1' on without): coordinates cycle 10 A
/// with the collector. The guard's relocation of host-resident images runs only while usage is
/// under critical - EDEN_NX2_C11_VRAM_COORD_MARGIN_MB (default 256) and only with that much room
/// left after the picked images (their re-creation must not push the collector into evicting
/// frame-hot images), and an emergency collection (costly images unprotected) never evicts an
/// image used in the last EDEN_NX2_C11_VRAM_COORD_HOT frames (default 60).
[[nodiscard]] inline bool CoordOn() {
    static const bool on =
        Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_C11_VRAM_COORD"), MT::Lite());
    return on;
}
[[nodiscard]] inline u64 CoordMargin() {
    static const u64 margin = Detail::ReadNumber("EDEN_NX2_C11_VRAM_COORD_MARGIN_MB", 256) << 20;
    return margin;
}
[[nodiscard]] inline u64 CoordHotFrames() {
    static const u64 frames = Detail::ReadNumber("EDEN_NX2_C11_VRAM_COORD_HOT", 60);
    return frames;
}

/// V5 EDEN_NX2_C11_RAMTIER (default off; only if V1 + V2 do not get under critical): the BC bytes
/// of transcoded ASTC images read from (or written to) the disk cache stay in a system-RAM LRU of
/// EDEN_NX2_C11_RAMTIER_MB (default 384, 64..2048) keyed by the disk cache key, so re-creating
/// an evicted image skips the file read (the hash stays: guest pages of evicted images are not
/// write-tracked any more). Needs EDEN_NX2_ASTC_DISKCACHE=1.
[[nodiscard]] inline bool RamTierOn() {
    static const bool on =
        DiskCacheOn() && Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_C11_RAMTIER"), false);
    return on;
}
[[nodiscard]] inline u64 RamTierCap() {
    static const u64 cap =
        std::clamp<u64>(Detail::ReadNumber("EDEN_NX2_C11_RAMTIER_MB", 384), 64, 2048) << 20;
    return cap;
}

/// EDEN_NX2_C11_PRESSURE_CAP_MB (default 3072 = the cycle 10 value, 0..3072): the most the
/// texture cache's "Out of device memory: keeping N MiB more free" margin may grow to. In the
/// cycle 11 race it reached 3072 MiB in 3 of 4 runs (c10 too): critical ~3.2 GiB while the
/// cache holds ~5.8 GiB of frame-hot images, so every frame runs an emergency collection that
/// cannot get under it (docs/cycle11-analysis.md). A knob for the next round (e.g. 1024).
[[nodiscard]] inline u64 PressureCap() {
    static const u64 cap =
        (std::min)(Detail::ReadNumber("EDEN_NX2_C11_PRESSURE_CAP_MB", 3072), u64{3072}) << 20;
    return cap;
}

/// V3: EDEN_NX2_VRAM_STATS=1 (the batch 8 switch) also prints the per-category line.
[[nodiscard]] inline bool StatsOn() {
    static const bool on = Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_VRAM_STATS"), false);
    return on;
}
/// Frames between two category walks over the cache's images.
inline constexpr u64 CategoryWalkFrames = 120;

struct Counters {
    // V1 (since start; the stats line prints differences).
    std::atomic<u64> dropped{};          ///< 1x images freed
    std::atomic<u64> dropped_bytes{};
    std::atomic<u64> recreated{};        ///< Re-created on demand
    std::atomic<u64> recreated_bytes{};
    std::atomic<u64> recreated_upload{}; ///< ...for an upload (ScaleDown(ignore))
    std::atomic<u64> recreated_down{};   ///< ...for a scale-down (download, blacklist, copies)
    std::atomic<u64> refused{};          ///< Sweep candidates refused (alias, flags, format...)
    std::atomic<u64> refused_views{};    ///< ...because a storage view of the 1x image exists
    std::atomic<u64> refused_helper{};   ///< ...because the blit helper (views/fbs/format) is used
    // V2.
    std::atomic<u64> trimmed{};
    std::atomic<u64> trimmed_bytes{};
    // V4.
    std::atomic<u64> reloc_refused_margin{}; ///< Relocation passes skipped for the margin
    std::atomic<u64> emergency_hot_passes{}; ///< Emergency passes that kept hot images
    // V5.
    std::atomic<u64> ram_hits{};
    std::atomic<u64> ram_hit_bytes{};
    std::atomic<u64> ram_misses{};
    std::atomic<u64> ram_inserts{};
    std::atomic<u64> ram_evictions{};
};
inline Counters g_counters;

/// V3: the latest category walk (texture cache images) and the runtime's own images. Written on
/// the thread that ticks the texture cache, read by the stats line on the same thread.
struct Categories {
    u64 walk_frame{};
    u64 images{};
    u64 rt_1x_bytes{};         ///< GPU-written images not rescaled
    u64 rt_1x_count{};
    u64 rt_2x_bytes{};         ///< Scaled copies of rescaled images
    u64 rt_2x_count{};
    u64 originals_bytes{};     ///< 1x originals kept next to a scaled copy
    u64 originals_count{};
    u64 dropped_count{};       ///< Rescaled images whose 1x original is dropped now (V1)
    u64 astc_bc7_bytes{};      ///< Sampled ASTC transcoded to BC7
    u64 astc_bc7_count{};
    u64 astc_bc1_bytes{};      ///< ... to BC1 / BC3 / other host formats
    u64 astc_bc1_count{};
    u64 sampled_bytes{};       ///< Other images (sampled, not GPU-written)
    u64 sampled_count{};
    u64 sparse_bytes{};        ///< Bound memory of sparse / unbacked images
    u64 host_bytes{};          ///< Of all of the above, in host memory (cycle 10 A)
    // Runtime (TextureCacheRuntime::TickVram).
    u64 decode_class_bytes{};  ///< Persistent decode images
    u64 decode_class_count{};
    u64 transcode_scratch_bytes{}; ///< Pending / free transcode images
    u64 msaa_scratch_count{};
};
inline Categories g_categories;

/// Start line (once, from the runtime's first VRAM tick).
inline void LogStart() {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true)) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "Cycle 11 VRAM: drop1x {} (age {} frames, check {}), decode_trim {} ({} frames), "
             "coord {} (margin {} MiB, hot {} frames), ramtier {} ({} MiB), category stats {}, "
             "pressure cap {} MiB",
             Drop1xOn(), Drop1xAge(), Drop1xCheck(), DecodeTrimOn(), DecodeTrimFrames(),
             CoordOn(), CoordMargin() >> 20, CoordHotFrames(), RamTierOn(), RamTierCap() >> 20,
             StatsOn(), PressureCap() >> 20);
}

/// V3 + counters: two lines, called by the "VRAM in 10 s" stats (TickVramStats) with the device
/// local heaps' VMA allocation bytes (what is not in a category above is buffers, staging, the
/// swapchain-adjacent allocations and other runtime images).
inline void LogStats(u64 device_alloc_bytes) {
    if (!StatsOn()) {
        return;
    }
    struct Snapshot {
        u64 dropped, dropped_bytes, recreated, recreated_bytes, recreated_upload, recreated_down,
            refused, refused_views, refused_helper, trimmed, trimmed_bytes, reloc_refused,
            emergency_hot, ram_hits, ram_hit_bytes, ram_misses, ram_inserts, ram_evictions;
    };
    static Snapshot previous{};
    auto& c = g_counters;
    const auto load = [](const std::atomic<u64>& value) {
        return value.load(std::memory_order_relaxed);
    };
    const Snapshot now{
        load(c.dropped),       load(c.dropped_bytes),        load(c.recreated),
        load(c.recreated_bytes), load(c.recreated_upload),   load(c.recreated_down),
        load(c.refused),       load(c.refused_views),        load(c.refused_helper),
        load(c.trimmed),       load(c.trimmed_bytes),
        load(c.reloc_refused_margin), load(c.emergency_hot_passes), load(c.ram_hits),
        load(c.ram_hit_bytes), load(c.ram_misses),           load(c.ram_inserts),
        load(c.ram_evictions),
    };
    const Categories& k = g_categories;
    const u64 categorized = k.rt_1x_bytes + k.rt_2x_bytes + k.originals_bytes +
                            k.astc_bc7_bytes + k.astc_bc1_bytes + k.sampled_bytes +
                            k.sparse_bytes + k.decode_class_bytes + k.transcode_scratch_bytes;
    const u64 other = device_alloc_bytes - (std::min)(device_alloc_bytes, categorized);
    LOG_INFO(Render_Vulkan,
             "C11 VRAM categories (MiB, walk at frame {}, {} images): render targets 1x {} ({}) / "
             "2x {} ({}), rescale originals {} ({}, {} dropped), ASTC BC7 {} ({}) / BC1-BC3 {} "
             "({}), other sampled {} ({}), sparse bound {}, of them in host memory {}; decode "
             "classes {} ({}), transcode scratch {}, msaa scratch images {}; VMA device-local "
             "allocations {} -> buffers/staging/other {}",
             k.walk_frame, k.images, k.rt_1x_bytes >> 20, k.rt_1x_count, k.rt_2x_bytes >> 20,
             k.rt_2x_count, k.originals_bytes >> 20, k.originals_count, k.dropped_count,
             k.astc_bc7_bytes >> 20, k.astc_bc7_count, k.astc_bc1_bytes >> 20, k.astc_bc1_count,
             k.sampled_bytes >> 20, k.sampled_count, k.sparse_bytes >> 20, k.host_bytes >> 20,
             k.decode_class_bytes >> 20, k.decode_class_count, k.transcode_scratch_bytes >> 20,
             k.msaa_scratch_count, device_alloc_bytes >> 20, other >> 20);
    LOG_INFO(Render_Vulkan,
             "C11 VRAM in 10 s: drop1x dropped {} ({} MiB), re-created {} ({} MiB; upload {}, "
             "scale-down {}), refused {} (+{} storage views, +{} blit helper); decode trim {} "
             "classes ({} MiB); relocation skipped for the margin {}, emergency passes keeping hot "
             "images {}; ramtier hits {} ({} MiB) misses {} inserts {} evictions {}",
             now.dropped - previous.dropped, (now.dropped_bytes - previous.dropped_bytes) >> 20,
             now.recreated - previous.recreated,
             (now.recreated_bytes - previous.recreated_bytes) >> 20,
             now.recreated_upload - previous.recreated_upload,
             now.recreated_down - previous.recreated_down, now.refused - previous.refused,
             now.refused_views - previous.refused_views,
             now.refused_helper - previous.refused_helper,
             now.trimmed - previous.trimmed,
             (now.trimmed_bytes - previous.trimmed_bytes) >> 20,
             now.reloc_refused - previous.reloc_refused,
             now.emergency_hot - previous.emergency_hot, now.ram_hits - previous.ram_hits,
             (now.ram_hit_bytes - previous.ram_hit_bytes) >> 20,
             now.ram_misses - previous.ram_misses, now.ram_inserts - previous.ram_inserts,
             now.ram_evictions - previous.ram_evictions);
    previous = now;
}

/// V5: bounded LRU of byte blobs keyed by a 128-bit key (the ASTC disk cache key). One owner
/// thread (the texture cache runtime's), no locking.
class RamTier {
public:
    struct Key {
        u64 lo{};
        u64 hi{};
        bool operator==(const Key&) const = default;
    };

    /// Copies the entry into `out` (resized) and makes it the most recent; false on a miss or a
    /// size mismatch.
    [[nodiscard]] bool Find(const Key& key, u64 expected_size, std::vector<u8>& out) {
        const auto it = index.find(key);
        if (it == index.end() || it->second->second.size() != expected_size) {
            g_counters.ram_misses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        entries.splice(entries.begin(), entries, it->second);
        const std::vector<u8>& bytes = it->second->second;
        out.resize(bytes.size());
        std::memcpy(out.data(), bytes.data(), bytes.size());
        g_counters.ram_hits.fetch_add(1, std::memory_order_relaxed);
        g_counters.ram_hit_bytes.fetch_add(bytes.size(), std::memory_order_relaxed);
        return true;
    }

    void Insert(const Key& key, std::span<const u8> bytes) {
        const u64 cap = RamTierCap();
        if (bytes.empty() || bytes.size() > cap / 4) {
            return;
        }
        if (const auto it = index.find(key); it != index.end()) {
            entries.splice(entries.begin(), entries, it->second);
            return;
        }
        while (!entries.empty() && held + bytes.size() > cap) {
            auto& last = entries.back();
            held -= last.second.size();
            index.erase(last.first);
            entries.pop_back();
            g_counters.ram_evictions.fetch_add(1, std::memory_order_relaxed);
        }
        entries.emplace_front(key, std::vector<u8>(bytes.begin(), bytes.end()));
        index.emplace(key, entries.begin());
        held += bytes.size();
        g_counters.ram_inserts.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] u64 HeldBytes() const noexcept {
        return held;
    }

private:
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            return static_cast<size_t>(key.lo ^ (key.hi * 0x9E3779B97F4A7C15ULL));
        }
    };
    using Entry = std::pair<Key, std::vector<u8>>;
    std::list<Entry> entries;
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index;
    u64 held{};
};

} // namespace VideoCommon::C11Vram
