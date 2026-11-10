// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/lite_switches.h"
#include "video_core/nx2.h"

/// Cycle 10, workstream B (docs/cycle10-plan.md): the front's waits on the back. On by default
/// under EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1; EDEN_NX2_C10_<X>=0 turns one off ('1' turns it on
/// without LITE; nothing without EDEN_NX2_MT). Read once. With a switch at '0' the code paths are
/// the ones before cycle 10 (no extra flushes, waits or notes).
namespace VideoCommon::MT {

/// EDEN_NX2_C10_ACQ_SEQ (B1): a semaphore GEQ acquire on a word with a release still noted as
/// pending whose value the front cannot use (PredictedRelease does not meet it) used to drain
/// the whole back (ACQUIRE_STATS "site 1 op 41 pending met": MKW's QMD plain-release words, met
/// by memory already in every case). Now the front waits only until the back ran the entries
/// that make the noted writes over that word (the last pending release / add / noted write, each
/// with the ring position by which the front had sent the method that triggers it) and the
/// copy / I2M / 2D writers sent so far (their semaphore releases and writes are not noted), and
/// for the committer only up to its guest writes queued before that position (as the LITE puller
/// drain does). Then memory decides: met -> passes, unmet -> the old drain path (unchanged, with
/// its suspension). Not for words on pages GPU work wrote (shader/copy writes are not noted).
[[nodiscard]] inline bool C10AcqSeq() {
    static const bool on = LiteDetail::Read("EDEN_NX2_C10_ACQ_SEQ");
    return on;
}

/// EDEN_NX2_C10_LIST_PRECISE (B2): the L24 writer wait before a command list on GPU-written pages
/// (GpuSplit::WaitForListWriters) waits for the committer only up to max(the list pages' last
/// captured writer, the committer's last queued BULK guest write, the copy/I2M/2D writers sent
/// (un-noted semaphore releases), the noted writes over the list's bytes) instead of every
/// committer guest write (semaphore/report words and fences, which the front notes with their
/// addresses: one over the list's bytes keeps the old bound). The sequencer part (every writer
/// sent) stays: compute write ranges are only known at capture.
[[nodiscard]] inline bool C10ListPrecise() {
    static const bool on = LiteDetail::Read("EDEN_NX2_C10_LIST_PRECISE");
    return on;
}

/// EDEN_NX2_C10_LIST_CHECK=1 (diagnostic, off; not part of LITE): a list fetch whose committer
/// wait B2 shortened is read again after the full writer wait (WaitForWriters) and compared;
/// differing reads count as stale and the second read is used. Logged every 10 s
/// ("C10 list check:").
[[nodiscard]] inline bool C10ListCheck() {
    static const bool on = Enabled() && Tegra::Nx2EnvMode("EDEN_NX2_C10_LIST_CHECK") == '1';
    return on;
}

/// Start line (once, when the split starts with the MT back).
inline void C10LogStart() {
    if (!Enabled()) {
        return;
    }
    LOG_INFO(HW_GPU, "Cycle 10 front: acqseq {}, listprecise {}, listcheck {}", C10AcqSeq(),
             C10ListPrecise(), C10ListCheck());
}

/// Front-thread counters for the "GPU split drains per second" line (EDEN_NX2_GPU_PROFILE).
struct C10FrontStats {
    // B1, per acquire that reached it (site 1: GEQ, release pending, not predicted).
    u64 acq_tries{};
    u64 acq_nowait{};     ///< Back already past the target: no wait at all, met
    u64 acq_short{};      ///< Waited, target before everything sent, met
    u64 acq_full{};       ///< Waited, the target was everything sent (no gain), met
    u64 acq_unmet{};      ///< Memory did not meet it after the wait: old drain path
    u64 acq_refused{};    ///< Page GPU-written, drain-known note, table overflow...: old path
    // B2, per L24 list wait.
    u64 list_calls{};
    u64 list_nowait{};    ///< No committer wait where the L24 bound would have waited
    u64 list_short{};     ///< Committer waited, but to an earlier position than L24's bound
    u64 list_kept{};      ///< A noted write over the list's bytes: L24's bound kept

    [[nodiscard]] std::string Format(double seconds) const {
        const auto rate = [seconds](u64 value) { return static_cast<double>(value) / seconds; };
        return fmt::format(
            " | c10 acq seq {:.0f}/s (no wait {:.0f}, shorter {:.0f}, full {:.0f}, unmet {:.0f}, "
            "refused {:.0f}) | c10 list settle {:.0f}/s (no wait {:.0f}, shorter {:.0f}, "
            "kept {:.0f})",
            rate(acq_tries), rate(acq_nowait), rate(acq_short), rate(acq_full), rate(acq_unmet),
            rate(acq_refused), rate(list_calls), rate(list_nowait), rate(list_short),
            rate(list_kept));
    }
};

} // namespace VideoCommon::MT
