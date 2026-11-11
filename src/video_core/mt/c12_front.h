// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/c11_front.h"
#include "video_core/mt/lite_switches.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"

/// Cycle 12, workstream W1 "front drains, second pass" (docs/cycle12-plan.md W1). Every piece
/// has its own switch: '0' = the cycle 11 path, '1' = on, 'a' = alternating (Nx2AltOn; only
/// where the switch is read per call). Nothing without EDEN_NX2_MT. Start line: "Cycle 12
/// front: ..." (C12LogStart, once, from GpuSplit::MaybeStart). Stats line every 10 s:
/// "C12 front per 10 s: ..." (EDEN_NX2_C12_STATS=1, GPU_PROFILE or a C12 check switch).
namespace VideoCommon::MT {

// ---------------------------------------------------------------------------------------------
// P1: adjacent-release overlay
// ---------------------------------------------------------------------------------------------

/// EDEN_NX2_C12_POSADJ (P1; on by default under EDEN_NX2_MT_LITE, '0' = cycle 11 path, 'a'
/// alternates per read): KnownPendingWords' range rule becomes per byte
/// (GpuSplit::C12KnownPendingWords). Every "past known words" refusal in the c11b race was
/// `macro 0x0 +0, 24 bytes over a 16-byte direct release`: the MME reads the 16-byte report and
/// the next 8 bytes. The cycle 11 rule refused any read reaching past a direct release's known
/// words, even past its END, where the release writes nothing. Now:
///  - a direct 16-byte payload release lays all of its 16 bytes: words 0-1 (payload, 0) and
///    words 2-3 = the front's GPU().GetTicks() at the read (the sequencer writes its own, later,
///    GetTicks(): the timestamp is earlier by the backlog, as POSFWD's);
///  - bytes of the read past that release are each other noted write's business (its own known
///    words, laid in note order: the neighbouring release of a 16-byte report sequence lays its
///    payload words) or memory's, when no queued write the front noted covers them (the same
///    rule as the plain no-wait read: no table overflow, no writer/drain-known note, no
///    GPU-written page: FrontDmaRead checks those first and they still refuse);
///  - a wider write with only its first word known still refuses a read of its other bytes.
/// Stats: the writer of the bytes past the release (queued note with tag / direct / channel /
/// adjacency, settled note, unnoted).
[[nodiscard]] inline bool C12PosAdj() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_C12_POSADJ");
    return C11Detail::LiteCall(mode);
}

/// EDEN_NX2_C12_POSADJ_CHECK=1 (diagnostic): a read P1 let through where the cycle 11 rule would
/// have refused it also waits for its writes (position wait, committer included) and compares
/// every word. Mismatches are split into timestamp words (words 2-3 of a direct release:
/// expected to differ, the front's clock is earlier) and every other word ("payload", must be
/// 0: rate-limited ERROR "C12 posadj check"). The waited words are used (old behaviour).
[[nodiscard]] inline bool C12PosAdjCheck() {
    static const bool on = Enabled() && Tegra::Nx2EnvMode("EDEN_NX2_C12_POSADJ_CHECK") == '1';
    return on;
}

// ---------------------------------------------------------------------------------------------
// P2: LISTRANGE enablement gate
// ---------------------------------------------------------------------------------------------

/// EDEN_NX2_C12_LISTRANGE (P2; off by default: '1' = the gate): turns cycle 11 F2 LISTRANGE on
/// (C11ListRange, which reads this variable too) and puts it on probation: every F2-shortened
/// fetch is checked as with C11_LISTRANGE_CHECK=1 (read again after every writer, the second read
/// used) until EDEN_NX2_C12_LISTRANGE_CALLS (default 20000) list calls were seen. Then the plan's
/// gate decides in the run: "shorter" > EDEN_NX2_C12_LISTRANGE_PCT (default 30) % of the list
/// calls and 0 stale -> stays on without the check; else off (the cycle 11 sequencer target;
/// F2's bookkeeping keeps running, it is static). A stale read at any time (probation, or later
/// with C11_LISTRANGE_CHECK=1) turns it off at once. One INFO line per decision.
[[nodiscard]] inline bool C12ListRangeGateOn() {
    return C12Shared::ListRangeGateEnv();
}

/// Front-thread state of the P2 gate.
class C12ListRangeGate {
public:
    enum class State : u8 { Probation, On, Off };

    static C12ListRangeGate& Get() {
        static C12ListRangeGate instance;
        return instance;
    }

    /// Whether F2's shortened sequencer target may be used (true without the gate: the cycle 11
    /// switch alone decides).
    [[nodiscard]] bool Use() const {
        return !C12ListRangeGateOn() || state != State::Off;
    }
    /// Whether the gate wants every shortened fetch checked now.
    [[nodiscard]] bool Probation() const {
        return C12ListRangeGateOn() && state == State::Probation;
    }
    [[nodiscard]] State Current() const {
        return state;
    }
    /// WaitForListWriters: one list call with F2 (shorter: its target fell below every writer).
    void NoteCall(bool shorter) {
        if (!C12ListRangeGateOn()) {
            return;
        }
        ++calls;
        shorter_calls += shorter ? 1 : 0;
        if (state == State::Probation && calls >= CallsNeeded()) {
            const bool pass = stale == 0 && shorter_calls * 100 > calls * Percent();
            Decide(pass ? State::On : State::Off, pass ? "passed" : "shorter too rare");
        }
    }
    /// DmaPusher: an F2-shortened fetch was read again after every writer.
    void NoteCheck(bool was_stale) {
        if (!C12ListRangeGateOn()) {
            return;
        }
        ++compared;
        if (was_stale) {
            ++stale;
            if (state != State::Off) {
                Decide(State::Off, "stale read");
            }
        }
    }
    [[nodiscard]] std::string Format() const {
        if (!C12ListRangeGateOn()) {
            return "off";
        }
        return fmt::format("{} (calls {}, shorter {}, compared {}, stale {})",
                           state == State::Probation ? "probation"
                           : state == State::On      ? "on"
                                                     : "off",
                           calls, shorter_calls, compared, stale);
    }

private:
    static u64 CallsNeeded() {
        static const u64 value = [] {
            const char* text = std::getenv("EDEN_NX2_C12_LISTRANGE_CALLS");
            const long long parsed = text != nullptr ? std::atoll(text) : 0;
            return parsed > 0 ? static_cast<u64>(parsed) : u64{20000};
        }();
        return value;
    }
    static u64 Percent() {
        static const u64 value = [] {
            const char* text = std::getenv("EDEN_NX2_C12_LISTRANGE_PCT");
            const long long parsed = text != nullptr ? std::atoll(text) : -1;
            return parsed >= 0 && parsed <= 100 ? static_cast<u64>(parsed) : u64{30};
        }();
        return value;
    }
    void Decide(State next, const char* why) {
        state = next;
        C12Shared::listrange_probation_over = true; // C11ListRangeCheck back to its switch
        LOG_INFO(HW_GPU,
                 "C12 listrange gate: {} ({}): {} list calls, {} shorter ({:.1f} %, needs > {} "
                 "%), {} checked, {} stale",
                 next == State::On ? "on" : "off", why, calls, shorter_calls,
                 calls != 0 ? 100.0 * static_cast<double>(shorter_calls) /
                                  static_cast<double>(calls)
                            : 0.0,
                 Percent(), compared, stale);
    }
    State state{State::Probation};
    u64 calls{};
    u64 shorter_calls{};
    u64 compared{};
    u64 stale{};
};

// ---------------------------------------------------------------------------------------------
// P3: list writer census and same-content speculation
// ---------------------------------------------------------------------------------------------

/// EDEN_NX2_C12_LISTWAIT (P3 fix; contingent, OFF by default: '1' on). A GPU-written command
/// list whose writer census found the content unchanged by its writer
/// EDEN_NX2_C12_LISTWAIT_TRUST (default 64) times in a row (same address and size; any change
/// seen bans it for the run) is read without the writer wait: the front parses the current
/// bytes, and the read is verified later on the front (C12 verification: once the back has run
/// the writers that were queued at the read, the list is read again and hashed). A mismatch is
/// counted, logged as ERROR "C12 listwait verify" and bans the list, but it is NOT undone: the
/// methods were already sent (the plan's rewind to the list start is only legal before the first
/// method reaches the ring, which every list does at once here: FlushMacroList publishes at
/// publish_every). So this stays off until LISTWAIT_CHECK shows 0 "would differ" over whole
/// races. LiteListCheck (EDEN_NX2_MT_LITE_LISTCHECK) also re-reads speculated lists.
[[nodiscard]] inline bool C12ListWait() {
    static const bool on = Enabled() && Tegra::Nx2EnvMode("EDEN_NX2_C12_LISTWAIT") == '1';
    return on;
}

/// EDEN_NX2_C12_LISTWAIT_CHECK=1 (census content check, diagnostic): every list fetch that would
/// wait for its writers is also read BEFORE the wait; after the wait both reads are compared
/// ("same" / "changed" / "unreadable" per last writer kind). With LISTWAIT on, trusted lists
/// still wait and count "would differ" instead of speculating.
/// Cycle 13 L0: also on, unset, with EDEN_NX2_C13_LIST_CHECK=1 (mt/c13_front.h).
[[nodiscard]] inline bool C12ListWaitCheck() {
    static const bool on = [] {
        const char mode = Tegra::Nx2EnvMode("EDEN_NX2_C12_LISTWAIT_CHECK");
        return Enabled() && (mode == '1' || (mode == 'd' && C13Shared::ListCheckEnv()));
    }();
    return on;
}

[[nodiscard]] inline u32 C12ListWaitTrust() {
    static const u32 value = [] {
        const char* text = std::getenv("EDEN_NX2_C12_LISTWAIT_TRUST");
        const long long parsed = text != nullptr ? std::atoll(text) : 0;
        return parsed > 0 ? static_cast<u32>(std::min<long long>(parsed, 1'000'000)) : u32{64};
    }();
    return value;
}

/// EDEN_NX2_C12_STATS=1 (also on with EDEN_NX2_GPU_PROFILE or a C12 check switch): the "C12
/// front per 10 s" line, including the P3 writer census (kind, compute program, wait time).
[[nodiscard]] inline bool C12Stats() {
    static const bool on = Enabled() && (std::getenv("EDEN_NX2_C12_STATS") != nullptr ||
                                         Nx2Profile::Enabled() || C12PosAdjCheck() ||
                                         C12ListWaitCheck() || C12ListWait() ||
                                         C12ListRangeGateOn());
    return on;
}

/// The P3 census bookkeeping (program of the last compute writer) runs.
[[nodiscard]] inline bool C12ListCensus() {
    return C12Stats();
}
/// The content pre-read before a list writer wait runs.
[[nodiscard]] inline bool C12ListPreRead() {
    static const bool on = C12ListWait() || C12ListWaitCheck();
    return on;
}

/// Per DmaPusher: the lists the census found unchanged by their writers, and the one
/// speculative read waiting for its verification. Front thread only.
struct C12ListWaitState {
    struct Trust {
        GPUVAddr address{};
        u32 count{};
        u32 same_run{};  ///< Consecutive "same" census results
        bool banned{};
    };
    std::unordered_map<u64, Trust> trust;

    struct Pending {
        bool active{};
        GPUVAddr address{};
        u32 count{};
        u64 hash{};
        u64 target{};    ///< WritersTarget at the read: verified once the back is past it
    };
    Pending pending;

    [[nodiscard]] static u64 Key(GPUVAddr address, u32 count) {
        return (address * 0x9E3779B97F4A7C15ULL) ^ (u64{count} << 1) ^ (u64{count} << 40);
    }
    Trust* Find(GPUVAddr address, u32 count) {
        const auto it = trust.find(Key(address, count));
        if (it == trust.end() || it->second.address != address || it->second.count != count) {
            return nullptr;
        }
        return &it->second;
    }
    [[nodiscard]] bool Trusted(GPUVAddr address, u32 count) {
        const Trust* entry = Find(address, count);
        return entry != nullptr && !entry->banned && entry->same_run >= C12ListWaitTrust();
    }
    /// Census result of a checked fetch: returns whether the entry became trusted now.
    bool NoteCensus(GPUVAddr address, u32 count, bool same) {
        if (trust.size() >= 8192) {
            // Keep the bans (cheap) and forget the rest.
            std::erase_if(trust, [](const auto& item) { return !item.second.banned; });
        }
        const u64 key = Key(address, count);
        auto [it, inserted] = trust.try_emplace(key, Trust{address, count, 0, false});
        Trust& entry = it->second;
        if (!inserted && (entry.address != address || entry.count != count)) {
            entry = Trust{address, count, 0, false}; // Key collision: the newer list wins
        }
        if (entry.banned) {
            return false;
        }
        if (!same) {
            entry.banned = true;
            entry.same_run = 0;
            return false;
        }
        ++entry.same_run;
        return entry.same_run == C12ListWaitTrust();
    }
    void Ban(GPUVAddr address, u32 count) {
        const u64 key = Key(address, count);
        trust[key] = Trust{address, count, 0, true};
    }

    [[nodiscard]] static u64 Hash(const void* data, size_t bytes) {
        const auto* p = static_cast<const u8*>(data);
        u64 hash = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < bytes; ++i) {
            hash = (hash ^ p[i]) * 0x100000001b3ULL;
        }
        return hash;
    }
};

// ---------------------------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------------------------

/// Front-thread counters ("C12 front per 10 s").
struct C12FrontStats {
    static C12FrontStats& Get() {
        static C12FrontStats instance;
        return instance;
    }
    // P1
    u64 adj_reads{};        ///< Overlay reads through the per-byte rule
    u64 adj_served{};       ///< ... that the cycle 11 rule would have refused (a wait saved)
    u64 adj_refused_value{};///< Per-byte rule still refused: a write without a known value
    u64 adj_refused_past{}; ///< ... a wider write whose other bytes the read covers
    /// Writer of the bytes past the release, for reads the cycle 11 rule refuses as "past known":
    /// class (1 queued note, 2 settled note, 3 unnoted), tag, direct, same channel, adjacent.
    std::map<std::tuple<u8, u8, bool, bool, bool>, u64> adj_past;
    u64 check_reads{};
    u64 check_payload_mismatch{};
    u64 check_ts_mismatch{};
    u64 check_payload_total{}; ///< Whole run (ERROR rate limit)
    // P3 census: waited GPU-written list fetches by last writer kind (engine type, 6 none)
    std::array<u64, 7> wait_kind{};
    std::array<u64, 7> wait_kind_ns{};
    std::array<u64, 7> same_kind{};
    std::array<u64, 7> changed_kind{};
    std::array<u64, 7> unreadable_kind{};
    std::map<u64, std::pair<u64, u64>> wait_program; ///< compute program -> waits, ns
    u64 spec_reads{};          ///< LISTWAIT: trusted lists read without the wait
    u64 spec_would{};          ///< LISTWAIT_CHECK: trusted lists that would have been
    u64 spec_would_differ{};   ///< ... and differed after the wait (must be 0)
    u64 spec_verified{};
    u64 spec_mismatch{};       ///< Verification differed with no later writer: a wrong read
    u64 spec_inconclusive{};   ///< Differed or unreadable with a later writer: banned only
    u64 spec_mismatch_total{};
    u64 trusted_new{};
    u64 window{};

    static constexpr std::array<const char*, 7> KindNames{"timer", "compute", "3d", "2d",
                                                          "dma", "memory", "none"};

    void NoteWait(u8 kind, u64 program, u64 ns) {
        const size_t k = std::min<size_t>(kind, 6);
        ++wait_kind[k];
        wait_kind_ns[k] += ns;
        if (k == 1 && (wait_program.size() < 64 || wait_program.contains(program))) {
            auto& entry = wait_program[program];
            ++entry.first;
            entry.second += ns;
        }
    }
    void NoteContent(u8 kind, int result) { // 0 same, 1 changed, 2 unreadable
        const size_t k = std::min<size_t>(kind, 6);
        (result == 0 ? same_kind : result == 1 ? changed_kind : unreadable_kind)[k] += 1;
    }
    void NotePayloadMismatch(u64 address, u32 word, u32 front, u32 memory) {
        ++check_payload_mismatch;
        const u64 total = check_payload_total++;
        if (total < 16 || (total % 1000) == 0) {
            LOG_ERROR(HW_GPU,
                      "C12 posadj check: {:#x} word {} front {:#x}, after the wait {:#x} ({} so "
                      "far)",
                      address, word, front, memory, total + 1);
        }
    }
    void NoteSpecMismatch(u64 address, u32 count) {
        ++spec_mismatch;
        const u64 total = spec_mismatch_total++;
        if (total < 16 || (total % 1000) == 0) {
            LOG_ERROR(HW_GPU,
                      "C12 listwait verify: speculated list {:#x} ({} words) changed by its "
                      "writer ({} so far)",
                      address, count, total + 1);
        }
    }

    void MaybeLog() {
        if (!C12Stats()) {
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
        static constexpr std::array<const char*, 4> classes{"-", "queued note", "settled note",
                                                            "unnoted"};
        std::string past;
        u32 shown = 0;
        for (const auto& [key, count] : adj_past) {
            if (shown++ == 8) {
                break;
            }
            const u8 cls = std::min<u8>(std::get<0>(key), 3);
            if (cls == 1 || cls == 2) {
                past += fmt::format(" [{} tag {}{}{}{}]: {}", classes[cls], std::get<1>(key),
                                    std::get<2>(key) ? " direct" : "",
                                    std::get<3>(key) ? " same channel" : " other channel",
                                    std::get<4>(key) ? " adjacent" : "", count);
            } else {
                past += fmt::format(" [{}]: {}", classes[cls], count);
            }
        }
        std::string kinds;
        for (size_t k = 0; k < wait_kind.size(); ++k) {
            if (wait_kind[k] == 0 && same_kind[k] == 0 && changed_kind[k] == 0 &&
                unreadable_kind[k] == 0) {
                continue;
            }
            kinds += fmt::format(" {} {} ({:.1f} ms; same {}, changed {}, unreadable {})",
                                 KindNames[k], wait_kind[k],
                                 static_cast<double>(wait_kind_ns[k]) / 1e6, same_kind[k],
                                 changed_kind[k], unreadable_kind[k]);
        }
        std::vector<std::pair<u64, std::pair<u64, u64>>> top(wait_program.begin(),
                                                             wait_program.end());
        const size_t count = std::min<size_t>(top.size(), 6);
        std::partial_sort(top.begin(), top.begin() + count, top.end(),
                          [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        std::string programs;
        for (size_t i = 0; i < count; ++i) {
            programs += fmt::format(" {:#x} {} ({:.1f} ms)", top[i].first, top[i].second.first,
                                    static_cast<double>(top[i].second.second) / 1e6);
        }
        LOG_INFO(HW_GPU,
                 "C12 front per 10 s: posadj reads {}, served {} (refused: no value {}, past "
                 "known {}), past bytes:{} | posadj check {} reads, payload mismatch {}, "
                 "timestamp mismatch {} | listrange gate {} | list writer waits by kind:{} | "
                 "compute programs:{} | listwait speculated {}, would {} (differ {}), verified "
                 "{}, mismatch {}, inconclusive {}, newly trusted {}",
                 adj_reads, adj_served, adj_refused_value, adj_refused_past,
                 past.empty() ? " -" : past, check_reads, check_payload_mismatch,
                 check_ts_mismatch, C12ListRangeGate::Get().Format(),
                 kinds.empty() ? " -" : kinds, programs.empty() ? " -" : programs, spec_reads,
                 spec_would, spec_would_differ, spec_verified, spec_mismatch, spec_inconclusive,
                 trusted_new);
        const u64 keep_payload_total = check_payload_total;
        const u64 keep_spec_total = spec_mismatch_total;
        *this = C12FrontStats{};
        check_payload_total = keep_payload_total;
        spec_mismatch_total = keep_spec_total;
        window = now;
    }
};

/// Start line (once, when the split starts with the MT back).
inline void C12LogStart() {
    if (!Enabled()) {
        return;
    }
    const auto mode = [](const char* name, bool default_on) {
        const char m = Tegra::Nx2EnvMode(name);
        return m == 'a' ? std::string{"alt"} : Tegra::Nx2Switch(m, default_on) ? "on" : "off";
    };
    LOG_INFO(HW_GPU,
             "Cycle 12 front: posadj {} (check {}), listrange gate {} (calls {}, needs listrange "
             "{} listprecise {}), listwait {} (check {}, trust {}), stats {}",
             mode("EDEN_NX2_C12_POSADJ", Lite()), C12PosAdjCheck(), C12ListRangeGateOn(),
             std::getenv("EDEN_NX2_C12_LISTRANGE_CALLS") != nullptr
                 ? std::getenv("EDEN_NX2_C12_LISTRANGE_CALLS")
                 : "20000",
             C11ListRange(), C10ListPrecise(), C12ListWait(), C12ListWaitCheck(),
             C12ListWaitTrust(), C12Stats());
}

} // namespace VideoCommon::MT
