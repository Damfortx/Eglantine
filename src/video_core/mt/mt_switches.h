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

} // namespace VideoCommon::MT
