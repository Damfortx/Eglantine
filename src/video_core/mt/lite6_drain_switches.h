// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/mt/lite3_front_switches.h"
#include "video_core/mt/lite_switches.h"
#include "video_core/nx2_profile.h"

/// MT lite round 6, W3 (docs/mt-lite-plan6.md): the front's remaining drains (position waits
/// before MME DMA reads, puller acquire drains). On by default under EDEN_NX2_MT_LITE=1 (with
/// EDEN_NX2_MT=1); EDEN_NX2_MT_LITE_<X>=0 turns one off, '1' turns it on without LITE (the front
/// pieces work on the plain split too; SEQPOS only changes anything with the MT back). Read once.
namespace VideoCommon::MT {

/// EDEN_NX2_MT_LITE_REPORTSIZE (D1): a 3D report with short_query set (no timestamp: the query
/// cache writes one 32-bit word, fence or not) is noted as 4 bytes, not 16: MME DMA reads of the
/// words packed next to it no longer wait for it (as EDEN_NX2_RELEASE_SIZE did for the puller's
/// 4-byte releases), and a short payload release's value is overlay-able without OVERLAY.
[[nodiscard]] inline bool Lite6ReportSize() {
    static const bool on = Lite3Detail::Read("EDEN_NX2_MT_LITE_REPORTSIZE");
    return on;
}

/// EDEN_NX2_MT_LITE_OVERLAY64 (D2): 16-byte payload releases (puller SEMAPHORE WriteLong without
/// RELEASE_SIZE, 3D report Release of Payload without short_query or reduction) write the payload
/// as a 64-bit value (word 1 = 0) and the timestamp after it, on the sequencer
/// (RasterizerVulkan::Query / QueryCacheBase::CounterReport direct path; not with GPU accuracy
/// high): the overlay knows words 0 and 1, so reads within the first 8 bytes need no wait.
[[nodiscard]] inline bool Lite6Overlay64() {
    static const bool on = Lite3Detail::Read("EDEN_NX2_MT_LITE_OVERLAY64");
    return on;
}

/// EDEN_NX2_MT_LITE_SEQPOS (D3): a position wait waits for the sequencer (`consumed`) past every
/// noted write over the read, and for the committer (`settled`) only past the ones it makes
/// (fence releases, counter reports, I2M): the direct 16-byte payload releases of D2 are in memory
/// once the sequencer ran their entry. Not when writer-known writes (QMD releases) overlap.
[[nodiscard]] inline bool Lite6SeqPosition() {
    static const bool on = Lite3Detail::Read("EDEN_NX2_MT_LITE_SEQPOS");
    return on;
}

/// EDEN_NX2_MT_LITE_I2MPOS (D4): correctness. A 3D inline upload (launch_dma + inline_data)
/// writes its destination as the back runs its data words, but the note carried the position of
/// launch_dma (a position wait could end before the data was written), multi-word data runs were
/// not noted at all, and a drain between the launch and the data dropped the note. Now the note
/// stays open (GpuSplit::OpenI2mWrite): every data entry sent (single inline_data writes and
/// FrontCallMultiMethod runs) moves its position past it (NoteI2mData), drains and table
/// compaction keep it, and the next launch_dma closes it into the table.
[[nodiscard]] inline bool Lite6I2mPosition() {
    static const bool on = Lite3Detail::Read("EDEN_NX2_MT_LITE_I2MPOS");
    return on;
}

/// EDEN_NX2_MT_LITE_PENDACQ (D5): an unpredicted semaphore acquire (trigger) on a word whose
/// still-queued noted writes all have known values there, none of which meets it (no adds, no
/// writer/drain-known or valueless note, no open I2M, not a QMD/DMA release target, page never
/// GPU-written), and which memory does not meet now, suspends without the drain: after the drain
/// the word would hold one of those values or what memory holds, so only an un-noted write could
/// meet it (latency, never order; the latch, registered first, sees no noted value meet it). The
/// waiter latch is registered and fences sent, as FOREIGNACQ.
[[nodiscard]] inline bool Lite6PendingAcquire() {
    static const bool on = Lite3Detail::Read("EDEN_NX2_MT_LITE_PENDACQ");
    return on;
}

/// EDEN_NX2_MT_LITE_PENDPOLL (D6): resume polls of acquires with a release still pending (they
/// drained every poll) where D5's test holds: the latch every poll, a drain every 8th.
[[nodiscard]] inline bool Lite6PendingPoll() {
    static const bool on = Lite3Detail::Read("EDEN_NX2_MT_LITE_PENDPOLL");
    return on;
}

/// EDEN_NX2_MT_LITE_PENDCHECK=1 (off, diagnostic): every D5/D6 skip drains anyway and counts the
/// ones the drain met (PULLER_STATS "pendacq would meet": the latency D5/D6 add); the drained
/// answer is used.
[[nodiscard]] inline bool Lite6PendingCheck() {
    static const bool on = Tegra::Nx2EnvMode("EDEN_NX2_MT_LITE_PENDCHECK") == '1';
    return on;
}

/// Whether payload releases that are not fences are written with their payload as a 64-bit value
/// on the sequencer (D2/D3): RasterizerVulkan::Query / QueryCacheBase::CounterReport take the
/// direct path unless GPU accuracy is high.
[[nodiscard]] inline bool Lite6DirectPayloads() {
    return !Settings::IsGPULevelHigh();
}

/// Statistics (EDEN_NX2_GPU_PROFILE or EDEN_NX2_PULLER_STATS).
[[nodiscard]] inline bool Lite6Stats() {
    static const bool on = Nx2Profile::Enabled() || std::getenv("EDEN_NX2_PULLER_STATS") != nullptr;
    return on;
}

inline void Lite6LogOnce() {
    static const bool logged = [] {
        LOG_INFO(HW_GPU,
                 "MT lite 6: reportsize {}, overlay64 {}, seqpos {}, i2mpos {}, pendacq {}, "
                 "pendpoll {}, pendcheck {}",
                 Lite6ReportSize(), Lite6Overlay64(), Lite6SeqPosition(), Lite6I2mPosition(),
                 Lite6PendingAcquire(), Lite6PendingPoll(), Lite6PendingCheck());
        return true;
    }();
    (void)logged;
}

/// Tags of noted writes (GpuSplit::NotePendingWrite `tag`; EDEN_NX2_DMA_READ_WHY and the stats).
enum Lite6WriteTag : u8 {
    TagOther = 0,
    TagI2m = 1,
    TagCounterReport = 2, ///< 3D report of a counter (or with reduction): value from the host GPU
    TagPayloadReport = 3, ///< 3D report of its payload register
    TagPullerRelease = 4, ///< puller semaphore release (SEMAPHORE_TRIGGER / SEM_EXECUTE)
    TagQmd = 5,           ///< compute: QMD release (Writers-known)
    TagCount = 6,
};

/// Front thread: MME DMA read position waits by the kind of the newest noted write they wait
/// for, logged every 10 s with the stats on.
struct Lite6PositionStats {
    static Lite6PositionStats& Get() {
        static Lite6PositionStats instance;
        return instance;
    }
    /// payload-seq: direct 16-byte payload release (sequencer); fence: other payload releases
    /// (puller fence releases, short 3D payload reports: committer fence callbacks).
    enum Kind : u32 { PayloadSeq, Fence, Counter, I2m, Unknown, KindCount };
    static Kind KindOf(u8 tag, bool direct) {
        if (direct) {
            return PayloadSeq;
        }
        switch (tag) {
        case TagPayloadReport:
        case TagPullerRelease:
            return Fence;
        case TagCounterReport:
            return Counter;
        case TagI2m:
            return I2m;
        default:
            return Unknown;
        }
    }
    std::array<u64, KindCount> calls{}, waited{}, ns{};
    u64 seq_only{}, seq_only_waited{}, seq_only_ns{};
    u64 settle{}, settle_waited{}, settle_ns{};
    u64 refused_no_value{}, refused_past_known{}, open_i2m{};
    u64 writers_only{}, overlay_reads{}, report4{};
    u64 window{};

    void Note(Kind kind, bool sequencer_only, bool did_wait, u64 wait_ns) {
        ++calls[kind];
        waited[kind] += did_wait ? 1 : 0;
        ns[kind] += wait_ns;
        if (sequencer_only) {
            ++seq_only;
            seq_only_waited += did_wait ? 1 : 0;
            seq_only_ns += wait_ns;
        } else {
            ++settle;
            settle_waited += did_wait ? 1 : 0;
            settle_ns += wait_ns;
        }
    }
    void MaybeLog() {
        const u64 now = Nx2Profile::Now();
        if (window == 0) {
            window = now;
            return;
        }
        if (now - window < 10'000'000'000ULL) {
            return;
        }
        const double seconds = static_cast<double>(now - window) / 1e9;
        static constexpr std::array<const char*, KindCount> names{"payload-seq", "fence",
                                                                  "counter", "i2m", "unknown"};
        std::string line;
        for (u32 i = 0; i < KindCount; ++i) {
            line += fmt::format(" | {} {:.0f}/s ({:.0f} waited, {:.1f} ms)", names[i],
                                static_cast<double>(calls[i]) / seconds,
                                static_cast<double>(waited[i]) / seconds,
                                static_cast<double>(ns[i]) / 1e6 / seconds);
        }
        line += fmt::format(
            " | sequencer-only {:.0f}/s ({:.0f} waited, {:.1f} ms) | settle {:.0f}/s ({:.0f} "
            "waited, {:.1f} ms) | overlay reads {:.0f}/s, refused: no value {:.0f}/s, past known "
            "words {:.0f}/s | open i2m {:.0f}/s | writers only {:.0f}/s | 4-byte reports noted "
            "{:.0f}/s",
            static_cast<double>(seq_only) / seconds, static_cast<double>(seq_only_waited) / seconds,
            static_cast<double>(seq_only_ns) / 1e6 / seconds, static_cast<double>(settle) / seconds,
            static_cast<double>(settle_waited) / seconds,
            static_cast<double>(settle_ns) / 1e6 / seconds,
            static_cast<double>(overlay_reads) / seconds,
            static_cast<double>(refused_no_value) / seconds,
            static_cast<double>(refused_past_known) / seconds,
            static_cast<double>(open_i2m) / seconds, static_cast<double>(writers_only) / seconds,
            static_cast<double>(report4) / seconds);
        LOG_INFO(HW_GPU, "MT lite 6 position waits per second (newest noted write):{}", line);
        *this = Lite6PositionStats{};
        window = now;
    }
};

} // namespace VideoCommon::MT
