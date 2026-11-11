// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/c11_front.h"
#include "video_core/mt/c12_front.h"
#include "video_core/mt/lite4_switches.h"
#include "video_core/mt/lite_switches.h"
#include "video_core/nx2.h"

/// Cycle 13, workstream W3 "front list writer waits" (docs/cycle13-plan.md W3). Nothing without
/// EDEN_NX2_MT. Start line: "Cycle 13 front: ..." (C13FrontLogStart, once, from
/// GpuSplit::MaybeStart). Stats line every 10 s: "C13 front (10 s): ..." (EDEN_NX2_C13_STATS=1
/// or any switch below marked CHECK).
///
///  - L0 (data wiring): EDEN_NX2_C13_LIST_CHECK=1 turns on, in one switch, the existing
///    EDEN_NX2_C12_LISTWAIT_CHECK census, the EDEN_NX2_C12_LISTRANGE gate and
///    EDEN_NX2_C13_KICK_CHECK (each still forced off by its own variable set to '0'). The C13
///    line carries the run totals the plan's L3 decision needs ("same" share, "would differ").
///  - L1 census by shader hash (EDEN_NX2_C13_STATS): the list writer waits on compute keyed by
///    the program's shader hash (published by the sequencer, C13PublishProgramHash; '@address'
///    when the hash is not known yet), with per program: method calls and microseconds between
///    the writer's send and the fetch, the back's backlog at the wait (ring words behind the
///    sequencer / the committer) and the split of the wait into sequencer (WaitConsumed) and
///    committer (TimedWaitSettled) parts.
///  - L2 early kick (EDEN_NX2_C13_KICK, CHECK): a dispatch of a program learned as a list writer
///    (EDEN_NX2_C13_KICK_LEARN waits, default 16) is published at once and its ring position
///    becomes the back's wait target (the sequencer Marks there, the committer settles there and,
///    with the hooks, ends its lock run there; the sequencer skips C12 memo record work for the
///    draws before it), so the back settles while the front parses the methods in between.
namespace VideoCommon::MT {

// ---------------------------------------------------------------------------------------------
// Switches
// ---------------------------------------------------------------------------------------------

namespace C13FrontDetail {
/// A diagnostic switch: '1' on, '0' off, unset: on with EDEN_NX2_C13_LIST_CHECK=1.
[[nodiscard]] inline bool CheckSwitch(const char* name) {
    const char mode = Tegra::Nx2EnvMode(name);
    return Enabled() && (mode == '1' || (mode == 'd' && C13Shared::ListCheckEnv()));
}
[[nodiscard]] inline u32 EnvCount(const char* name, u32 fallback, u32 max_value) {
    const char* text = std::getenv(name);
    const long long parsed = text != nullptr ? std::atoll(text) : 0;
    return parsed > 0 ? static_cast<u32>(std::min<long long>(parsed, max_value)) : fallback;
}
} // namespace C13FrontDetail

/// EDEN_NX2_C13_KICK (L2; on by default under EDEN_NX2_MT_LITE, '0' = the cycle 12 path, 'a'
/// = decided per kicked dispatch with Nx2AltOn: the waits on it are the A/B halves). Needs
/// EDEN_NX2_MT_LITE_SEQTARGET (the sequencer clears the target once past it; without it a
/// target left set makes it Mark after every entry): off without it.
[[nodiscard]] inline char C13KickMode() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_C13_KICK");
    return mode;
}
/// Whether kicks can happen at all in this run (learning runs then).
[[nodiscard]] inline bool C13KickConfigured() {
    static const bool on = Enabled() && LiteSeqTarget() &&
                           (C13KickMode() == '1' || C13KickMode() == 'a' ||
                            (C13KickMode() != '0' && Lite()));
    return on;
}
/// Per kick (Nx2AltOn for 'a').
[[nodiscard]] inline bool C13Kick() {
    return C13KickConfigured() && Tegra::Nx2Switch(C13KickMode(), true);
}
/// EDEN_NX2_C13_KICK_CHECK=1 (also with EDEN_NX2_C13_LIST_CHECK=1): counts the waits that still
/// found the back behind a kicked target, and their time. No content risk: the wait is the
/// cycle 12 wait in every case.
[[nodiscard]] inline bool C13KickCheck() {
    static const bool on = C13FrontDetail::CheckSwitch("EDEN_NX2_C13_KICK_CHECK");
    return on;
}
/// EDEN_NX2_C13_KICK_LEARN (default 16): waits on a program before its dispatches are kicked.
[[nodiscard]] inline u32 C13KickLearn() {
    static const u32 value = C13FrontDetail::EnvCount("EDEN_NX2_C13_KICK_LEARN", 16, 1'000'000);
    return value;
}
/// EDEN_NX2_C13_KICK_SEQ (with KICK, default on; '0' off): the sequencer skips C12 memo record
/// work for draws before a pending kick (hook in vk_mt_seq.h, docs/cycle13-hooks-W3.md).
[[nodiscard]] inline bool C13KickSeq() {
    static const bool on =
        C13KickConfigured() && Tegra::Nx2EnvMode("EDEN_NX2_C13_KICK_SEQ") != '0';
    return on;
}
/// EDEN_NX2_C13_KICK_RUN (with KICK, default on; '0' off): the committer ends its lock run at
/// the kick's Mark (hook in vk_mt_pipeline.h, docs/cycle13-hooks-W3.md).
[[nodiscard]] inline bool C13KickRun() {
    static const bool on =
        C13KickConfigured() && Tegra::Nx2EnvMode("EDEN_NX2_C13_KICK_RUN") != '0';
    return on;
}
/// EDEN_NX2_C13_STATS=1 (also with any check above, or EDEN_NX2_C13_LIST_CHECK=1): the
/// "C13 front (10 s)" line with the L1 census.
[[nodiscard]] inline bool C13FrontStatsOn() {
    static const bool on = Enabled() && (std::getenv("EDEN_NX2_C13_STATS") != nullptr ||
                                         C13KickCheck() || C13Shared::ListCheckEnv());
    return on;
}
/// The front's program bookkeeping (program of the last compute writer, learning) runs.
[[nodiscard]] inline bool C13FrontTrack() {
    static const bool on = C13FrontStatsOn() || C13KickConfigured();
    return on;
}

// ---------------------------------------------------------------------------------------------
// Program -> shader hash (published by the sequencer, read by the front)
// ---------------------------------------------------------------------------------------------

/// Fixed table keyed like C11ComputeWrites (address space + program address). The sequencer
/// stores the compute pipeline's shader hash at capture (hook in Nx2DrawPipeline::CaptureCompute,
/// docs/cycle13-hooks-W3.md); the front reads it for the census key. Program addresses are heap
/// addresses that move between runs; the shader hash does not.
class C13ProgramHashes {
public:
    static constexpr u32 Slots = 1024;

    static C13ProgramHashes& Get() {
        static C13ProgramHashes instance;
        return instance;
    }
    /// Sequencer.
    void Publish(u64 address_space, u64 program, u64 hash) {
        const u64 key = C11ComputeWrites::Key(address_space, program);
        if (key == 0) {
            return;
        }
        Slot& slot = slots[Index(key)];
        if (slot.key.load(std::memory_order_relaxed) == key &&
            slot.hash.load(std::memory_order_relaxed) == hash) {
            return; // the per-dispatch fast path
        }
        slot.key.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.hash.store(hash, std::memory_order_relaxed);
        slot.key.store(key, std::memory_order_release);
    }
    /// Front: the program's hash, if published.
    [[nodiscard]] bool Lookup(u64 address_space, u64 program, u64& hash) const {
        const u64 key = C11ComputeWrites::Key(address_space, program);
        if (key == 0) {
            return false;
        }
        const Slot& slot = slots[Index(key)];
        if (slot.key.load(std::memory_order_acquire) != key) {
            return false;
        }
        hash = slot.hash.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        return slot.key.load(std::memory_order_relaxed) == key;
    }

private:
    struct Slot {
        std::atomic<u64> key{};
        std::atomic<u64> hash{};
    };
    [[nodiscard]] static u32 Index(u64 key) {
        return static_cast<u32>((key ^ (key >> 17) ^ (key >> 31)) * 0x9E3779B1u) % Slots;
    }
    std::array<Slot, Slots> slots{};
};

/// Hook entry for the sequencer (CaptureCompute): a no-op unless the C13 front census runs.
inline void C13PublishProgramHash(u64 address_space, u64 program, u64 hash) {
    if (C13FrontStatsOn()) {
        C13ProgramHashes::Get().Publish(address_space, program, hash);
    }
}

/// Back-side counters (hooks; any back thread).
inline std::atomic<u64> g_c13_kick_marks{};     ///< Committer: kick Marks seen
inline std::atomic<u64> g_c13_kick_run_ends{};  ///< ... that ended a held lock run
inline std::atomic<u64> g_c13_seq_skips{};      ///< Sequencer: memo records skipped before a kick

// ---------------------------------------------------------------------------------------------
// L2 learning (front thread)
// ---------------------------------------------------------------------------------------------

/// Programs (by address: stable within a run) seen as the last writer of waited list fetches.
class C13WriterLearn {
public:
    static C13WriterLearn& Get() {
        static C13WriterLearn instance;
        return instance;
    }
    /// A list fetch waited (the back behind) on a writer call that dispatched `program`.
    void NoteWait(u64 program) {
        if (program == 0) {
            return;
        }
        if (waits.size() >= 4096 && !waits.contains(program)) {
            std::erase_if(waits, [](const auto& item) { return item.second < C13KickLearn(); });
            if (waits.size() >= 4096) {
                return;
            }
        }
        u32& count = waits[program];
        if (count < C13KickLearn()) {
            ++count;
            learned_new += count == C13KickLearn() ? 1 : 0;
        }
    }
    [[nodiscard]] bool IsWriter(u64 program) const {
        if (program == 0) {
            return false;
        }
        const auto it = waits.find(program);
        return it != waits.end() && it->second >= C13KickLearn();
    }
    [[nodiscard]] size_t Learned() const {
        return static_cast<size_t>(std::count_if(waits.begin(), waits.end(), [](const auto& item) {
            return item.second >= C13KickLearn();
        }));
    }
    u64 learned_new{};

private:
    std::unordered_map<u64, u32> waits;
};

// ---------------------------------------------------------------------------------------------
// Stats (front thread)
// ---------------------------------------------------------------------------------------------

/// What DmaPusher measured of one waited list fetch whose last writer was compute.
struct C13ListWaitSample {
    u64 program{};       ///< Program address of the writer call's last dispatch (0: none)
    u64 address_space{};
    u64 wait_ns{};       ///< The whole writer wait
    u64 seq_ns{};        ///< ... of it in WaitConsumed (the sequencer)
    u64 settle_ns{};     ///< ... of it in TimedWaitSettled (the committer)
    u64 since_ns{};      ///< Writer sent -> this fetch
    u64 calls{};         ///< DmaPusher method calls between them
    u64 words{};         ///< Ring words sent between them
    u64 backlog_seq{};   ///< Ring words the sequencer was behind at the wait
    u64 backlog_settle{};///< ... the committer
    bool learned{};      ///< The program was a learned writer at its dispatch
    bool kicked{};       ///< ... and the dispatch was kicked
    bool kick_behind{};  ///< CHECK: the back was still behind the kicked target at the fetch
};

struct C13FrontStats {
    static C13FrontStats& Get() {
        static C13FrontStats instance;
        return instance;
    }

    struct Program {
        u64 waits{};
        u64 ns{};
        u64 seq_ns{};
        u64 settle_ns{};
        u64 since_ns{};
        u64 calls{};
        u64 words{};
        u64 backlog_seq{};
        u64 backlog_settle{};
        u64 kicked{};
        u64 kicked_ns{};
        u64 unkicked{};    ///< Learned, not kicked ('a' off half)
        u64 unkicked_ns{};
    };
    /// Key: (hashed, hash or program address).
    std::map<std::pair<bool, u64>, Program> programs;
    u64 waits_other{};     ///< Waits past the 128-key table
    // L2
    u64 kicks{};           ///< Dispatches kicked
    u64 kicks_off{};       ///< Learned dispatches not kicked ('a' off half)
    u64 kick_restored{};   ///< A front wait for an earlier position put the kick target back
    u64 learned_waits{};
    u64 kicked_waits{};
    u64 kicked_ns{};
    u64 unkicked_waits{};
    u64 unkicked_ns{};
    u64 kick_behind{};     ///< CHECK: kicked waits that still found the back behind the target
    u64 kick_behind_ns{};
    // L0 run totals (not reset): EDEN_NX2_C12_LISTWAIT_CHECK content census
    u64 run_same{};
    u64 run_changed{};
    u64 run_unreadable{};
    u64 run_would{};
    u64 run_would_differ{};
    u64 window{};

    void NoteWait(const C13ListWaitSample& sample, bool check) {
        u64 hash = 0;
        const bool hashed = sample.program != 0 &&
                            C13ProgramHashes::Get().Lookup(sample.address_space, sample.program,
                                                           hash);
        const std::pair<bool, u64> key{hashed, hashed ? hash : sample.program};
        if (programs.size() < 128 || programs.contains(key)) {
            Program& p = programs[key];
            ++p.waits;
            p.ns += sample.wait_ns;
            p.seq_ns += sample.seq_ns;
            p.settle_ns += sample.settle_ns;
            p.since_ns += sample.since_ns;
            p.calls += sample.calls;
            p.words += sample.words;
            p.backlog_seq += sample.backlog_seq;
            p.backlog_settle += sample.backlog_settle;
            if (sample.learned) {
                (sample.kicked ? p.kicked : p.unkicked) += 1;
                (sample.kicked ? p.kicked_ns : p.unkicked_ns) += sample.wait_ns;
            }
        } else {
            ++waits_other;
        }
        if (sample.learned) {
            ++learned_waits;
            if (sample.kicked) {
                ++kicked_waits;
                kicked_ns += sample.wait_ns;
                if (check && sample.kick_behind) {
                    ++kick_behind;
                    kick_behind_ns += sample.wait_ns;
                }
            } else {
                ++unkicked_waits;
                unkicked_ns += sample.wait_ns;
            }
        }
    }
    /// L0: one EDEN_NX2_C12_LISTWAIT_CHECK content result (0 same, 1 changed, 2 unreadable).
    void NoteListContent(int result, bool trusted) {
        (result == 0 ? run_same : result == 1 ? run_changed : run_unreadable) += 1;
        if (trusted) {
            ++run_would;
            run_would_differ += result != 0 ? 1 : 0;
        }
    }

    void MaybeLog() {
        if (!C13FrontStatsOn()) {
            return;
        }
        const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
        if (window == 0) {
            window = now;
            return;
        }
        if (now - window < 10'000'000'000ULL) {
            return;
        }
        const size_t learned = C13WriterLearn::Get().Learned();
        const auto ms = [](u64 ns) { return static_cast<double>(ns) / 1e6; };
        const auto avg = [](u64 sum, u64 n) {
            return n != 0 ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
        };
        std::vector<std::pair<std::pair<bool, u64>, Program>> top(programs.begin(),
                                                                 programs.end());
        const size_t count = std::min<size_t>(top.size(), 8);
        std::partial_sort(top.begin(), top.begin() + static_cast<std::ptrdiff_t>(count),
                          top.end(),
                          [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
        std::string text;
        for (size_t i = 0; i < count; ++i) {
            const auto& [key, p] = top[i];
            text += fmt::format(
                " [{}{:#x}: {} waits {:.1f} ms ({:.2f} ms each; seq {:.1f} ms, committer {:.1f} "
                "ms), writer->fetch {:.0f} us {:.0f} calls {:.0f} words, backlog seq {:.0f} / "
                "committer {:.0f} words; kicked {} ({:.1f} ms), not kicked {} ({:.1f} ms)]",
                key.first ? "hash " : "@", key.second, p.waits, ms(p.ns),
                avg(p.ns, p.waits) / 1e6, ms(p.seq_ns), ms(p.settle_ns),
                avg(p.since_ns, p.waits) / 1e3, avg(p.calls, p.waits), avg(p.words, p.waits),
                avg(p.backlog_seq, p.waits), avg(p.backlog_settle, p.waits), p.kicked,
                ms(p.kicked_ns), p.unkicked, ms(p.unkicked_ns));
        }
        const u64 content = run_same + run_changed + run_unreadable;
        const double same_pct =
            content != 0 ? 100.0 * static_cast<double>(run_same) / static_cast<double>(content)
                         : 0.0;
        LOG_INFO(HW_GPU,
                 "C13 front (10 s): compute list writer waits by program:{}{} | kick: {} kicked, "
                 "{} learned not kicked, {} restored, learned programs {} (+{}); learned waits "
                 "{}: kicked {} ({:.1f} ms, {:.3f} ms each), not kicked {} ({:.1f} ms, {:.3f} "
                 "ms each); check behind kick {} ({:.1f} ms); back: kick marks {} (run ends "
                 "{}), seq memo skips {} | L0 run: listwait check same {} ({:.1f} %), changed "
                 "{}, unreadable {}, would {} (differ {}){}; listrange gate {}",
                 text.empty() ? " -" : text,
                 waits_other != 0 ? fmt::format(" (+{} waits past the table)", waits_other)
                                  : std::string{},
                 kicks, kicks_off, kick_restored, learned, C13WriterLearn::Get().learned_new,
                 learned_waits, kicked_waits, ms(kicked_ns), avg(kicked_ns, kicked_waits) / 1e6,
                 unkicked_waits, ms(unkicked_ns), avg(unkicked_ns, unkicked_waits) / 1e6,
                 kick_behind, ms(kick_behind_ns),
                 g_c13_kick_marks.exchange(0, std::memory_order_relaxed),
                 g_c13_kick_run_ends.exchange(0, std::memory_order_relaxed),
                 g_c13_seq_skips.exchange(0, std::memory_order_relaxed), run_same, same_pct,
                 run_changed, run_unreadable, run_would, run_would_differ,
                 !C12ListWaitCheck()          ? " (check off)"
                 : content == 0               ? ""
                 : run_would_differ == 0 && same_pct > 90.0 ? " -> LISTWAIT eligible so far"
                                                            : " -> LISTWAIT not eligible",
                 C12ListRangeGate::Get().Format());
        C13FrontStats next{};
        next.run_same = run_same;
        next.run_changed = run_changed;
        next.run_unreadable = run_unreadable;
        next.run_would = run_would;
        next.run_would_differ = run_would_differ;
        next.window = now;
        *this = std::move(next);
        C13WriterLearn::Get().learned_new = 0;
    }
};

/// Start line (once, when the split starts with the MT back).
inline void C13FrontLogStart() {
    if (!Enabled()) {
        return;
    }
    const char kick = C13KickMode();
    LOG_INFO(HW_GPU,
             "Cycle 13 front: kick {} (mode {}, needs seqtarget {}, learn {}, check {}, seq "
             "skip {}, committer run end {}), stats {}, list check {} (listwait check {}, "
             "listrange gate {}, listwait {})",
             C13KickConfigured() ? (kick == 'a' ? "alt" : "on") : "off",
             kick == 'd' ? std::string{"default"} : std::string(1, kick), LiteSeqTarget(),
             C13KickLearn(), C13KickCheck(), C13KickSeq(), C13KickRun(), C13FrontStatsOn(),
             C13Shared::ListCheckEnv(), C12ListWaitCheck(), C12ListRangeGateOn(),
             C12ListWait());
}

} // namespace VideoCommon::MT
