// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/c11_vram.h"
#include "video_core/nx2.h"

/// Cycle 13, workstream W1 "VRAM: real pressure signal, hot relocation, render-target diet"
/// (docs/cycle13-plan.md section 2 W1). Backend neutral (TextureCache<P>, the Vulkan image,
/// allocator and staging code include it): no Vulkan types. Every piece has its own switch, read
/// once; OOM and RELOC on by default under MT lite ('0' off), the others OFF by default ('1' on,
/// anything else off; 'a' is not useful: structural state):
///  V1 EDEN_NX2_C13_OOM           out-of-memory margin from the measured deficit, per episode
///  V2 EDEN_NX2_C13_RELOC         host-resident images moved back by real room (RTs rebound)
///  V3 EDEN_NX2_C13_RT_DIET       square depth / R32 targets (shadow-map shape) stay 1x
///  V4 EDEN_NX2_C13_STAGING_TRIM  staging pool: idle buffers freed, device-local pool capped
/// Diagnostics: EDEN_NX2_C13_OOM_CHECK, EDEN_NX2_C13_RELOC_CHECK, EDEN_NX2_C13_STAGING_TRIM_CHECK
/// (census only), EDEN_NX2_C13_STATS=1 (one "C13 vram (10 s)" line; on with any W1 CHECK; the
/// render-target census every 30 s and the VRAM census line with it).
namespace VideoCommon::C13Vram {

namespace Detail {
constexpr u64 MiB = u64{1} << 20;
[[nodiscard]] inline bool On(const char* name) {
    return Tegra::Nx2EnvMode(name) == '1';
}
/// '1' on, '0' off, unset: on under EDEN_NX2_MT_LITE (cycle 13 results: OOM and RELOC default-on).
[[nodiscard]] inline bool OnUnderLite(const char* name) {
    const char mode = Tegra::Nx2EnvMode(name);
    return mode == '1' || (mode != '0' && VideoCommon::MT::Lite());
}
[[nodiscard]] inline u64 ReadNumber(const char* name, u64 fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] < '0' || value[0] > '9') {
        return fallback;
    }
    return std::strtoull(value, nullptr, 10);
}
} // namespace Detail

/// V1 EDEN_NX2_C13_OOM (default on under EDEN_NX2_MT_LITE, '0' off): the texture cache's
/// "keeping N MiB more free" margin is no
/// longer +512 MiB per frame with new allocator failures (one burst over 3 frames was +1.5 GiB,
/// decaying 128 MiB per 600 frames: the whole race). The allocator records each device-full
/// failure with the heap usage and budget at that moment and the failed size; failures less than
/// EpisodeQuietFrames apart form one episode, whose margin is max(0, usage + failed bytes - budget
/// + NeedPad), the max over the episode (capped by EDEN_NX2_C11_PRESSURE_CAP_MB). Episodes during
/// a load (driver allocations > 1 GiB/s or a frame > 100 ms) hold it until LoadHoldFrames frames
/// after the load, then drop back to the margin held before the episode in one step. Otherwise it
/// decays DecayStep per DecayFrames frames while usage + margin < budget. Floor: critical never
/// below the measured non-evictable set (device-resident render targets + non-texture VMA bytes +
/// the driver's own usage outside VMA blocks) + FloorPad, capped at budget - 256 MiB.
[[nodiscard]] inline bool OomOn() {
    static const bool on = Detail::OnUnderLite("EDEN_NX2_C13_OOM");
    return on;
}
/// EDEN_NX2_C13_OOM_CHECK=1: the rule not applied runs as a shadow (with OOM off the new one, with
/// OOM on the cycle 12 one); every episode is logged with both margins ("C13 oom episode"), frames
/// over one critical but not the other are counted, and device-full failures in the 300 frames
/// after an episode closed or dropped its hold are counted ("refails", must be 0 in the race).
[[nodiscard]] inline bool OomCheck() {
    static const bool on = Detail::On("EDEN_NX2_C13_OOM_CHECK");
    return on;
}
/// The allocator records failures and the texture cache runs the episode state.
[[nodiscard]] inline bool OomRecording() {
    return OomOn() || OomCheck();
}

inline constexpr u64 EpisodeQuietFrames = 60;
inline constexpr u64 LoadHoldFrames = 300;
inline constexpr u64 DecayFrames = 120;
inline constexpr u64 DecayStep = 256 * Detail::MiB;
inline constexpr u64 NeedPad = 256 * Detail::MiB;
inline constexpr u64 FloorPad = 512 * Detail::MiB;
inline constexpr u64 RefailWatchFrames = 300;
inline constexpr double LoadAllocBytesPerSecond = 1024.0 * 1024.0 * 1024.0;
inline constexpr double LoadFrameMs = 100.0;
/// Frames between two non-evictable measurements (an image walk, as the C11 category walk).
inline constexpr u64 FloorWalkFrames = 120;

/// V2 EDEN_NX2_C13_RELOC (default on under EDEN_NX2_MT_LITE, '0' off): host-resident images
/// (cycle 10 guard) move back to VRAM
/// when the heap has real room (usage + the image's bytes + RelocPad < the driver budget), not
/// only under the texture cache's critical, frame-hot ones included, the largest first, one a
/// frame. GPU-written images (render targets, never re-loadable from guest memory) get a new
/// device-local backing filled by a GPU copy (Image::C13Rebind: same ImageId, views and
/// framebuffers dropped as on a rescale); the others take the cycle 10 delete / re-create path.
[[nodiscard]] inline bool RelocOn() {
    static const bool on = Detail::OnUnderLite("EDEN_NX2_C13_RELOC");
    return on;
}
/// EDEN_NX2_C13_RELOC_CHECK=1: each rebind also copies two 64x64 tiles (corner, centre) of level
/// 0 / layer 0 of the old and the new backing to a readback buffer and compares them once the GPU
/// is done (mismatches counted, first 32 logged as "C13 reloc check"). Single-sample images only.
[[nodiscard]] inline bool RelocCheck() {
    static const bool on = Detail::On("EDEN_NX2_C13_RELOC_CHECK");
    return on;
}
inline constexpr u64 RelocPad = 512 * Detail::MiB;
/// Frames without a relocation attempt after the new backing could not be allocated.
inline constexpr u64 RelocBackoffFrames = 120;
inline constexpr u32 RelocCheckTile = 64;

enum class RebindResult : u8 {
    Done,    ///< Every host-resident part has a device-local backing now
    Refused, ///< Not eligible (sparse, converted, ...): use the delete path
    NoRoom,  ///< The device-local allocation within the budget failed: try later
};

/// V3 EDEN_NX2_C13_RT_DIET (default off: changes image quality): square 2D single-sample images
/// of EDEN_NX2_C13_RT_DIET_MIN..EDEN_NX2_C13_RT_DIET_MAX texels a side (default 1024..8192) in a
/// depth or single-channel 32-bit format (the shadow-map shape: 3072^2 x 4 B, 144 MiB at 2x) are
/// never rescaled (ImageCanRescale refuses them; TextureCache::ScaleUp too). A square target the
/// size of a screen would be caught too: the census (EDEN_NX2_C13_STATS) lists every one.
[[nodiscard]] inline bool RtDietOn() {
    static const bool on = Detail::On("EDEN_NX2_C13_RT_DIET");
    return on;
}
[[nodiscard]] inline u32 RtDietMin() {
    static const u32 value =
        static_cast<u32>((std::min)(Detail::ReadNumber("EDEN_NX2_C13_RT_DIET_MIN", 1024), u64{65536}));
    return value;
}
[[nodiscard]] inline u32 RtDietMax() {
    static const u32 value =
        static_cast<u32>((std::min)(Detail::ReadNumber("EDEN_NX2_C13_RT_DIET_MAX", 8192), u64{65536}));
    return value;
}

/// The diet's shape test (also the census' "diet candidate"). `Info` is VideoCommon::ImageInfo.
template <class Info>
[[nodiscard]] inline bool DietShape(const Info& info) noexcept {
    // The format type from Info (no surface.h here: it pulls gpu.h -> nvdata.h, whose enum names
    // collide with X11 macros in the Vulkan translation units that include this header).
    using PixelFormat = std::remove_cvref_t<decltype(info.format)>;
    if (info.type != decltype(info.type)::e2D || info.num_samples != 1 ||
        info.size.width != info.size.height || info.size.width < RtDietMin() ||
        info.size.width > RtDietMax()) {
        return false;
    }
    switch (info.format) {
    case PixelFormat::D32_FLOAT:
    case PixelFormat::D16_UNORM:
    case PixelFormat::X8_D24_UNORM:
    case PixelFormat::D24_UNORM_S8_UINT:
    case PixelFormat::S8_UINT_D24_UNORM:
    case PixelFormat::D32_FLOAT_S8_UINT:
    case PixelFormat::R32_FLOAT:
    case PixelFormat::R32_UINT:
    case PixelFormat::R32_SINT:
        return true;
    default:
        return false;
    }
}

/// V4 EDEN_NX2_C13_STAGING_TRIM (default off): every TrimSweepFrames frames the staging pool
/// frees every free buffer unused for TrimIdleFrames frames (any pool, any size; the batch 8
/// sweep keeps them 600 frames and visits 16 per level per 64 frames), and outside loads caps
/// the device-local pool at EDEN_NX2_C13_STAGING_CAP_MB (default 256), oldest first.
[[nodiscard]] inline bool StagingTrimOn() {
    static const bool on = Detail::On("EDEN_NX2_C13_STAGING_TRIM");
    return on;
}
/// EDEN_NX2_C13_STAGING_TRIM_CHECK=1: census only (staging pools by usage / idle age, stream
/// buffer, VMA device-local bytes by memory type and pool) in a "C13 vram census" line.
[[nodiscard]] inline bool StagingCheck() {
    static const bool on = Detail::On("EDEN_NX2_C13_STAGING_TRIM_CHECK");
    return on;
}
[[nodiscard]] inline u64 StagingCap() {
    static const u64 cap = Detail::ReadNumber("EDEN_NX2_C13_STAGING_CAP_MB", 256) << 20;
    return cap;
}
inline constexpr u64 TrimSweepFrames = 60;
inline constexpr u64 TrimIdleFrames = 300;
inline constexpr u64 CensusFrames = 600;

/// EDEN_NX2_C13_STATS=1 (or any W1 CHECK): the "C13 vram (10 s)" line, the render-target
/// census (every 30 s) and the VRAM census line (with the VRAM stats line, EDEN_NX2_VRAM_STATS).
[[nodiscard]] inline bool StatsOn() {
    static const bool on = Detail::On("EDEN_NX2_C13_STATS") || OomCheck() || RelocCheck() ||
                           StagingCheck();
    return on;
}
[[nodiscard]] inline bool CensusOn() {
    return StatsOn();
}
inline constexpr u64 RtCensusFrames = 1800;

struct Counters {
    // V1 (since start; the stats line prints differences).
    std::atomic<u64> failures{};       ///< Device-full failures recorded by the allocator
    std::atomic<u64> failure_bytes{};
    std::atomic<u64> episodes{};       ///< Closed episodes
    std::atomic<u64> load_episodes{};  ///< ...of them during a load
    std::atomic<u64> load_drops{};     ///< Load holds released
    std::atomic<u64> decays{};
    std::atomic<u64> refails{};        ///< Failures within RefailWatchFrames after a close/drop
    std::atomic<u64> floor_raises{};   ///< Budget checks where the floor raised critical
    std::atomic<u64> frames{};
    std::atomic<u64> frames_over{};    ///< Frames over the applied critical
    std::atomic<u64> frames_old_only{}; ///< Over the cycle 12 rule's critical, not the new one
    std::atomic<u64> frames_new_only{}; ///< Over the new rule's critical, not the cycle 12 one
    // V2.
    std::atomic<u64> rebinds{};
    std::atomic<u64> rebind_bytes{};
    std::atomic<u64> deletes{};        ///< Host images evicted to be re-created (non-RT)
    std::atomic<u64> delete_bytes{};
    std::atomic<u64> refused_room{};   ///< Frames with a host image but no real room
    std::atomic<u64> no_alloc{};       ///< Rebinds whose device-local allocation failed
    std::atomic<u64> refused{};        ///< Rebinds refused (not eligible: delete path instead)
    std::atomic<u64> check_tiles{};
    std::atomic<u64> check_mismatches{};
    // V3.
    std::atomic<u64> diet_scaleups_refused{};
    // V4.
    std::atomic<u64> staging_trimmed{};
    std::atomic<u64> staging_trimmed_bytes{};
};
inline Counters g_counters;

/// Gauges, written and read on the thread that ticks the texture cache (the VRAM stats line runs
/// from its TickFrame too).
struct Gauges {
    u64 margin{};          ///< Applied out-of-memory margin
    u64 old_margin{};      ///< The cycle 12 rule's (applied with OOM off, shadow with it on)
    u64 new_margin{};      ///< The episode rule's (applied with OOM on, shadow with CHECK)
    u64 floor{};           ///< Last non-evictable floor (0 = not measured)
    u64 nonevictable{};
    u64 rt_bytes{};        ///< Device-resident GPU-written images (both scales)
    u64 image_bytes{};     ///< Device-resident texture cache images
    u64 vma_alloc{};       ///< VMA device-local allocation bytes
    u64 vma_blocks{};      ///< VMA device-local block bytes
    u64 budget{};
    u64 critical{};
    u64 old_critical{};    ///< Critical under the other rule (CHECK)
    u64 new_critical{};
    u64 diet_images{};     ///< Census: 1x images of the diet shape (GPU-written)
    u64 diet_bytes{};
    bool loading{};
};
inline Gauges g_gauges;

/// Load signal for the staging trim (set by the texture cache's episode tick).
inline std::atomic<bool> g_loading{false};

/// V4 census (staging pool thread writes, the stats line reads).
struct StagingCensus {
    std::atomic<u64> frame{};
    std::atomic<u64> device_count{}, device_bytes{}, device_idle_bytes{}, device_vram_bytes{};
    std::atomic<u64> upload_count{}, upload_bytes{}, upload_idle_bytes{}, upload_vram_bytes{};
    std::atomic<u64> download_count{}, download_bytes{}, download_idle_bytes{},
        download_vram_bytes{};
    std::atomic<u64> deferred{};
    std::atomic<u64> stream_bytes{};
    std::atomic<bool> stream_vram{};
};
inline StagingCensus g_staging;

/// One device-full failure batch as the allocator recorded it.
struct FailureBatch {
    u32 failures{};
    u64 bytes{};
    s64 worst_need{(std::numeric_limits<s64>::min)()}; ///< usage + bytes - budget, max
    u64 usage{};        ///< At the worst failure
    u64 budget{};
    u64 failed_bytes{};
};

namespace Detail {
inline std::mutex g_fail_mutex;
inline FailureBatch g_fail;
} // namespace Detail
inline std::atomic<u64> g_fail_seq{0};

/// Allocator side (any thread): a device-local allocation failed even past the budget.
inline void NoteDeviceFull(u64 usage, u64 budget, u64 bytes) {
    if (!OomRecording()) {
        return;
    }
    const s64 need = static_cast<s64>(usage) + static_cast<s64>(bytes) - static_cast<s64>(budget);
    {
        std::scoped_lock lock{Detail::g_fail_mutex};
        FailureBatch& f = Detail::g_fail;
        ++f.failures;
        f.bytes += bytes;
        if (need > f.worst_need) {
            f.worst_need = need;
            f.usage = usage;
            f.budget = budget;
            f.failed_bytes = bytes;
        }
    }
    g_counters.failures.fetch_add(1, std::memory_order_relaxed);
    g_counters.failure_bytes.fetch_add(bytes, std::memory_order_relaxed);
    g_fail_seq.fetch_add(1, std::memory_order_release);
}

/// Texture cache side: takes what the allocator recorded since the last call.
[[nodiscard]] inline bool DrainFailures(FailureBatch& out, u64& seen_seq) {
    const u64 seq = g_fail_seq.load(std::memory_order_acquire);
    if (seq == seen_seq) {
        return false;
    }
    seen_seq = seq;
    std::scoped_lock lock{Detail::g_fail_mutex};
    out = Detail::g_fail;
    Detail::g_fail = FailureBatch{};
    return out.failures != 0;
}

/// V1: episode state, owned by the texture cache (one thread).
class OomState {
public:
    /// Once a frame. `usage`: the cache's device usage, `budget`: the last budget read (0 when
    /// unknown), `alloc_bytes`: driver allocation bytes since start (0 when not counted),
    /// `old_margin`: the cycle 12 rule's margin now (for the CHECK log). True when the margin
    /// changed (thresholds must be recomputed).
    bool Tick(u64 frame, u64 usage, u64 budget, u64 alloc_bytes, u64 old_margin) {
        using Clock = std::chrono::steady_clock;
        const Clock::time_point now = Clock::now();
        const double frame_ms =
            has_last ? std::chrono::duration<double, std::milli>(now - last_tick).count() : 0.0;
        last_tick = now;
        has_last = true;
        // Load signal: driver allocation rate over 30 frames, or a long frame.
        if (window_frames == 0) {
            window_start = now;
            window_bytes = alloc_bytes;
        }
        if (++window_frames >= 30) {
            const double seconds = std::chrono::duration<double>(now - window_start).count();
            const u64 delta = alloc_bytes - (std::min)(alloc_bytes, window_bytes);
            rate_loading =
                seconds > 0.0 && static_cast<double>(delta) / seconds > LoadAllocBytesPerSecond;
            window_frames = 0;
        }
        loading = rate_loading || frame_ms > LoadFrameMs;
        if (loading) {
            last_load_frame = frame;
        }
        g_loading.store(loading, std::memory_order_relaxed);
        bool changed = false;
        FailureBatch batch;
        if (DrainFailures(batch, seen_seq)) {
            if (frame < watch_until) {
                g_counters.refails.fetch_add(batch.failures, std::memory_order_relaxed);
            }
            if (!open) {
                open = true;
                episode_load = false;
                episode_start = frame;
                episode = FailureBatch{};
                base_margin = load_hold ? hold_base : margin;
                load_hold = false;
                episode_old_start = old_margin;
            }
            episode.failures += batch.failures;
            episode.bytes += batch.bytes;
            if (batch.worst_need > episode.worst_need) {
                episode.worst_need = batch.worst_need;
                episode.usage = batch.usage;
                episode.budget = batch.budget;
                episode.failed_bytes = batch.failed_bytes;
            }
            last_failure_frame = frame;
            const u64 target = (std::max)(base_margin, EpisodeMargin());
            if (target > margin) {
                margin = target;
                changed = true;
            }
        }
        if (open) {
            episode_load = episode_load || loading;
            if (frame - last_failure_frame >= EpisodeQuietFrames) {
                Close(frame, old_margin);
            }
        } else if (load_hold) {
            if (frame >= (std::max)(hold_close_frame, last_load_frame) + LoadHoldFrames) {
                const u64 before = margin;
                margin = (std::min)(margin, hold_base);
                changed = changed || margin != before;
                load_hold = false;
                watch_until = frame + RefailWatchFrames;
                g_counters.load_drops.fetch_add(1, std::memory_order_relaxed);
                if (OomCheck()) {
                    LOG_INFO(HW_GPU,
                             "C13 oom load hold released at frame {}: margin {} -> {} MiB "
                             "(cycle 12 rule now {} MiB)",
                             frame, before >> 20, margin >> 20, old_margin >> 20);
                }
            }
        } else if (margin != 0 && ++decay_frames % DecayFrames == 0) {
            if (budget != 0 && usage + margin < budget) {
                margin -= (std::min)(margin, DecayStep);
                changed = true;
                g_counters.decays.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return changed;
    }

    [[nodiscard]] u64 Margin() const noexcept {
        return margin;
    }
    [[nodiscard]] bool Loading() const noexcept {
        return loading;
    }

private:
    [[nodiscard]] u64 EpisodeMargin() const noexcept {
        if (episode.worst_need == (std::numeric_limits<s64>::min)()) {
            return 0;
        }
        const s64 padded = episode.worst_need + static_cast<s64>(NeedPad);
        const u64 value = padded > 0 ? static_cast<u64>(padded) : 0;
        return (std::min)(value, C11Vram::PressureCap());
    }

    void Close(u64 frame, u64 old_margin) {
        open = false;
        watch_until = frame + RefailWatchFrames;
        g_counters.episodes.fetch_add(1, std::memory_order_relaxed);
        if (episode_load) {
            load_hold = true;
            hold_close_frame = frame;
            hold_base = base_margin;
            g_counters.load_episodes.fetch_add(1, std::memory_order_relaxed);
        }
        if (OomCheck()) {
            static std::atomic<u32> logged{};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 256) {
                LOG_INFO(HW_GPU,
                         "C13 oom episode: failures {}, {} MiB, usage/budget at the worst "
                         "failure {}/{} MiB ({} KiB failed, deficit {} MiB), frames {}-{}, load "
                         "{}; old margin {} -> {} MiB, new margin {} -> {} MiB",
                         episode.failures, episode.bytes >> 20, episode.usage >> 20,
                         episode.budget >> 20, episode.failed_bytes >> 10,
                         episode.worst_need / static_cast<s64>(Detail::MiB), episode_start,
                         last_failure_frame, episode_load ? "yes" : "no",
                         episode_old_start >> 20, old_margin >> 20, base_margin >> 20,
                         margin >> 20);
            }
        }
    }

    std::chrono::steady_clock::time_point last_tick{};
    std::chrono::steady_clock::time_point window_start{};
    bool has_last{};
    u64 window_bytes{};
    u64 window_frames{};
    bool rate_loading{};
    bool loading{};
    u64 last_load_frame{};
    u64 seen_seq{};
    bool open{};
    bool episode_load{};
    u64 episode_start{};
    u64 last_failure_frame{};
    FailureBatch episode{};
    u64 base_margin{};
    u64 episode_old_start{};
    bool load_hold{};
    u64 hold_close_frame{};
    u64 hold_base{};
    u64 watch_until{};
    u64 decay_frames{};
    u64 margin{};
};

/// Per-frame accounting (texture cache thread): applied critical and, with CHECK, the other
/// rule's critical (0 = not known yet).
inline void NoteFrame(u64 used, u64 critical, u64 old_critical, u64 new_critical) {
    auto& c = g_counters;
    c.frames.fetch_add(1, std::memory_order_relaxed);
    if (used >= critical) {
        c.frames_over.fetch_add(1, std::memory_order_relaxed);
    }
    if (old_critical != 0 && new_critical != 0) {
        const bool old_over = used >= old_critical;
        const bool new_over = used >= new_critical;
        if (old_over && !new_over) {
            c.frames_old_only.fetch_add(1, std::memory_order_relaxed);
        } else if (new_over && !old_over) {
            c.frames_new_only.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

/// Start line (once, from the runtime's first VRAM tick).
inline void LogStart() {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true)) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "Cycle 13 VRAM: oom {} (check {}), reloc {} (check {}), rt diet {} ({}..{} texels), "
             "staging trim {} (cap {} MiB, census {}), stats {}",
             OomOn(), OomCheck(), RelocOn(), RelocCheck(), RtDietOn(), RtDietMin(), RtDietMax(),
             StagingTrimOn(), StagingCap() >> 20, StagingCheck(), StatsOn());
}

/// The "C13 vram (10 s)" line; call once a frame from the texture cache's TickFrame.
inline void TickStats() {
    if (!StatsOn()) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    struct Snapshot {
        u64 failures, failure_bytes, episodes, load_episodes, load_drops, decays, refails,
            floor_raises, frames, frames_over, frames_old_only, frames_new_only, rebinds,
            rebind_bytes, deletes, delete_bytes, refused_room, no_alloc, refused, check_tiles,
            check_mismatches, diet_refused, trimmed, trimmed_bytes;
    };
    static Snapshot previous{};
    const Clock::time_point now = Clock::now();
    if (now - last < std::chrono::seconds{10}) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - last).count();
    last = now;
    const auto load = [](const std::atomic<u64>& value) {
        return value.load(std::memory_order_relaxed);
    };
    auto& c = g_counters;
    const Snapshot s{
        load(c.failures),       load(c.failure_bytes),  load(c.episodes),
        load(c.load_episodes),  load(c.load_drops),     load(c.decays),
        load(c.refails),        load(c.floor_raises),   load(c.frames),
        load(c.frames_over),    load(c.frames_old_only), load(c.frames_new_only),
        load(c.rebinds),        load(c.rebind_bytes),   load(c.deletes),
        load(c.delete_bytes),   load(c.refused_room),   load(c.no_alloc),
        load(c.refused),        load(c.check_tiles),    load(c.check_mismatches),
        load(c.diet_scaleups_refused), load(c.staging_trimmed), load(c.staging_trimmed_bytes),
    };
    const Snapshot& p = previous;
    const Gauges& g = g_gauges;
    LOG_INFO(HW_GPU,
             "C13 vram (10 s, {:.1f} s): oom margin {} MiB (old rule {} / new rule {}), floor {} "
             "MiB (non-evictable {}: RTs {}, images {}, VMA {} in blocks {}), budget {} critical "
             "{} (old {} / new {}); failures {} ({} MiB), episodes {} (load {}), load drops {}, "
             "decays {}, refails {}, floor raises {}; frames {} over critical {} (old only {}, "
             "new only {}); reloc rebinds {} ({} MiB), deletes {} ({} MiB), no room {}, no "
             "alloc {}, refused {}, check tiles {} mismatches {}; diet 1x images {} ({} MiB), "
             "scale-ups refused {}; staging trimmed {} ({} MiB); loading {}",
             seconds, g.margin >> 20, g.old_margin >> 20, g.new_margin >> 20, g.floor >> 20,
             g.nonevictable >> 20, g.rt_bytes >> 20, g.image_bytes >> 20, g.vma_alloc >> 20,
             g.vma_blocks >> 20, g.budget >> 20, g.critical >> 20, g.old_critical >> 20,
             g.new_critical >> 20, s.failures - p.failures,
             (s.failure_bytes - p.failure_bytes) >> 20, s.episodes - p.episodes,
             s.load_episodes - p.load_episodes, s.load_drops - p.load_drops,
             s.decays - p.decays, s.refails - p.refails, s.floor_raises - p.floor_raises,
             s.frames - p.frames, s.frames_over - p.frames_over,
             s.frames_old_only - p.frames_old_only, s.frames_new_only - p.frames_new_only,
             s.rebinds - p.rebinds, (s.rebind_bytes - p.rebind_bytes) >> 20,
             s.deletes - p.deletes, (s.delete_bytes - p.delete_bytes) >> 20,
             s.refused_room - p.refused_room, s.no_alloc - p.no_alloc, s.refused - p.refused,
             s.check_tiles - p.check_tiles, s.check_mismatches - p.check_mismatches,
             g.diet_images, g.diet_bytes >> 20, s.diet_refused - p.diet_refused,
             s.trimmed - p.trimmed, (s.trimmed_bytes - p.trimmed_bytes) >> 20,
             g.loading ? "yes" : "no");
    previous = s;
}

} // namespace VideoCommon::C13Vram
