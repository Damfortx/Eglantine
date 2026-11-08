// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>

#include "common/common_types.h"
#include "video_core/mt/mt_switches.h"

/// Blocking waits of the multithreaded back, as a watchdog sees them (EDEN_NX2_MT_STATS or
/// EDEN_NX2_MT_WATCHDOG=1). A thread that is about to sleep notes the wait in its slot; the
/// pipeline's watchdog thread reports any wait older than 2 s with the counters that say who
/// should have woken it, then kicks every waiter (each wait re-checks its condition in a loop,
/// so a kick never ends a wait early: it only recovers a lost wake-up, which it reports).
namespace VideoCommon::MT {

enum class WaitSite : u32 {
    SeqWaitCommitted, ///< sequencer: stream consumed by the committer (lockstep, barriers)
    SeqSerial,        ///< sequencer: a Serial's completion
    SeqBarrierAcks,   ///< sequencer: resolvers' channel barrier acks
    SeqBackPressure,  ///< sequencer: resolved slot ring room (committed_seq)
    SeqStreamSpace,   ///< sequencer: SPMC ring space (slowest consumer)
    CommitResolved,   ///< committer: a resolver's batch
    ResolverBarrier0, ///< resolver 0..7: the channel Serial's completion
    Count = ResolverBarrier0 + 8,
};

inline constexpr std::array<const char*, 6> WaitSiteNames{
    "sequencer wait-committed", "sequencer serial",       "sequencer barrier acks",
    "sequencer back-pressure",  "sequencer stream space", "committer wait-resolved",
};

[[nodiscard]] inline const char* WaitSiteName(u32 site) {
    return site < WaitSiteNames.size() ? WaitSiteNames[site] : "resolver channel barrier";
}

/// EDEN_NX2_MT_WATCHDOG=1|0 (default: on with EDEN_NX2_MT_STATS).
[[nodiscard]] inline bool WatchdogEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_MT_WATCHDOG");
        if (value != nullptr && value[0] != '\0') {
            return value[0] == '1';
        }
        const char* stats = std::getenv("EDEN_NX2_MT_STATS");
        return stats != nullptr && stats[0] == '1';
    }();
    return enabled;
}

struct WaitSlot {
    /// steady_clock ns when the wait began, 0 while not waiting.
    alignas(64) std::atomic<u64> since{0};
    /// What the waiter waits for (a position, seq or batch), for the report.
    std::atomic<u64> target{0};
    /// Episodes reported (one report per wait).
    std::atomic<u64> reported_since{0};
};

class WaitWatch {
public:
    [[nodiscard]] static u64 NowNs() noexcept {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
    }

    void Begin(WaitSite site, u64 target) noexcept {
        if (!on) {
            return;
        }
        WaitSlot& slot = slots[static_cast<u32>(site)];
        slot.target.store(target, std::memory_order_relaxed);
        slot.since.store(NowNs(), std::memory_order_release);
    }
    void End(WaitSite site) noexcept {
        if (!on) {
            return;
        }
        WaitSlot& slot = slots[static_cast<u32>(site)];
        const u64 since = slot.since.exchange(0, std::memory_order_acq_rel);
        if (since != 0 && slot.reported_since.load(std::memory_order_relaxed) == since) {
            recovered.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /// RAII for a wait's slow path.
    class Scope {
    public:
        Scope(WaitWatch& watch_, WaitSite site_, u64 target) : watch{watch_}, site{site_} {
            watch.Begin(site, target);
        }
        ~Scope() {
            watch.End(site);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        WaitWatch& watch;
        WaitSite site;
    };

    const bool on = WatchdogEnabled();
    std::array<WaitSlot, static_cast<size_t>(WaitSite::Count)> slots{};
    /// Waits that ended after the watchdog reported (and kicked) them.
    std::atomic<u64> recovered{0};
};

} // namespace VideoCommon::MT
