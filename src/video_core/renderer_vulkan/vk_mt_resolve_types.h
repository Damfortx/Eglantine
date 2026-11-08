// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Plain result types of the multithreaded back's resolvers (vk_mt_resolve.h), needed by
// Nx2DrawPipeline's declaration.

#include <array>
#include <atomic>

#include "common/common_types.h"
#include "video_core/mt/resolve_packet.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"


namespace Vulkan::MT {

enum class ResolveStatus : u8 { Ok, Slow, Skip };

enum SlowReason : u8 {
    SlowNone,
    ViewMapMiss,
    ViewNotClean,
    ViewLazyHandle,
    SamplerMapMiss,
    BufferPageMiss,
    OverlayMiss,
    PipelineMiss,
    ArenaFull,
    TextureBuffers,
    NoChannel,
    Exception,
    Injected,
    Other,
    SlowReasonCount,
};

inline constexpr std::array<const char*, SlowReasonCount> SlowReasonNames{
    "none",           "view-map-miss", "view-not-clean", "view-lazy-handle", "sampler-map-miss",
    "buffer-miss",    "overlay-miss",  "pipeline-miss",  "arena-full",       "texture-buffers",
    "no-channel",     "exception",     "injected",       "other",
};

/// Why a resolved draw failed validation at commit.
enum ValidateFail : u8 {
    FailNone,
    FailChannel,
    FailPipeline,
    FailView,
    FailSampler,
    FailBuffer,
    ValidateFailCount,
};
inline constexpr std::array<const char*, ValidateFailCount> ValidateFailNames{
    "none", "channel", "pipeline", "view", "sampler", "buffer"};

struct ViewRef {
    u32 view;
    u32 view_gen;
    u32 image;
    u32 image_gen;
    u32 image_version;
    u8 written;
    u8 blacklist;
    u8 null;
    u8 refresh; ///< EDEN_NX2_MT_RREFRESH: CPU-modified, the committer refreshes it
};
static_assert(sizeof(ViewRef) == 24);

struct SamplerRef {
    u32 sampler;
    u32 gen;
};

enum BufferKind : u8 { KindUbo, KindSsbo, KindVertex, KindIndex, KindUboStream };

/// Why a resolved draw's buffer half (EDEN_NX2_MT_RBUF) was left to the committer.
enum BufferFail : u8 {
    BufNone,
    BufOff,
    BufIndirect,
    BufInlineIndex,
    BufAccurateFences,
    BufStorageLimit,
    BufLayout,
    BufMissingUniform,
    BufUniformMemo,
    BufRingFull,
    BufNullStorage,
    BufNoRing,
    BufFailCount,
};
inline constexpr std::array<const char*, BufFailCount> BufferFailNames{
    "none",           "off",          "indirect",    "inline-index", "accurate-fences",
    "storage-limit",  "layout",       "missing-ubo", "ubo-memo",     "ring-full",
    "null-storage",   "no-ring"};

using BufferRef = VideoCommon::MT::ResolvedBufferRef;
static_assert(u8{KindUbo} == u8{BufferRef::Uniform} && u8{KindSsbo} == u8{BufferRef::Storage} &&
              u8{KindVertex} == u8{BufferRef::Vertex} && u8{KindIndex} == u8{BufferRef::Index} &&
              u8{KindUboStream} == u8{BufferRef::UniformStream});

/// One draw's result, in the resolved ring slot of its seq. Written by the owning resolver,
/// read by the committer after the batch publish.
struct alignas(64) ResolvedDraw {
    u64 seq;
    u64 batch;
    ResolveStatus status;
    u8 reason;
    u8 resolver;
    u8 buffer_fail; ///< BufNone: `payload` holds every descriptor entry (EDEN_NX2_MT_RBUF)
    u32 channel_gen;
    GraphicsPipeline* pipeline;
    u64 buffer_epoch;
    u64 result_end; ///< ResultArena position to release once committed (0: none)
    const ViewRef* views;
    const SamplerRef* samplers;
    const BufferRef* buffers;
    u32 num_views;
    u32 num_samplers;
    u32 num_buffers;
    u32 sampler_creations;
    /// PushImageDescriptors' entries of every stage (stage_begin/count index them). With
    /// EDEN_NX2_MT_RBUF they sit inside the full payload, after each stage's buffer entries.
    const DescriptorUpdateEntry* image_entries;
    /// The whole descriptor payload in ConfigureImpl's order (valid when buffer_fail == BufNone).
    const DescriptorUpdateEntry* payload;
    u32 num_payload;
    u32 pad2;
    std::array<u32, Tegra::Engines::Maxwell3D::Regs::MaxShaderStage> stage_begin;
    std::array<u32, Tegra::Engines::Maxwell3D::Regs::MaxShaderStage> stage_count;
    std::array<u32, Shader::Backend::SPIRV::NUM_TEXTURE_AND_IMAGE_SCALING_WORDS> rescaling;
};
static_assert(sizeof(DescriptorUpdateEntry) % 8 == 0);

/// Per-resolver counters (relaxed; read by the stats logger).
struct ResolverCounters {
    std::atomic<u64> draws{};
    std::atomic<u64> batches{};
    std::atomic<u64> busy_ns{};
    std::atomic<u64> wait_ns{};
    std::atomic<u64> sampler_creations{};
    std::array<std::atomic<u64>, SlowReasonCount> slow{};
    // EDEN_NX2_MT_RBUF.
    std::array<std::atomic<u64>, BufFailCount> buffer_fail{};
    std::atomic<u64> ring_bytes{};
    std::atomic<u64> ring_reuses{};
    std::atomic<u64> ring_streams{};
};

} // namespace Vulkan::MT
