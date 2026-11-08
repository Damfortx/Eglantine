// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"

namespace VideoCommon::MT {

struct CapturePlan;

/// DrawStream entry kinds (spmc_stream.h header word, bits 0-7).
enum class StreamEntry : u8 {
    Pad = 0,
    Deltas,      ///< (method | value << 32) pairs for the 3D engine
    ComputeDeltas,
    CbBind,      ///< CbBindDelta
    RootTable,   ///< RootTableDelta run
    DrawPacket,
    ComputePacket,
    Command,     ///< ordered, asynchronous
    Serial,      ///< the sequencer blocks until it ran
    BatchEnd,
    Mark,        ///< ring position for the front's waits
    ChannelSync,
};

struct TicCapture {
    u32 index;
    u32 pad;
    std::array<u32, 8> raw; // 32-byte TIC entry
};

struct TscCapture {
    u32 index;
    u32 pad;
    std::array<u32, 8> raw; // 32-byte TSC entry
};

struct UboCapture {
    enum class Kind : u8 { Staged, Cached, Disabled };
    Kind kind;
    u8 stage;
    u8 slot;
    u8 pad;
    u32 size;
    u64 device_addr; // DAddr for Cached
    u64 staging_buffer; // VkBuffer handle for Staged
    u64 staging_offset;
};

struct SsboCapture {
    u8 stage;
    u8 index;
    u8 is_written;
    u8 pad;
    u32 size;
    u64 device_addr;
};

struct VertexCapture {
    u32 index;
    u32 stride;
    u64 device_addr;
    u32 size;
    u32 pad;
};

struct IndexCapture {
    u64 device_addr;
    u32 size;
    u32 format;
    u32 first;
    u32 count;
};

/// CPU-modified bytes copied into the sequencer staging ring at capture, applied by the
/// committer in stream order.
struct StagedUpload {
    u64 device_addr;
    u32 size;
    u32 pad;
    u64 staging_ref; // ring offset / tag
};

struct DrawPacketHeader {
    u64 seq;
    u64 ring_position;
    u32 flags;
    u32 topology;
    u32 vertex_first;
    u32 vertex_count;
    u32 base_instance;
    u32 num_instances;
    s32 base_vertex;
    u32 zpass_enable;
    const CapturePlan* plan;
    std::array<u64, 6> hashes;
    u64 capture_write_gen;
    u16 num_tic;
    u16 num_tsc;
    u16 num_ubo;
    u16 num_ssbo;
    u16 num_vertex;
    u16 num_staged;
    u16 inline_index_bytes;
    u16 has_index;
    enum Flags : u32 {
        Indexed = 1 << 0,
        InlineIndex = 1 << 1,
        SlowPath = 1 << 2,
        Clear = 1 << 3,
    };
};

/// Compute analog: launch description copy plus the same capture arrays.
struct ComputePacketHeader {
    u64 seq;
    u64 ring_position;
    u32 flags;
    u32 qmd_words;
    const CapturePlan* plan;
    u64 hash;
    u16 num_tic;
    u16 num_tsc;
    u16 num_ubo;
    u16 num_ssbo;
    u16 num_staged;
    u16 indirect_bytes;
};

} // namespace VideoCommon::MT
