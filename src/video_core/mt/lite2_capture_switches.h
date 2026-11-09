// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/mt/lite_switches.h"

/// MT lite iteration 2, sequencer capture (docs/mt-lite-plan.md). Same rules as lite_switches.h:
/// on by default under EDEN_NX2_MT_LITE=1, '0' turns one off, '1' turns it on without LITE,
/// inert with EDEN_NX2_MT unset, read once.
namespace VideoCommon::MT {

/// EDEN_NX2_MT_LITE_SEQLOCK: the sequencer holds tracker_mutex less. A draw's or dispatch's
/// capture releases it before the overlay is built and copies the CPU-modified upload bytes
/// after releasing it (the tracker bits are taken under it as before), and FlushCaching's buffer
/// invalidation takes it once even for one range, skipping the GPU-data lookup on pages never
/// GPU-written.
[[nodiscard]] inline bool LiteSeqOneLock() {
    static const bool on = LiteDetail::Read("EDEN_NX2_MT_LITE_SEQLOCK");
    return on;
}

/// EDEN_NX2_MT_LITE_CAPMEMO: FinishOverlayCompact reuses the sorted order and the slot
/// eligibility of an earlier capture whose reads (addresses, sizes, kinds, offsets) were the
/// same, instead of sorting and scanning them again for every draw.
[[nodiscard]] inline bool LiteCaptureMemo() {
    static const bool on = LiteDetail::Read("EDEN_NX2_MT_LITE_CAPMEMO");
    return on;
}

/// EDEN_NX2_MT_LITE_XLATE: the sequencer's GPU -> device address translations at capture go
/// through a small per-page cache valid while the page table layout generation is unchanged.
[[nodiscard]] inline bool LiteXlateCache() {
    static const bool on = LiteDetail::Read("EDEN_NX2_MT_LITE_XLATE");
    return on;
}

} // namespace VideoCommon::MT
