// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Batch 8 U2: environment overrides for frame generation (present/frame_gen.*, FSR 3 path).
//
// EDEN_NX2_FRAMEGEN       unset: the frame_gen / frame_gen_type settings decide (default off).
//                         1: frame generation on, FSR 3 type (in-tree optical flow +
//                            interpolation compute shaders, no FidelityFX SDK).
//                         0: frame generation off whatever the settings say.
//                         Read once per process; 'a' (alternate) is NOT supported (the present
//                         frame count and swapchain present mode are fixed at creation) and is
//                         treated as unset.
// EDEN_NX2_FRAMEGEN_MULT  2..4: output frames per emulated frame (frame_gen_multiplier).
// EDEN_NX2_FRAMEGEN_RATE  <Hz>: adaptive pacing target (frame_gen_target_rate; 0 = always
//                         generate MULT-1 frames). Set it to the display refresh rate on a
//                         display slower than MULT x the game rate, e.g. 120 for MKW on 120 Hz.
// The sub-switches apply whenever frame generation ends up on (setting or EDEN_NX2_FRAMEGEN=1).

#pragma once

#include <algorithm>
#include <cstdlib>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/nx2.h"

namespace Vulkan::FrameGenSwitches {

/// 'd' (unset / settings), '0' (forced off) or '1' (forced on, FSR 3).
[[nodiscard]] inline char Mode() {
    static const char mode = [] {
        const char value = Tegra::Nx2EnvMode("EDEN_NX2_FRAMEGEN");
        return value == '0' || value == '1' ? value : 'd';
    }();
    return mode;
}

/// Unsigned value of an environment variable, or `fallback` when unset / not a number.
[[nodiscard]] inline u32 EnvU32(const char* name, u32 fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value < '0' || *value > '9') {
        return fallback;
    }
    return static_cast<u32>(std::strtoul(value, nullptr, 10));
}

/// Whether frame generation is active (replaces Settings::values.frame_gen.GetValue()).
[[nodiscard]] inline bool Enabled() {
#ifdef HAS_LSFG
    switch (Mode()) {
    case '0':
        return false;
    case '1':
        return true;
    default:
        return Settings::values.frame_gen.GetValue();
    }
#else
    return false; // frame generation sources not compiled in (ENABLE_LSFG=OFF)
#endif
}

/// Whether the FSR 3 path is used (replaces frame_gen_type == FrameGenType::Fsr3).
[[nodiscard]] inline bool UseFsr3() {
    return Mode() == '1' ||
           Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::Fsr3;
}

/// Replaces Settings::FrameGenMultiplier().
[[nodiscard]] inline u32 Multiplier() {
    static const u32 env = EnvU32("EDEN_NX2_FRAMEGEN_MULT", 0);
    if (env != 0) {
        return std::clamp(env, Settings::MIN_FRAME_GEN_MULTIPLIER,
                          Settings::MAX_FRAME_GEN_MULTIPLIER);
    }
    return Settings::FrameGenMultiplier();
}

/// Replaces Settings::values.frame_gen_target_rate.GetValue().
[[nodiscard]] inline u32 TargetRate() {
    static const u32 env = EnvU32("EDEN_NX2_FRAMEGEN_RATE", ~0u);
    if (env != ~0u) {
        return std::min(env, 240u);
    }
    return Settings::values.frame_gen_target_rate.GetValue();
}

/// Replaces Settings::FrameGenGenerations().
[[nodiscard]] inline size_t Generations() {
    return Enabled() ? Multiplier() - 1 : 0;
}

/// Replaces Settings::FrameGenMaxGenerations().
[[nodiscard]] inline size_t MaxGenerations() {
    if (!Enabled()) {
        return 0;
    }
    if (TargetRate() != 0) {
        return Settings::MAX_FRAME_GEN_MULTIPLIER - 1;
    }
    return Multiplier() - 1;
}

/// One INFO line describing what is active; call at renderer creation.
inline void LogOnce() {
    static const bool logged = [] {
        const char mode = Mode();
        LOG_INFO(Render_Vulkan,
                 "FrameGen: EDEN_NX2_FRAMEGEN={} active={} type={} multiplier={} "
                 "target_rate={} max_generations={}",
                 mode == 'd' ? "unset(settings)" : (mode == '1' ? "1(forced on)" : "0(forced off)"),
                 Enabled(), UseFsr3() ? "fsr3" : "lsfg", Multiplier(), TargetRate(),
                 MaxGenerations());
        return true;
    }();
    (void)logged;
}

} // namespace Vulkan::FrameGenSwitches
