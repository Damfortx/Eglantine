// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/mt_switches.h"
#include "video_core/nx2.h"

/// Cycle 13, workstream W4 (docs/cycle13-plan.md §2 W4): memos that survive view alternation.
///
/// In 2P split screen both views draw through the same pipelines, A B A B: every cycle 12 memo
/// keeps one entry per pipeline (or per uniform slot), so each view evicts the other's entry and
/// the miss-streak throttle sees a pipeline that "never repeats". EDEN_NX2_C13_MEMO_WAYS keeps 2
/// (or 4) entries per slot, least recently used replacement:
///  - M1, the sequencer's capture repeat (vk_mt_seq.h C12Begin / C13BeginWays): a slot holds
///    `ways` C12Memo entries; a capture looks at the entries of its pipeline, the one whose view
///    tag (the first uniform buffer binding: the per-view camera buffer) equals this draw's
///    first, then most recently used, and validates at most two (same register key, same pages
///    and bytes: the cycle 12 validation, unchanged). The miss streak is counted per way: a
///    pipeline is throttled only when every way of it is in a miss streak. A miss records into
///    the same-view way when the tag tells the views apart, else the least recently used way.
///    The texture half on the committer follows the packet the way names (c12_repeat = the seq
///    of the same way's last packet): the pipeline's texture memo by packet keeps `ways` entries
///    too (vk_graphics_pipeline.cpp C13SelectTextureWay).
///  - M2, the committer's buffer half (buffer_cache.h C12UpdateGraphicsBuffers): the pipeline's
///    C12GraphicsBufferMemo becomes way 0 of a C13WaySet; the draw takes the first way (most
///    recently used first) whose state, masks and bindings match, a miss resolves in full and
///    saves into the least recently used way.
///  - M3, the uniform bind memo (BindGraphicsUniformBuffer / UpdateUniformBuffers): extra ways
///    per (stage, slot) through C13PromoteWay / C13DemoteFirst (hook lines:
///    docs/cycle13-hooks-W4.md, applied by the coordinator).
/// A wrong way is a miss, never a wrong draw: every way is validated exactly as the single
/// entry was.
///
/// W4c, the second-committer gate (data only): EDEN_NX2_C13_COMMIT_PROFILE samples every 32nd
/// draw packet on the committer and splits its time into phases (C13Phase): cache resolution
/// (uploads, textures, buffers, render targets) vs encoding (descriptor acquire/push, configure
/// draw, draw record). One "C13 commit profile (10 s)" line.
namespace VideoCommon::MT {

inline constexpr u32 kC13MaxWays = 4;

namespace C13Detail {
struct WaysConfig {
    u32 ways = 1;
    bool alternating = false;
    char mode = '1';
};

/// EDEN_NX2_C13_MEMO_WAYS: '1' or '0' one way (cycle 12), '2' (default under MT lite), '4',
/// 'a' alternating 2 ways / 1 way (Nx2AltOn). Read once.
[[nodiscard]] inline WaysConfig ReadWays() {
    WaysConfig config{};
    if (!Enabled()) {
        config.mode = '0';
        return config;
    }
    const char mode = Tegra::Nx2EnvMode("EDEN_NX2_C13_MEMO_WAYS");
    switch (mode) {
    case '0':
    case '1':
        config.ways = 1;
        config.mode = mode;
        return config;
    case '2':
        config.ways = 2;
        config.mode = '2';
        return config;
    case '4':
        config.ways = 4;
        config.mode = '4';
        return config;
    case 'a':
        config.ways = 2;
        config.alternating = true;
        config.mode = 'a';
        return config;
    default:
        config.ways = Lite() ? 2 : 1;
        config.mode = Lite() ? '2' : '1';
        return config;
    }
}

[[nodiscard]] inline const WaysConfig& Ways() {
    static const WaysConfig config = ReadWays();
    return config;
}
} // namespace C13Detail

/// Ways allocated per memo slot (1, 2 or 4). 1: every piece runs its cycle 12 path.
[[nodiscard]] inline u32 C13MemoWaysMax() {
    return C13Detail::Ways().ways;
}

/// Ways this draw may use (per draw: 'a' gives 1 in its off half; the storage keeps every way
/// in both halves, the 1-way half only uses way 0).
[[nodiscard]] inline u32 C13MemoWaysNow() {
    const C13Detail::WaysConfig& config = C13Detail::Ways();
    if (config.ways <= 1) {
        return 1;
    }
    if (config.alternating && !Tegra::Nx2AltOn()) {
        return 1;
    }
    return config.ways;
}

/// EDEN_NX2_C13_MEMO_TAG (with WAYS > 1): the view tag M1 prefers among a pipeline's ways.
/// '1' (default) the first uniform buffer binding of the first stage with uniform buffers (the
/// plan's per-view camera buffer), 'h' a hash of all that stage's uniform bindings, '0' no tag
/// (plain most-recently-used order, least-recently-used replacement).
[[nodiscard]] inline char C13MemoTagMode() {
    static const char mode = [] {
        const char value = Tegra::Nx2EnvMode("EDEN_NX2_C13_MEMO_TAG");
        return value == '0' || value == 'h' ? value : '1';
    }();
    return mode;
}

/// EDEN_NX2_C13_MEMO_CHECK=1 (diagnostic, off): turns on the cycle 12 checks that cover every way
/// (EDEN_NX2_C12_CAPTURE_REPEAT_CHECK: a capture hit runs pass 1 too and compares;
/// EDEN_NX2_C12_COMMIT_REPEAT_CHECK: buffer half, uniform bind memo and, through it, the C11
/// texture repeat check). Their mismatch counters must stay 0 ("C12 ... check" ERROR lines).
[[nodiscard]] inline bool C13MemoCheck() {
    static const bool on = Enabled() && C13MemoWaysMax() > 1 &&
                           (Tegra::Nx2EnvMode("EDEN_NX2_C13_MEMO_CHECK") == '1' ||
                            Tegra::Nx2EnvMode("EDEN_NX2_C13_MEMO_WAYS_CHECK") == '1');
    return on;
}

/// EDEN_NX2_C13_STATS=1 (also on with EDEN_NX2_C13_MEMO_CHECK): the "C13 memo (10 s)" line.
[[nodiscard]] inline bool C13Stats() {
    static const bool on =
        Enabled() && (Tegra::Nx2EnvMode("EDEN_NX2_C13_STATS") == '1' || C13MemoCheck());
    return on;
}

/// EDEN_NX2_C13_COMMIT_PROFILE: '1' on, '0' off; default on with EDEN_NX2_C13_STATS (W4c).
[[nodiscard]] inline bool C13CommitProfile() {
    static const bool on = [] {
        if (!Enabled()) {
            return false;
        }
        const char mode = Tegra::Nx2EnvMode("EDEN_NX2_C13_COMMIT_PROFILE");
        if (mode == '0' || mode == '1') {
            return mode == '1';
        }
        return Tegra::Nx2EnvMode("EDEN_NX2_C13_STATS") == '1';
    }();
    return on;
}

/// Counters (relaxed; the sequencer writes the capture ones, the committer the rest).
struct C13MemoCounters {
    // M1 capture (sequencer), captures that ran with more than one way.
    std::atomic<u64> cap_lookups{};
    std::atomic<u64> cap_hit_mru{};     ///< hit in the pipeline's most recently used way
    std::atomic<u64> cap_hit_other{};   ///< hit in another way (what one way would have missed)
    std::atomic<u64> cap_hit_tag{};     ///< of the hits: the way had this draw's view tag
    std::atomic<u64> cap_second{};      ///< captures that validated a second way
    std::atomic<u64> cap_throttled{};   ///< every way of the pipeline in a miss streak
    std::atomic<u64> cap_new{};         ///< no way of this pipeline in the slot
    std::atomic<u64> cap_evict_other{}; ///< the victim held another pipeline
    std::atomic<u64> cap_replace_tag{}; ///< the victim was this pipeline's same-view way
    std::atomic<u64> cap_replace_lru{}; ///< the victim was this pipeline's least recent way
    std::atomic<u64> cap_ways_used{};   ///< sum over lookups of the pipeline's ways in the slot
    // Texture memo by packet (committer).
    std::atomic<u64> tex_lookups{};  ///< packet draws with repeat_of and more than one way
    std::atomic<u64> tex_active{};   ///< repeat_of was the active memo's packet
    std::atomic<u64> tex_spare{};    ///< repeat_of was a kept way's packet (swapped in)
    std::atomic<u64> tex_missing{};  ///< repeat_of in no way
    std::atomic<u64> tex_rotate{};   ///< the active memo kept as a way before a fill
    // M2 buffer half (committer).
    std::atomic<u64> buf_tries{};
    std::atomic<u64> buf_hit_mru{};
    std::atomic<u64> buf_hit_other{};
    std::atomic<u64> buf_replace{};  ///< full resolution saved into the least recent way
    // M3 uniform bind memo (committer, hook lines).
    std::atomic<u64> ubind_promote{};       ///< translation found in another way
    std::atomic<u64> ubind_demote{};        ///< way 0 kept as another way before a fill
    std::atomic<u64> ubind_resolve_other{}; ///< resolution found in another way
};

inline C13MemoCounters g_c13_memo{};

inline void C13Add(std::atomic<u64>& counter, u64 value = 1) {
    if (C13Stats()) {
        counter.fetch_add(value, std::memory_order_relaxed);
    }
}

/// N entries of one memo type for one owner: way 0 is the owner's existing (cycle 12) memo, the
/// other ways are allocated at the first use with more than one way. Single-threaded (the
/// owner's thread).
template <typename Memo>
struct C13WaySet {
    std::unique_ptr<Memo[]> extra;                  ///< ways 1 .. kC13MaxWays - 1
    std::array<u64, kC13MaxWays> last_use{};        ///< 0: never used
    u64 clock = 0;
    u32 active = 1;                                  ///< ways the current draw may use

    void Ensure() {
        if (!extra) {
            extra = std::make_unique<Memo[]>(kC13MaxWays - 1);
        }
    }
    [[nodiscard]] Memo& Way(Memo& first, u32 way) {
        return way == 0 ? first : extra[way - 1];
    }
    void Touch(u32 way) {
        last_use[way] = ++clock;
    }
    /// Ways 0 .. count-1, most recently used first.
    void MruOrder(u32 count, std::array<u32, kC13MaxWays>& out) const {
        for (u32 way = 0; way < count; ++way) {
            out[way] = way;
        }
        std::sort(out.begin(), out.begin() + count,
                  [this](u32 a, u32 b) { return last_use[a] > last_use[b]; });
    }
    /// The least recently used of ways 0 .. count-1 (a never used way first).
    [[nodiscard]] u32 LruWay(u32 count) const {
        u32 best = 0;
        for (u32 way = 1; way < count; ++way) {
            if (last_use[way] < last_use[best]) {
                best = way;
            }
        }
        return best;
    }
};

/// M2: the pipeline's buffer way set for C12UpdateGraphicsBuffers (a C13WaySet of
/// C12GraphicsBufferMemo; null: one way). Set by GraphicsPipeline::ConfigureImpl around the call,
/// committer thread only.
inline thread_local void* t_c13_buffer_ways = nullptr;

/// M3 helpers for small trivially copyable memos kept as `first` + `extra` (most recently used
/// first). Promote: the extra way that matches becomes `first` (the old first goes to the front
/// of `extra`). Returns whether one matched.
template <typename Memo, size_t N, typename Match>
[[nodiscard]] inline bool C13PromoteWay(Memo& first, std::array<Memo, N>& extra, u32 ways,
                                        Match&& match) {
    const size_t count = std::min<size_t>(ways > 0 ? ways - 1 : 0, N);
    for (size_t k = 0; k < count; ++k) {
        if (!match(static_cast<const Memo&>(extra[k]))) {
            continue;
        }
        const Memo found = extra[k];
        for (size_t i = k; i > 0; --i) {
            extra[i] = extra[i - 1];
        }
        extra[0] = first;
        first = found;
        return true;
    }
    return false;
}

/// Demote: `first` is kept as the most recent extra way (the least recent one is dropped) before
/// the caller overwrites `first`.
template <typename Memo, size_t N>
inline void C13DemoteFirst(const Memo& first, std::array<Memo, N>& extra, u32 ways) {
    const size_t count = std::min<size_t>(ways > 0 ? ways - 1 : 0, N);
    if (count == 0) {
        return;
    }
    for (size_t i = count - 1; i > 0; --i) {
        extra[i] = extra[i - 1];
    }
    extra[0] = first;
}

/// Finds an extra way by a predicate (no reordering): its index, or N.
template <typename Memo, size_t N, typename Match>
[[nodiscard]] inline size_t C13FindWay(const std::array<Memo, N>& extra, u32 ways, Match&& match) {
    const size_t count = std::min<size_t>(ways > 0 ? ways - 1 : 0, N);
    for (size_t k = 0; k < count; ++k) {
        if (match(extra[k])) {
            return k;
        }
    }
    return N;
}

// ---- W4c: committer draw-path profile (sampled) ----

enum class C13Phase : u32 {
    Setup,           ///< packet parse, draw state copy, FlushWork
    Uploads,         ///< MtApplyUploads (buffer lock included)
    PreConfigure,    ///< resolver validation, ConfigureDrawPipeline up to ConfigureImpl (locks)
    TexResolve,      ///< descriptor sync, texture repeat/memo, handles, FillImageViews
    BufResolve,      ///< UpdateGraphicsBuffers (or the C12 buffer half), geometry binds
    DescAcquire,     ///< guest descriptor queue acquire
    StageBind,       ///< BindHostStageBuffers (uniform streaming, buffer descriptor entries)
    PushDescriptors, ///< image descriptor entries (pushed, replayed or resolved)
    RenderTargets,   ///< UpdateRenderTargets, feedback loop check
    ConfigureDraw,   ///< dynamic state, descriptor set update, pipeline bind, push constants
    DrawRecord,      ///< RecordDraw / RecordDrawIndirect
    Tail,            ///< after the record (unlocks, shadow compare, counters)
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(C13Phase::Count)> c13_phase_names{
    "setup",          "uploads",       "pre-configure", "texture resolve",
    "buffer resolve", "desc acquire",  "stage bind",    "push descriptors",
    "render targets", "configure draw", "draw record",  "tail",
};

struct C13ProfileThread {
    bool sampling = false;
    u32 counter = 0;
    std::chrono::steady_clock::time_point last{};
    std::array<u64, static_cast<size_t>(C13Phase::Count)> ns{};
    u64 samples = 0;
    u64 draws = 0;
    std::chrono::steady_clock::time_point window{};
};

inline thread_local C13ProfileThread t_c13_prof;

inline constexpr u32 kC13ProfileEvery = 32;

/// Committer, at the start of a draw packet.
inline void C13ProfBegin() {
    if (!C13CommitProfile()) {
        return;
    }
    C13ProfileThread& t = t_c13_prof;
    ++t.draws;
    if ((++t.counter % kC13ProfileEvery) != 0) {
        t.sampling = false;
        return;
    }
    t.sampling = true;
    ++t.samples;
    t.last = std::chrono::steady_clock::now();
}

/// The time since the previous mark goes to `phase` (only on a sampled draw of this thread).
inline void C13ProfMark(C13Phase phase) {
    C13ProfileThread& t = t_c13_prof;
    if (!t.sampling) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    t.ns[static_cast<size_t>(phase)] += static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - t.last).count());
    t.last = now;
}

/// Committer, at the end of a draw packet: closes the sample and logs every 10 s.
inline void C13ProfEnd() {
    if (!C13CommitProfile()) {
        return;
    }
    C13ProfileThread& t = t_c13_prof;
    if (!t.sampling) {
        return;
    }
    C13ProfMark(C13Phase::Tail);
    t.sampling = false;
    const auto now = std::chrono::steady_clock::now();
    if (t.window == std::chrono::steady_clock::time_point{}) {
        t.window = now;
        return;
    }
    if (now - t.window < std::chrono::seconds{10} || t.samples == 0) {
        return;
    }
    const double window_s =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - t.window).count()) /
        1e6;
    const auto phase_ns = [&t](C13Phase phase) { return t.ns[static_cast<size_t>(phase)]; };
    u64 total = 0;
    for (const u64 value : t.ns) {
        total += value;
    }
    const u64 resolve = phase_ns(C13Phase::Uploads) + phase_ns(C13Phase::TexResolve) +
                        phase_ns(C13Phase::BufResolve) + phase_ns(C13Phase::RenderTargets);
    const u64 encode = phase_ns(C13Phase::DescAcquire) + phase_ns(C13Phase::PushDescriptors) +
                       phase_ns(C13Phase::ConfigureDraw) + phase_ns(C13Phase::DrawRecord);
    const u64 mixed = phase_ns(C13Phase::StageBind);
    const u64 other = total - resolve - encode - mixed;
    const double samples = static_cast<double>(t.samples);
    const auto pct = [total](u64 part) {
        return total != 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(total) : 0.0;
    };
    std::string phases;
    for (size_t i = 0; i < t.ns.size(); ++i) {
        phases += fmt::format("{}{} {:.2f}", i == 0 ? "" : ", ", c13_phase_names[i],
                              static_cast<double>(t.ns[i]) / samples / 1000.0);
    }
    const double per_draw_us = static_cast<double>(total) / samples / 1000.0;
    LOG_INFO(HW_GPU,
             "C13 commit profile (10 s): {} draws ({:.0f}/s), {} sampled (1/{}); per draw {:.2f} "
             "us (est. {:.0f} ms/s): {} us; resolve {:.1f}%, encode {:.1f}%, stage bind {:.1f}%, "
             "other {:.1f}%; gate: encode {} 40% ({})",
             t.draws, static_cast<double>(t.draws) / window_s, t.samples, kC13ProfileEvery,
             per_draw_us, per_draw_us * static_cast<double>(t.draws) / window_s / 1000.0, phases,
             pct(resolve), pct(encode), pct(mixed), pct(other), pct(encode) > 40.0 ? ">" : "<=",
             pct(encode) > 40.0 ? "resolve/encode committer split" : "per-view capture split");
    t.ns.fill(0);
    t.samples = 0;
    t.draws = 0;
    t.window = now;
}

// ---- Start line and the 10 s memo line ----

/// Once (the sequencer's or the committer's start, whichever comes first).
inline void C13MemoLogStart() {
    static std::atomic<bool> logged{};
    if (!Enabled() || logged.exchange(true)) {
        return;
    }
    const C13Detail::WaysConfig& config = C13Detail::Ways();
    LOG_INFO(HW_GPU,
             "Cycle 13 memo: ways {} (mode {}{}), tag {}, check {}, stats {}, commit profile {}",
             config.ways, config.mode, config.alternating ? ", alternating with 1" : "",
             C13MemoTagMode(), C13MemoCheck(), C13Stats(), C13CommitProfile());
}

/// Committer: logs and resets the counters every 10 s (C13 STATS or CHECK).
inline void C13MemoMaybeLog() {
    if (!C13Stats()) {
        return;
    }
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds{10}) {
        return;
    }
    last = now;
    C13MemoCounters& c = g_c13_memo;
    const auto take = [](std::atomic<u64>& value) {
        return value.exchange(0, std::memory_order_relaxed);
    };
    const auto pct = [](u64 part, u64 whole) {
        return whole != 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
    };
    const u64 lookups = take(c.cap_lookups);
    const u64 hit_mru = take(c.cap_hit_mru);
    const u64 hit_other = take(c.cap_hit_other);
    const u64 hit_tag = take(c.cap_hit_tag);
    const u64 second = take(c.cap_second);
    const u64 throttled = take(c.cap_throttled);
    const u64 fresh = take(c.cap_new);
    const u64 evict_other = take(c.cap_evict_other);
    const u64 replace_tag = take(c.cap_replace_tag);
    const u64 replace_lru = take(c.cap_replace_lru);
    const u64 ways_used = take(c.cap_ways_used);
    const u64 tex_lookups = take(c.tex_lookups);
    const u64 tex_active = take(c.tex_active);
    const u64 tex_spare = take(c.tex_spare);
    const u64 tex_missing = take(c.tex_missing);
    const u64 tex_rotate = take(c.tex_rotate);
    const u64 buf_tries = take(c.buf_tries);
    const u64 buf_mru = take(c.buf_hit_mru);
    const u64 buf_other = take(c.buf_hit_other);
    const u64 buf_replace = take(c.buf_replace);
    const u64 ub_promote = take(c.ubind_promote);
    const u64 ub_demote = take(c.ubind_demote);
    const u64 ub_resolve = take(c.ubind_resolve_other);
    LOG_INFO(HW_GPU,
             "C13 memo (10 s): ways {}; capture {} lookups ({:.2f} ways/pipeline), hits mru {} / "
             "other way {} ({:.1f}% of lookups; same view tag {}), second validations {}, "
             "throttled {}, new {}, victims: other pipeline {}, same view {}, lru {}; textures "
             "{} lookups: active {}, other way {}, missing {}, kept {}; buffers {} tries: mru "
             "{}, other way {} ({:.1f}%), replaced {}; ubind other-way translations {}, demoted "
             "{}, other-way resolutions {}",
             C13MemoWaysMax(), lookups,
             lookups != 0 ? static_cast<double>(ways_used) / static_cast<double>(lookups) : 0.0,
             hit_mru, hit_other, pct(hit_other, lookups), hit_tag, second, throttled, fresh,
             evict_other, replace_tag, replace_lru, tex_lookups, tex_active, tex_spare,
             tex_missing, tex_rotate, buf_tries, buf_mru, buf_other, pct(buf_other, buf_tries),
             buf_replace, ub_promote, ub_demote, ub_resolve);
}

} // namespace VideoCommon::MT
