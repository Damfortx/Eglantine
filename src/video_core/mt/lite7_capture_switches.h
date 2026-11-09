// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"
#include "video_core/mt/lite_switches.h"
#include "video_core/nx2.h"

/// MT lite batch 7, R2: a cheaper sequencer capture (docs/batch7-plan.md). Each piece has its
/// own switch: on by default under EDEN_NX2_MT_LITE=1 (with EDEN_NX2_MT=1), '0' off, '1' on
/// without LITE (still only with EDEN_NX2_MT: they are all sequencer pieces), 'a' alternating
/// (Nx2AltOn, decided per draw / per lock scope, which every piece allows: none keeps state
/// across draws that the other setting would misread). The modes are read once.
namespace VideoCommon::MT {

namespace Lite7Detail {
/// '0', '1' or 'a' for a piece.
[[nodiscard]] inline char Mode(const char* name) {
    if (!Enabled()) {
        return '0';
    }
    const char mode = Tegra::Nx2EnvMode(name);
    if (mode == '0' || mode == '1' || mode == 'a') {
        return mode;
    }
    return Lite() ? '1' : '0';
}
/// Whether a piece with this mode runs now.
[[nodiscard]] inline bool On(char mode) {
    return mode == '1' || (mode == 'a' && Tegra::Nx2AltOn());
}
[[nodiscard]] inline const char* Name(char mode) {
    return mode == 'a' ? "alternating" : (mode == '1' ? "on" : "off");
}
} // namespace Lite7Detail

/// EDEN_NX2_MT_LITE7_DESCDEDUP (C1): a TIC/TSC entry read twice by one capture (a sampler
/// shared by several textures, a texture bound in several stages) is read and recorded once; the
/// second read reuses the first one's bytes. Exact: the overlay is the union of the reads, so
/// a duplicate read added nothing to it (FinishOverlay merged it, FinishOverlayCompactSlots
/// skipped it), and both reads are within the same capture with nothing ordered between them.
[[nodiscard]] inline char Lite7DescDedupMode() {
    static const char mode = Lite7Detail::Mode("EDEN_NX2_MT_LITE7_DESCDEDUP");
    return mode;
}

/// EDEN_NX2_MT_LITE7_UBOLOCK (C2, needs EDEN_NX2_MT_LITE_SEQLOCK): a draw's staged uniform
/// buffers (the bytes streamed in the packet) are read after the tracker lock is released, as
/// SEQLOCK does for the upload bytes. The decisions stay under the lock: the streamed/cached
/// choice (IsRegionGpuModified), the UBO_GEN protection check and generation (whose hit needs
/// no bytes). Only the guest memory copy, the compare with the committer's kept bytes and the
/// patch go after it (sequencer-owned state). Bytes read later are at least as new: a guest
/// write in between bumps the generation the memo keeps (the next capture misses) exactly as a
/// write right after the capture would.
[[nodiscard]] inline char Lite7UboLockMode() {
    static const char mode = Lite7Detail::Mode("EDEN_NX2_MT_LITE7_UBOLOCK");
    return mode;
}

/// EDEN_NX2_MT_LITE7_LOCKSKIP (C3): a TrackerScope (or the sequencer's SeqTrackerLock) records
/// the tracker mutex its thread holds; a nested TrackerScope on the same mutex then skips the
/// recursive lock/unlock (thread id, owner load, depth). A capture takes the tracker lock once
/// and then ~10-30 nested scopes (every MtTakeUploads, MtIsRegionGpuModified,
/// IsRegionCpuModified, MtCaptureWrittenRange, MtTrapWrite...). Exact: the outer holder keeps the
/// mutex until after the nested scope ends (scopes are strictly nested).
[[nodiscard]] inline char Lite7LockSkipMode() {
    static const char mode = Lite7Detail::Mode("EDEN_NX2_MT_LITE7_LOCKSKIP");
    return mode;
}

/// EDEN_NX2_MT_LITE7_OVERLAY (C4): FinishOverlayCompactSlots copies an inserted descriptor's
/// bytes from the read it kept (offset noted in the first pass) instead of searching the kept
/// reads for it again (inserts x reads compares a draw). Exact: the first kept descriptor read
/// at that address is the one noted (duplicates are dropped before they are kept).
[[nodiscard]] inline char Lite7OverlayMode() {
    static const char mode = Lite7Detail::Mode("EDEN_NX2_MT_LITE7_OVERLAY");
    return mode;
}

/// EDEN_NX2_MT_LITE7_CAPTURE_CHECK=1 (diagnostic, needs EDEN_NX2_MT): computes both ways where
/// it can and counts the differences in the "MT lite 7 capture" stats line (EDEN_NX2_MT_STATS):
/// C1 reads a deduplicated descriptor again and compares (a difference is a concurrent guest
/// write, not a capture bug, but should stay ~0); C2 reads the bytes under the lock too and
/// compares with the late read (changes in between: the same race SEQLOCK accepts for uploads);
/// C3 checks that a skipped scope's mutex is held by the thread (must be 0); C4 also searches and
/// compares the offsets (must be 0).
[[nodiscard]] inline bool Lite7CaptureCheck() {
    static const bool on =
        Enabled() &&
        Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_MT_LITE7_CAPTURE_CHECK"), false);
    return on;
}

/// EDEN_NX2_MT_STATS, read here too (tracker_lock.h does not include role.h).
[[nodiscard]] inline bool Lite7Stats() {
    static const bool on = Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_MT_STATS"), false);
    return on;
}

/// C3's counters (any thread; only counted with EDEN_NX2_MT_STATS). The sequencer's LogStats
/// takes them.
inline std::atomic<u64> g_lite7_lock_skips{};
inline std::atomic<u64> g_lite7_lock_skip_mismatches{};

} // namespace VideoCommon::MT
