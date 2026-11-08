// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "common/common_types.h"
#include "video_core/nx2.h"

/// Switches of the multithreaded GPU back (docs/mt-plan.md). All read once per process; unset
/// keeps today's single-threaded back.
namespace VideoCommon::MT {

/// Numeric environment switch (decimal), `fallback` when unset or not a number.
[[nodiscard]] inline u32 EnvNumber(const char* name, u32 fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] < '0' || value[0] > '9') {
        return fallback;
    }
    return static_cast<u32>(std::strtoul(value, nullptr, 10));
}

/// EDEN_NX2_MT: capture on the sequencer, record on a committer thread (phase 4+).
[[nodiscard]] inline bool Enabled() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT");
    return Tegra::Nx2Switch(mode, false);
}

/// EDEN_NX2_MT_SELFTEST=1: stress the primitives at startup and log PASS/FAIL.
[[nodiscard]] inline bool SelftestEnabled() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_SELFTEST");
    return Tegra::Nx2Switch(mode, false);
}

/// EDEN_NX2_MT_MIRROR=N: keep a register mirror fed by deltas and compare it with the real
/// engine every N draws (0 = off).
[[nodiscard]] inline u32 MirrorInterval() {
    static const u32 interval = EnvNumber("EDEN_NX2_MT_MIRROR", 0);
    return interval;
}

/// EDEN_NX2_MT_RESOLVERS: resolver threads (phase 5).
[[nodiscard]] inline u32 Resolvers() {
    static const u32 count = EnvNumber("EDEN_NX2_MT_RESOLVERS", 2);
    return count;
}

/// EDEN_NX2_MT_BATCH: draws per batch.
[[nodiscard]] inline u32 BatchSize() {
    static const u32 size = EnvNumber("EDEN_NX2_MT_BATCH", 16);
    return size == 0 ? 16 : size;
}

/// EDEN_NX2_MT_EBR: free cache slots and retired tables through epochs (on when MT is).
[[nodiscard]] inline bool DeferredFree() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_EBR");
    static const bool on = Tegra::Nx2Switch(mode, Enabled());
    return on;
}

/// EDEN_NX2_MT_SHADOW=1|2: resolvers run, but the committer records the phase-4 result and
/// compares it with theirs (2 also logs every validation failure with the ids involved).
[[nodiscard]] inline u32 Shadow() {
    static const u32 level = EnvNumber("EDEN_NX2_MT_SHADOW", 0);
    return level;
}

/// EDEN_NX2_MT_PIPE=resolver: FixedPipelineState::Refresh and the key lookup on the resolvers
/// (default seq: the packet carries the pipeline the sequencer looked up).
[[nodiscard]] inline bool PipeOnResolver() {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_NX2_MT_PIPE");
        return value != nullptr && value[0] == 'r';
    }();
    return on;
}

/// EDEN_NX2_MT_RDESC: resolvers write descriptor-buffer bytes into their own blocks (off: the
/// committer writes them from the resolver's payload).
[[nodiscard]] inline bool ResolverDescriptors() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_RDESC");
    static const bool on = Tegra::Nx2Switch(mode, true);
    return on;
}

/// EDEN_NX2_MT_RUBO: resolvers stream small uniform buffers into their own ring (off: the
/// committer streams them from the packet).
[[nodiscard]] inline bool ResolverUniforms() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_RUBO");
    static const bool on = Tegra::Nx2Switch(mode, true);
    return on;
}

/// EDEN_NX2_MT_RCOMMIT (on): record validated resolver results (the texture half of
/// ConfigureImpl from the resolver; EDEN_NX2_MT_SHADOW turns it off). =0: the resolvers still run
/// and every draw takes the phase-4 path.
[[nodiscard]] inline bool ResolvedCommit() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_RCOMMIT");
    static const bool on = Tegra::Nx2Switch(mode, true);
    return on;
}

/// EDEN_NX2_MT_ADAPTIVE (on): a committer waiting for an open batch asks the sequencer to close
/// it after its next draw (batches shrink when the committer is ahead, grow up to
/// EDEN_NX2_MT_BATCH when it lags).
[[nodiscard]] inline bool AdaptiveBatches() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_ADAPTIVE");
    static const bool on = Tegra::Nx2Switch(mode, true);
    return on;
}

/// EDEN_NX2_MT_STEAL_US=N (default 0 = off): a committer that waited N us for a resolver's batch
/// records the draw itself (phase-4 path) instead of waiting longer.
[[nodiscard]] inline u32 StealMicros() {
    static const u32 us = EnvNumber("EDEN_NX2_MT_STEAL_US", 0);
    return us;
}

/// EDEN_NX2_MT_RBUF (on): resolvers also resolve the buffer half of ConfigureImpl (uniform,
/// storage, vertex and index ranges, streamed uniforms into a per-resolver ring) and build the
/// whole descriptor payload; the committer only applies the intents and records. =0: the
/// committer runs the buffer half (phase 5 RCOMMIT).
[[nodiscard]] inline bool ResolverBufferHalf() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_RBUF");
    static const bool on = Tegra::Nx2Switch(mode, true);
    return on;
}

/// EDEN_NX2_MT_REPLAY (on): the committer's scheduler records go into per-render-pass POD op
/// lists (one scheduler record per list), with bind elision; =2 also checks the order the
/// worker runs the ops in. =0 off.
[[nodiscard]] inline u32 Replay() {
    static const u32 level = [] {
        const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_REPLAY");
        if (mode == '2') {
            return 2u;
        }
        return Tegra::Nx2Switch(mode, true) ? 1u : 0u;
    }();
    return level;
}

/// EDEN_NX2_MT_COMPACT (on under EDEN_NX2_MT): smaller draw packets (packet_compact.h): a
/// compact header, TIC/TSC entries referenced from stream-ordered replicas instead of re-sent,
/// and streamed uniform buffers sent as "same as the slot's last bytes" or as changed 64-byte
/// chunks. =0: today's layout.
[[nodiscard]] inline bool Compact() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_COMPACT");
    static const bool on = Enabled() && Tegra::Nx2Switch(mode, true);
    return on;
}

/// EDEN_NX2_MT_RCHECK=1: resolver-side asserts (role, guard held, ring overrun, consistency of
/// the committer's geometry flags); 2 also fails 1 % of resolves on purpose.
[[nodiscard]] inline u32 ResolverChecks() {
    static const u32 level = EnvNumber("EDEN_NX2_MT_RCHECK", 0);
    return level;
}

} // namespace VideoCommon::MT
