// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon::MT {

/// One texture/image handle read, in the exact order ConfigureImpl visits views (texture
/// buffers, image buffers, textures, images per stage), so the resolver's payload matches.
struct HandleRead {
    enum class Kind : u8 { TextureBuffer, ImageBuffer, Texture, Image };
    Kind kind;
    u8 stage;
    u8 cbuf_index;
    u8 size_shift;
    u32 cbuf_offset;
    u32 count;
    u8 has_secondary;
    u8 secondary_cbuf_index;
    u8 shift_left;
    u8 secondary_shift_left;
    u32 secondary_cbuf_offset;
    u8 is_written;
    u8 is_blacklist_candidate;
    u8 handle_program; // shader_handle_program.h kind, 0 = plain
    u8 pad;
};

/// Storage buffer descriptor read from a constant buffer (address/size words).
struct SsboRead {
    u8 stage;
    u8 cbuf_index;
    u8 is_written;
    u8 pad;
    u32 cbuf_offset;
};

/// Immutable per pipeline-hash plan (built once from Shader::Info on the sequencer).
struct CapturePlan {
    std::array<u64, 6> hashes{};
    std::vector<HandleRead> handles;
    std::vector<SsboRead> ssbos;
    std::array<u32, 5> enabled_uniform_buffer_masks{};
    std::array<std::array<u32, 18>, 5> uniform_buffer_sizes{};
    u32 num_textures = 0;
    u32 num_images = 0;
    u32 flags = 0;
    enum Flags : u32 {
        UsesTextureBuffers = 1 << 0,
        UsesImageBuffers = 1 << 1,
        UsesRescaling = 1 << 2,
        UsesRenderArea = 1 << 3,
        ForceSlowPath = 1 << 4,
    };
};

} // namespace VideoCommon::MT
