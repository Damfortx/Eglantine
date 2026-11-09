// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/mt/lite_switches.h"

/// MT lite iteration 2, committer side of tracker_mutex (docs/mt-lite-plan.md). Same rules as
/// mt/lite_switches.h: default on under EDEN_NX2_MT_LITE=1, '0' turns it off, '1' turns it on
/// without LITE, inert with EDEN_NX2_MT unset, read once.
namespace VideoCommon::MT {

/// EDEN_NX2_MT_LITE_CTRACK: the committer recording a packet takes tracker_mutex once per bind
/// pass instead of once per written buffer. MarkWrittenBuffer's tracker half (GPU-modified marks,
/// fence downloads, read traps) is queued while a BufferCache::MtWrittenBatch is open and applied
/// under one hold when the outermost one closes; the vertex stale ranges of CPU write traps are
/// swapped out under the lock and applied after it.
[[nodiscard]] inline bool LiteCommitTracker() {
    static const bool on = LiteDetail::Read("EDEN_NX2_MT_LITE_CTRACK");
    return on;
}

} // namespace VideoCommon::MT
