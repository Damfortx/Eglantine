// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/mt/lite_switches.h"

/// MT lite iteration 3 (docs/mt-lite-plan3.md): the split's front (GPU thread: DmaPusher, puller,
/// Maxwell3D Front*, ring producer). On by default under EDEN_NX2_MT_LITE=1 (with EDEN_NX2_MT=1);
/// EDEN_NX2_MT_LITE_<X>=0 turns one off. Unlike the back pieces, '1' turns one on without the MT
/// back too (they are front-only and also apply to the plain split), so MT unset stays the
/// baseline unless asked. Read once.
namespace VideoCommon::MT {

namespace Lite3Detail {
[[nodiscard]] inline bool Read(const char* name) {
    const char mode = Tegra::Nx2EnvMode(name);
    if (mode == '1') {
        return true;
    }
    if (mode == '0') {
        return false;
    }
    return Lite();
}
} // namespace Lite3Detail

/// EDEN_NX2_MT_LITE_KNOWNLIST (L14): a command list on pages the GPU has not written, whose writer
/// wait found the copy/compute writers already done, is noted as a known CPU command list like one
/// that waited (IsKnownCpuCommandList). With EDEN_NX2_MT_LITE_WRITERS nearly no writer wait waits,
/// so no page was ever noted and every GPFIFO entry (~85k/s) went through WaitForWriters (and its
/// FlushState, which split the gathered state block at every entry).
[[nodiscard]] inline bool LiteKnownCpuLists() {
    static const bool on = Lite3Detail::Read("EDEN_NX2_MT_LITE_KNOWNLIST");
    return on;
}

/// EDEN_NX2_MT_LITE_FBATCH (L15): runs of incrementing 3D register writes (below the macro range)
/// go from ProcessCommands to Maxwell3D::FrontWriteRun in one call (EDEN_NX2_FRONT_BATCH, single
/// words too), which applies state writes inline (shadow RAM, same-value skip, state block) with
/// the shadow RAM mode read once per run; executable methods still take FrontWrite.
[[nodiscard]] inline bool LiteFrontBatch() {
    static const bool on = Lite3Detail::Read("EDEN_NX2_MT_LITE_FBATCH");
    return on;
}

/// EDEN_NX2_MT_LITE_FCALL (L16): cheaper per-method front calls: DmaPusher::CallMethod and
/// CallMultiMethod go straight to the 3D front (no writer/engine checks first, which never
/// apply to Maxwell3D), and a macro's same-value state write is skipped in the JIT's emit
/// callback before FrontWrite.
[[nodiscard]] inline bool LiteFrontCalls() {
    static const bool on = Lite3Detail::Read("EDEN_NX2_MT_LITE_FCALL");
    return on;
}

/// EDEN_NX2_MT_LITE_DMAKNOWN (L17): EDEN_NX2_DMA_READ_KNOWN defaults on: an MME DMA read of pages
/// a read already found not GPU-written after the writers ran skips the writer wait (~900/s,
/// ~45 ms/s of front wait in title 1, all on the sequencer).
[[nodiscard]] inline bool LiteDmaKnown() {
    static const bool on = Lite3Detail::Read("EDEN_NX2_MT_LITE_DMAKNOWN");
    return on;
}

/// EDEN_NX2_MT_LITE_EXECNODRAIN (L18): EDEN_NX2_SEM_EXECUTE_NODRAIN defaults on: a SEM_EXECUTE
/// acquire no queued release predicts goes on without draining the back and releasing fences
/// (an unmet one never blocks the channel anyway; it only made the front wait).
[[nodiscard]] inline bool LiteExecNoDrain() {
    static const bool on = Lite3Detail::Read("EDEN_NX2_MT_LITE_EXECNODRAIN");
    return on;
}

} // namespace VideoCommon::MT
