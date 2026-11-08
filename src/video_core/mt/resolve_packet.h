// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

/// Draw packet v2 arrays (docs/mt-phase5-spec.md §4.3): what the sequencer translated through
/// the GPU page table for a draw, so resolver threads never translate (Serial Map/Unmap change
/// the page table while they run).
namespace VideoCommon::MT {

/// A storage buffer descriptor of the draw, in BindGraphicsStorageBuffer order (every
/// descriptor of every stage, `skipped` where the binding is null).
struct SsboEntry {
    u8 stage;
    u8 index; ///< ssbo_index within the stage
    u8 is_written;
    u8 skipped; ///< gpu_addr == 0 or an empty/untranslated binding: the null binding
    u32 size;
    u64 device_addr;
    u64 gpu_addr;
};
static_assert(sizeof(SsboEntry) == 24);

/// An index or vertex range the draw binds (enabled streams only).
struct GeometryEntry {
    enum Kind : u8 { Index = 0, Vertex = 1, InlineIndex = 2 };
    u8 kind;
    u8 stream;
    u16 pad;
    u32 size;
    u64 device_addr; ///< 0: the null binding
};
static_assert(sizeof(GeometryEntry) == 16);

} // namespace VideoCommon::MT
