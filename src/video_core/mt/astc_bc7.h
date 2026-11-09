// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/nx2.h"

/// Batch 7 T1 (docs/batch7-plan.md): ASTC re-compressed to BC7 instead of BC3 by the GPU encoder
/// (bc_encoder.comp mode 2). Only images that go through the GPU transcode become BC7 (the
/// Vulkan runtime decides per image at creation; the CPU path has no BC7 encoder and stays BC3).
/// The format is fixed when an image is made, so 'a' (alternating) is treated as off. Read once.
namespace VideoCommon::MT {

namespace AstcBc7Detail {
[[nodiscard]] inline bool Read(const char* name) {
    const char mode = Tegra::Nx2EnvMode(name);
    return mode != 'a' && Tegra::Nx2Switch(mode, false);
}
} // namespace AstcBc7Detail

/// EDEN_NX2_ASTC_BC7=1: GPU-transcoded ASTC goes to BC7 where BC3 was used. Default off.
[[nodiscard]] inline bool AstcBc7Requested() {
    static const bool on = AstcBc7Detail::Read("EDEN_NX2_ASTC_BC7");
    return on;
}

/// EDEN_NX2_ASTC_BC7_OPAQUE_BC1=1: under BC7, opaque ASTC still goes to BC1 (half the memory).
/// Default off: opaque textures go to BC7 too.
[[nodiscard]] inline bool AstcBc7OpaqueBc1() {
    static const bool on = AstcBc7Detail::Read("EDEN_NX2_ASTC_BC7_OPAQUE_BC1");
    return on;
}

} // namespace VideoCommon::MT
