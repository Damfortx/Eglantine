// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/spin_recursive_mutex.h"
#include "video_core/mt/c12_commit.h"
#include "video_core/mt/lite7_capture_switches.h"
#include "video_core/mt/mt_switches.h"
#include "video_core/nx2.h"

/// Cycle 11, workstreams #4 (W4a, capture repeat) and #5 (W4b, one lock per run) of
/// docs/cycle11-plan.md: the committer's per-draw work. Each piece has its own switch: on by
/// default under EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 (REPEAT: off by default since the test
/// round), '0' off (the code paths before cycle 11), '1'
/// on without LITE (still only with EDEN_NX2_MT), 'a' alternating (Nx2AltOn, decided per capture
/// for REPEAT and per run for LOCKRUN). Modes are read once.
namespace VideoCommon::MT {

/// EDEN_NX2_C11_REPEAT (W4a): the sequencer keeps, per graphics pipeline, the texture inputs of
/// its last capture (TIC/TSC table addresses and limits, every texture/image handle and the 32
/// bytes of every TIC/TSC entry they select). A capture whose inputs are byte-identical sets
/// the packet's repeat delta (seq distance to that capture). The committer then reuses the
/// pipeline's TextureMemo (view and sampler ids, descriptor entries, rescaling words) without
/// reading handles, TIC/TSC entries (FillImageViews / GetSamplerId) or pushing descriptors, if
/// the memo was set by exactly that packet and the channel, view epoch, bindings serial and GPU
/// page table layout are unchanged and every image is clean (no CPU writes to upload, no
/// aliases to synchronize, no blacklisted rescaled view). Written images still get their
/// MarkModification.
///
/// Default OFF since the cycle 11 test round (docs/cycle11-analysis.md): 42-59 % of captures
/// with textures are identical, but ~35 % of those are refused at commit (image: CPU-modified /
/// aliased), so ~25 % of textured draws could take it, on the committer, which had 340-380 ms/s
/// idle in the race (not the ceiling: the plan's own condition for a gain). Off, the sequencer
/// builds no key and the committer fills no memo per draw. '1' on, 'a' alternates per capture
/// (the A/B the next round needs, with EDEN_NX2_C11_REPEAT_STATS=1).
[[nodiscard]] inline char C11RepeatMode() {
    static const char mode = [] {
        if (!Enabled()) {
            return '0';
        }
        const char value = Tegra::Nx2EnvMode("EDEN_NX2_C11_REPEAT");
        if (value == '1' || value == 'a') {
            return value;
        }
        // Cycle 12 W2 (c12_commit.h): the texture half of EDEN_NX2_C12_COMMIT_REPEAT. The
        // sequencer flags every identical capture; the committer decides per draw (C12 'a').
        if (value != '0' && C12TextureRepeatEnabled()) {
            return '1';
        }
        return '0';
    }();
    return mode;
}

/// EDEN_NX2_C11_REPEAT set to '1' or 'a' by itself (not through EDEN_NX2_C12_COMMIT_REPEAT): the
/// committer then takes the cycle 11 decisions whatever the C12 switch says for the draw.
[[nodiscard]] inline bool C11RepeatExplicit() {
    static const bool on = [] {
        const char value = Tegra::Nx2EnvMode("EDEN_NX2_C11_REPEAT");
        return Enabled() && (value == '1' || value == 'a');
    }();
    return on;
}

/// EDEN_NX2_C11_REPEAT_CHECK=1 (diagnostic, off; not part of LITE): a draw the repeat path
/// would take runs the full path instead and its view ids, sampler ids, descriptor entries and
/// rescaling words are compared with the memo's. Mismatches must stay 0 ("C11 repeat:" line).
[[nodiscard]] inline bool C11RepeatCheck() {
    static const bool on =
        Enabled() && (Tegra::Nx2EnvMode("EDEN_NX2_C11_REPEAT_CHECK") == '1' ||
                      (C12CommitRepeatCheck() && C12TextureRepeatEnabled()));
    return on;
}

/// EDEN_NX2_C11_REPEAT_STATS=1 (or EDEN_NX2_MT_STATS=1): the go/no-go counters, logged every 10 s
/// by the committer: captures with textures, repeats flagged, repeats taken, refusals by reason.
[[nodiscard]] inline bool C11RepeatStats() {
    static const bool on =
        Enabled() && (Tegra::Nx2EnvMode("EDEN_NX2_C11_REPEAT_STATS") == '1' ||
                      Tegra::Nx2EnvMode("EDEN_NX2_MT_STATS") == '1' ||
                      (C12CommitStats() && C12TextureRepeatEnabled()));
    return on;
}

/// EDEN_NX2_C11_LOCKRUN (W4b): the committer takes both cache mutexes once for a run of draw
/// packets (with the register deltas, uniform binds, root tables and marks between them) instead
/// of twice per draw (uploads under the buffer mutex, then both for Configure). A run ends
/// before any other entry (Commands, Serials, compute packets, batch ends), before the committer
/// goes idle, after EDEN_NX2_C11_LOCKRUN_MAX draws (default 16), before a draw whose FlushWork
/// dispatches or submits work, and as soon as another thread waits for one of the mutexes
/// (SpinRecursiveMutex::Contended). Off with MT_VERIFY / lockstep / resolvers. Its counters are
/// logged with REPEAT_STATS ("C11 lockrun:").
[[nodiscard]] inline char C11LockRunMode() {
    static const char mode = Lite7Detail::Mode("EDEN_NX2_C11_LOCKRUN");
    return mode;
}

/// EDEN_NX2_C11_LOCKRUN_MAX: draws per run (1-256, default 16).
[[nodiscard]] inline u32 C11LockRunMax() {
    static const u32 value = [] {
        const char* text = std::getenv("EDEN_NX2_C11_LOCKRUN_MAX");
        if (text == nullptr || text[0] < '0' || text[0] > '9') {
            return 16U;
        }
        const unsigned long parsed = std::strtoul(text, nullptr, 10);
        return parsed == 0 ? 16U : static_cast<u32>(parsed > 256 ? 256 : parsed);
    }();
    return value;
}

/// EDEN_NX2_C11_LOCKRUN_TRACKER=1 (contingent on EDEN_NX2_LOCK_STATS showing tracker_mutex
/// contention between the committer and the sequencer; off by default, also under LITE): a run
/// also holds the buffer cache's tracker_mutex (EDEN_NX2_MT_TRAPLOCK), noted for
/// EDEN_NX2_MT_LITE7_LOCKSKIP, so the committer's per-draw TrackerScopes (MarkWrittenBuffer,
/// IsRegionGpuModified, the written-range batch) skip their lock. The sequencer's captures wait
/// for it: a waiting sequencer ends the run like any contender.
[[nodiscard]] inline bool C11LockRunTracker() {
    static const bool on = Enabled() && Tegra::Nx2EnvMode("EDEN_NX2_C11_LOCKRUN_TRACKER") == '1';
    return on;
}

/// Counters (relaxed atomics; REPEAT: the sequencer and the committer write them, LOCKRUN: the
/// committer only).
struct C11CommitCounters {
    // Sequencer.
    std::atomic<u64> captures{};       ///< captures with texture/image elements, REPEAT on
    std::atomic<u64> flagged{};        ///< ... byte-identical to the pipeline's previous capture
    std::atomic<u64> key_far{};        ///< identical, but too many draws apart for the header
    // Committer.
    std::atomic<u64> packets_flagged{}; ///< flagged packets that reached Configure
    std::atomic<u64> taken{};           ///< repeat path used
    std::atomic<u64> refused_seq{};     ///< memo not set by the flagged capture's packet
    std::atomic<u64> refused_state{};   ///< channel / view epoch / bindings serial / layout moved
    std::atomic<u64> refused_image{};   ///< an image needs preparation (CPU write, alias, scale)
    std::atomic<u64> cpu_epoch_moved{}; ///< taken with the CPU-modified epoch moved (flags clean)
    std::atomic<u64> checks{};          ///< REPEAT_CHECK comparisons
    std::atomic<u64> check_mismatches{};
    // LOCKRUN.
    std::atomic<u64> runs{};
    std::atomic<u64> run_draws{};
    std::atomic<u64> end_max{};
    std::atomic<u64> end_contended{};
    std::atomic<u64> end_flush{};
    std::atomic<u64> end_entry{}; ///< another kind of entry
    std::atomic<u64> end_idle{};  ///< no entry ready (and stop / hooks)
};

inline C11CommitCounters g_c11_commit{};

/// Start line (once, when the committer starts).
inline void C11CommitLogStart(bool lockrun_allowed) {
    if (!Enabled()) {
        return;
    }
    LOG_INFO(HW_GPU,
             "Cycle 11 committer: repeat {}, repeatcheck {}, repeatstats {}, lockrun {}{} "
             "(max {} draws, tracker {}), lock tls id {}",
             Lite7Detail::Name(C11RepeatMode()), C11RepeatCheck(), C11RepeatStats(),
             Lite7Detail::Name(C11LockRunMode()),
             lockrun_allowed ? "" : " (inactive: verify/lockstep)", C11LockRunMax(),
             C11LockRunTracker(), Common::g_lock_tls_id);
}

/// Committer: logs and resets the counters every 10 s (REPEAT_STATS / MT_STATS, or CHECK).
inline void C11CommitMaybeLog() {
    if (!C11RepeatStats() && !C11RepeatCheck()) {
        return;
    }
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds{10}) {
        return;
    }
    last = now;
    C11CommitCounters& c = g_c11_commit;
    const auto take = [](std::atomic<u64>& value) {
        return value.exchange(0, std::memory_order_relaxed);
    };
    const u64 captures = take(c.captures);
    const u64 flagged = take(c.flagged);
    const u64 too_far = take(c.key_far);
    const u64 packets = take(c.packets_flagged);
    const u64 taken = take(c.taken);
    const u64 r_seq = take(c.refused_seq);
    const u64 r_state = take(c.refused_state);
    const u64 r_image = take(c.refused_image);
    const u64 moved = take(c.cpu_epoch_moved);
    const u64 checks = take(c.checks);
    const u64 mismatches = take(c.check_mismatches);
    LOG_INFO(HW_GPU,
             "C11 repeat (10 s): {} captures with textures, {} identical ({:.1f}%, {} too far), "
             "{} flagged at commit, {} taken (refused: seq {}, state {}, image {}; cpu epoch "
             "moved {}), check {} / mismatches {}",
             captures, flagged,
             captures != 0 ? 100.0 * static_cast<double>(flagged) / static_cast<double>(captures)
                           : 0.0,
             too_far, packets, taken, r_seq, r_state, r_image, moved, checks, mismatches);
    if (mismatches != 0) {
        LOG_ERROR(HW_GPU, "C11 repeat: {} mismatches between the memo and the full path",
                  mismatches);
    }
    const u64 runs = take(c.runs);
    const u64 run_draws = take(c.run_draws);
    const u64 e_max = take(c.end_max);
    const u64 e_cont = take(c.end_contended);
    const u64 e_flush = take(c.end_flush);
    const u64 e_entry = take(c.end_entry);
    const u64 e_idle = take(c.end_idle);
    if (runs != 0) {
        LOG_INFO(HW_GPU,
                 "C11 lockrun (10 s): {} runs, {} draws ({:.1f}/run); ended by max {}, "
                 "contended {}, flush {}, other entry {}, idle {}",
                 runs, run_draws, static_cast<double>(run_draws) / static_cast<double>(runs),
                 e_max, e_cont, e_flush, e_entry, e_idle);
    }
}

} // namespace VideoCommon::MT
