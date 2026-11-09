// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/mt/mt_switches.h"
#include "video_core/nx2.h"

/// The lightest multithreaded back (docs/mt-lite-plan.md): sequencer + committer, no resolvers,
/// minimum coordination. EDEN_NX2_MT_LITE=1 (with EDEN_NX2_MT=1) turns every piece below on and
/// makes EDEN_NX2_MT_RESOLVERS default to 0; each piece also has its own switch ('0' turns it off
/// under LITE, '1' turns it on without LITE). All are inert with EDEN_NX2_MT unset and read once.
namespace VideoCommon::MT {

namespace LiteDetail {
[[nodiscard]] inline bool Read(const char* name) {
    return Enabled() && Tegra::Nx2Switch(Tegra::Nx2EnvMode(name), Lite());
}
} // namespace LiteDetail

/// EDEN_NX2_MT_LITE_UBO (L1): the committer takes a uniform buffer's streamed/cached decision from
/// the packet (the sequencer made it at capture) instead of asking the buffer tracker again under
/// tracker_mutex for every bound uniform buffer of every draw (~4.6 locks a draw contended with
/// the sequencer's captures). EDEN_NX2_MT_VERIFY keeps the second check (it compares them).
[[nodiscard]] inline bool LiteUboFromCapture() {
    static const bool on = LiteDetail::Read("EDEN_NX2_MT_LITE_UBO");
    return on;
}

/// EDEN_NX2_MT_LITE_LOOP (L2): the committer's per-entry seq_cst fence and committer_quiet
/// exchange go: `committer_quiet` is stored only when it changes, and a sequencer waiting in
/// WaitCommitted is woken from the committer's Idle (fence + check before it spins or sleeps)
/// or by its own bounded sleeps, not by a Dekker check after every entry.
[[nodiscard]] inline bool LiteCommitLoop() {
    static const bool on = LiteDetail::Read("EDEN_NX2_MT_LITE_LOOP");
    return on;
}

/// EDEN_NX2_MT_LITE_TICVALID (L3): no TIC validity bits in draw packets while no resolver runs
/// (only resolvers read them).
[[nodiscard]] inline bool LiteNoTicValid() {
    static const bool on = LiteDetail::Read("EDEN_NX2_MT_LITE_TICVALID");
    return on;
}

/// EDEN_NX2_MT_LITE_DRAIN (L4): the front's puller and sync drains and WaitForPosition wait for
/// the sequencer (`consumed`) and for the committer only up to the last Command that writes guest
/// memory (as EDEN_NX2_MT_DMA_CONSUMED does for MME DMA reads), and no longer wait for the back
/// and the committer to go quiet (their idle hooks submit on their own).
[[nodiscard]] inline bool LiteDrain() {
    static const bool on = LiteDetail::Read("EDEN_NX2_MT_LITE_DRAIN");
    return on;
}

/// EDEN_NX2_MT_LITE_QUIET (L5): a front Drain that waits for the back to go quiet asks both
/// threads at once: the sequencer's idle spin (8192 pauses, ~100 us) and the committer's (4096)
/// end on the request instead of running out first (under EDEN_NX2_MT the request only reached
/// the committer, and only after its spin).
[[nodiscard]] inline bool LiteQuiet() {
    static const bool on = LiteDetail::Read("EDEN_NX2_MT_LITE_QUIET");
    return on;
}

} // namespace VideoCommon::MT
