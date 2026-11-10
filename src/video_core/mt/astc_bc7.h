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
[[nodiscard]] inline bool Read(const char* name, bool default_on = false) {
    const char mode = Tegra::Nx2EnvMode(name);
    return mode != 'a' && Tegra::Nx2Switch(mode, default_on);
}
} // namespace AstcBc7Detail

/// EDEN_NX2_ASTC_BC7=1: GPU-transcoded ASTC goes to BC7 where BC3 was used. Default off.
[[nodiscard]] inline bool AstcBc7Requested() {
    static const bool on = AstcBc7Detail::Read("EDEN_NX2_ASTC_BC7");
    return on;
}

/// EDEN_NX2_ASTC_BC7_OPAQUE_BC1 (default on since batch 9 U2; '0' off): under BC7, opaque ASTC
/// still goes to BC1 (8 bytes a block, half of BC7). Opacity is decided before the image is
/// created, from the guest ASTC blocks' endpoint modes / void-extent alpha (ImageInfo::
/// astc_opaque, Tegra::Texture::ASTC::IsOpaque, TextureCache JoinImages), so the format never
/// changes after creation. The ASTC disk cache key includes the BC mode (AstcDiskCacheParams::
/// bc_mode, 0 = BC1, 2 = BC7) and astc_opaque, so BC1 and BC7 entries never alias.
/// EDEN_NX2_ASTC_BC1=0 still keeps every opaque texture out of BC1.
[[nodiscard]] inline bool AstcBc7OpaqueBc1() {
    static const bool on = AstcBc7Detail::Read("EDEN_NX2_ASTC_BC7_OPAQUE_BC1", true);
    return on;
}

} // namespace VideoCommon::MT
