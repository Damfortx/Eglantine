// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/mt/mt_switches.h"
#include "video_core/nx2.h"

/// Throughput switches of the multithreaded GPU back (docs/mt-perf-plan.md). Every one is inert
/// with EDEN_NX2_MT unset and ON by default under EDEN_NX2_MT=1; '0' restores the previous
/// behaviour for bisecting. Those marked "alternates" also accept 'a' (EDEN_NX2_ALT_SECONDS A/B
/// windows); the others are read once (their two sides must agree for a whole run).
namespace VideoCommon::MT {

namespace PerfDetail {
[[nodiscard]] inline bool Read(char mode) {
    return Enabled() && Tegra::Nx2Switch(mode, true);
}
} // namespace PerfDetail

/// EDEN_NX2_MT_LAZY_WAKE (P1, read once): SpmcStream::Commit publishes with a plain release store
/// and the Dekker check of sleeping consumers (seq_cst fence, waiters load, futex wake) runs only
/// at flush points (batch end, mark, Serial, waits, stream full, every 32 KiB of entries). =0:
/// every Commit checks and wakes, as before.
[[nodiscard]] inline bool LazyWake() {
    static const bool on = PerfDetail::Read(Tegra::Nx2EnvMode("EDEN_NX2_MT_LAZY_WAKE"));
    return on;
}

/// EDEN_NX2_MT_UBIND_FOLD (P2, read once): constant buffer binds travel only as the CbBind
/// pseudo-deltas the sequencer already emits; the committer applies them to its buffer cache
/// when it applies the CbBind entry. =0: one "uniform" Command per bind (~170k/s in MKW).
[[nodiscard]] inline bool UniformBindFold() {
    static const bool on = PerfDetail::Read(Tegra::Nx2EnvMode("EDEN_NX2_MT_UBIND_FOLD"));
    return on;
}

/// EDEN_NX2_MT_CMD_IN_BATCH (P3, alternates): Commands that only record (query, fence, barrier,
/// clear, conditional rendering) no longer end the open batch. =0: every Command but the
/// uniform binds ends it.
[[nodiscard]] inline bool CommandsInBatch() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_CMD_IN_BATCH");
    return PerfDetail::Read(mode);
}

/// EDEN_NX2_MT_BATCH_MIN=N (P4, default 4): the adaptive close (a committer waiting for the open
/// batch) waits until the batch holds N draws. 1 = close after any draw, as before.
[[nodiscard]] inline u32 AdaptiveBatchMin() {
    static const u32 count = EnvNumber("EDEN_NX2_MT_BATCH_MIN", 4);
    return count == 0 ? 1 : count;
}

/// EDEN_NX2_MT_CONT_MEMO (P5, alternates): the capture's IsContinuousRange answers for storage
/// buffers are kept per (address, size) while the GPU page table layout stays the same.
[[nodiscard]] inline bool ContinuousMemo() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_CONT_MEMO");
    return PerfDetail::Read(mode);
}

/// EDEN_NX2_MT_INVAL_TEXFILTER (P6, alternates): the sequencer's FlushCaching takes the texture
/// cache mutex only for written ranges that may hold an image (Nx2MayHoldTexture, the test I5's
/// BeforeWrite already relies on). =0: the mutex for every flush with ranges.
[[nodiscard]] inline bool InvalTextureFilter() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_INVAL_TEXFILTER");
    return PerfDetail::Read(mode);
}

/// EDEN_NX2_MT_INVAL_ONELOCK (P7, alternates): SeqInvalidateBuffers holds the tracker lock once
/// for all of a flush's ranges instead of twice per range.
[[nodiscard]] inline bool InvalOneLock() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_INVAL_ONELOCK");
    return PerfDetail::Read(mode);
}

/// EDEN_NX2_MT_EBR_LAZY (P8, alternates): the committer's epoch collection (per batch and every
/// 16 draws) skips both cache mutexes when nothing is retired.
[[nodiscard]] inline bool LazyEpochCollect() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_EBR_LAZY");
    return PerfDetail::Read(mode);
}

/// EDEN_NX2_MT_RALIAS (P9, alternates): a view whose image only has aliases (no other unclean
/// flag, not CPU-modified) resolves on the resolver; the committer runs SynchronizeAliases for it
/// before validation (a rescale it causes bumps the version and the draw takes the slow path).
/// =0: such views are view-not-clean.
[[nodiscard]] inline bool ResolverAliases() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_RALIAS");
    return PerfDetail::Read(mode);
}

/// EDEN_NX2_MT_DMA_CONSUMED (P10, alternates): an MME DMA read of pages the GPU has not written
/// (GpuSplit::WaitForWriters / DrainForRead) waits for the sequencer (`consumed`) and only for
/// the committer's guest memory writes queued before it (GpuSplit::NoteCommitterWrite), not for
/// `settled` of everything sent. =0: they wait for `settled` (full round trip).
[[nodiscard]] inline bool DmaReadConsumed() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_DMA_CONSUMED");
    return PerfDetail::Read(mode);
}

} // namespace VideoCommon::MT
