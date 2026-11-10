// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>

#include "common/common_types.h"
#include "video_core/nx2.h"

/// Batch 9 U2: texture cache thresholds from the driver's video memory budget
/// (docs/batch9-plan.md). Backend neutral (TextureCache<P> includes it): no Vulkan types.
/// Switches: '0' off, '1' on, 'a' alternating every 30 s (re-read at each budget check), else
/// the default. Sizes are read once.
namespace VideoCommon::Lite9Vram {

namespace Detail {
constexpr u64 MiB = u64{1} << 20;
[[nodiscard]] inline u64 ReadMiB(const char* name, u64 default_mib) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] < '0' || value[0] > '9') {
        return default_mib * MiB;
    }
    return std::strtoull(value, nullptr, 10) * MiB;
}
} // namespace Detail

/// EDEN_NX2_TC_BUDGET_AUTO (default on, 'a' ok): with VK_EXT_memory_budget the collector's
/// critical/expected thresholds follow the driver's budget (re-read every 30 frames) minus
/// EDEN_NX2_TC_BUDGET_MARGIN_MB, the allocator's block slack and the out-of-memory margin, with
/// no fixed 5 GiB cap (MaxCriticalMemory). Off (or no extension): the batch 8 behaviour (budget
/// minus 1/8, then capped at 5 GiB). EDEN_NX2_TC_EXPECTED_MIB / _CRITICAL_MIB still win (capped
/// at what the budget allows).
[[nodiscard]] inline bool BudgetAutoOn() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_TC_BUDGET_AUTO");
    return Tegra::Nx2Switch(mode, true);
}

/// EDEN_NX2_TC_BUDGET_MARGIN_MB (default 768): room kept free under the budget besides the
/// block slack and the out-of-memory margin (driver internals, swapchain, transient spikes).
[[nodiscard]] inline u64 BudgetMargin() {
    static const u64 margin = Detail::ReadMiB("EDEN_NX2_TC_BUDGET_MARGIN_MB", 768);
    return margin;
}

/// EDEN_NX2_TC_BUDGET_FLOOR_MB (default 1536): critical never goes below this (below it the
/// cache would thrash: evict and decode the same textures every few frames).
[[nodiscard]] inline u64 BudgetFloor() {
    static const u64 floor = Detail::ReadMiB("EDEN_NX2_TC_BUDGET_FLOOR_MB", 1536);
    return floor;
}

/// EDEN_NX2_TC_BUDGET_GENTLE_PCT (default 15, 0-50): the gentle (high-priority) collector starts
/// this share of critical below it (at least 512 MiB), so the budget is seldom reached.
[[nodiscard]] inline u64 GentlePercent() {
    static const u64 percent = [] {
        const char* value = std::getenv("EDEN_NX2_TC_BUDGET_GENTLE_PCT");
        const u64 parsed = value != nullptr ? std::strtoull(value, nullptr, 10) : 15ULL;
        return (std::min)(parsed, u64{50});
    }();
    return percent;
}

/// EDEN_NX2_TC_BUDGET_PROACTIVE (default on, 'a' ok): in the gentle band (expected..critical)
/// the collector evicts at most one image a frame that must be downloaded first (each one is a
/// full GPU wait); more only once critical is reached. Bounds the work the earlier start adds.
[[nodiscard]] inline bool ProactiveOn() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_TC_BUDGET_PROACTIVE");
    return Tegra::Nx2Switch(mode, true);
}

struct BudgetCaps {
    u64 critical;
    u64 expected;
    u64 reserve; ///< margin + out-of-memory + slack actually subtracted
};

/// Thresholds (in GetDeviceMemoryUsage() terms, i.e. all of this process' device memory, so
/// buffers, staging and swapchain are already part of the usage they are compared with) from
/// the budget. `budget` must be > 2 GiB (callers check).
[[nodiscard]] inline BudgetCaps DeriveCaps(u64 budget, u64 pressure_margin, u64 slack_margin) {
    constexpr u64 MiB = Detail::MiB;
    const u64 reserve = (std::min)(BudgetMargin() + pressure_margin + slack_margin, budget / 2);
    u64 critical = budget - reserve;
    // The floor never goes past budget - 256 MiB (budget > 2 GiB keeps that above 1.75 GiB).
    critical = (std::max)(critical, (std::min)(BudgetFloor(), budget - 256 * MiB));
    const u64 band = (std::max)(512 * MiB, critical / 100 * GentlePercent());
    const u64 expected = critical - (std::min)(band, critical / 2);
    return BudgetCaps{critical, expected, reserve};
}

/// Below this device-local budget at startup a warning suggests closing programs or lowering
/// the resolution: 4 GiB at 1x plus 0.5 GiB per extra 1x of render area (5.5 GiB at 2x).
[[nodiscard]] inline u64 LowVramThreshold(float up_factor) {
    const double factor = (std::clamp)(static_cast<double>(up_factor), 0.5, 4.0);
    const double gib = (std::clamp)(4.0 + 0.5 * (factor * factor - 1.0), 3.5, 12.0);
    return static_cast<u64>(gib * 1024.0) * Detail::MiB;
}

/// EDEN_NX2_TC_CHURN=1 (default off, diagnostics only): every 10 s one "Texture churn in 10 s"
/// line: collector evictions (all / ASTC), ASTC images created and how many of them were
/// re-created at an address the collector had evicted an ASTC image from (an evict/re-decode
/// loop), ASTC content refreshes (each one a decode/transcode or disk cache read), frames in
/// high-priority / aggressive mode, device memory use against the thresholds.
[[nodiscard]] inline bool ChurnStatsOn() {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_NX2_TC_CHURN");
        return value != nullptr && value[0] == '1';
    }();
    return on;
}

} // namespace VideoCommon::Lite9Vram
