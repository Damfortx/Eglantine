// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Plain result types of the multithreaded back's resolvers (vk_mt_resolve.h), needed by
// Nx2DrawPipeline's declaration.

#include <array>
#include <atomic>

#include "common/common_types.h"
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
    u8 pad;
};
static_assert(sizeof(ViewRef) == 24);

struct SamplerRef {
    u32 sampler;
    u32 gen;
};

enum BufferKind : u8 { KindUbo, KindSsbo, KindVertex, KindIndex };

struct BufferRef {
    u32 buffer;
    u32 gen;
    u32 offset;
    u32 size;
    u8 kind;
    u8 written;
    u8 stage;
    u8 slot;
    u32 pad;
    u64 device_addr;
};
static_assert(sizeof(BufferRef) == 32);

/// One draw's result, in the resolved ring slot of its seq. Written by the owning resolver,
/// read by the committer after the batch publish.
struct alignas(64) ResolvedDraw {
    u64 seq;
    u64 batch;
    ResolveStatus status;
    u8 reason;
    u8 resolver;
    u8 pad;
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
    /// PushImageDescriptors' entries of every stage (stage_begin/count index them).
    const DescriptorUpdateEntry* image_entries;
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
};

} // namespace Vulkan::MT
