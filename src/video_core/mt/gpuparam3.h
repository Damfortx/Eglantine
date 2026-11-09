// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"
#include "video_core/nx2.h"

/// GPU params v2 robustness (docs/batch7-plan.md, R1). Self-contained: gpuparam2.h includes it;
/// the pipeline caches, the shader environment and the Vulkan rasterizer include it directly.
/// Every switch here only acts where GPU params v2 (EDEN_NX2_MT_LITE_GPUPARAM2 /
/// EDEN_NX2_GPUPARAM2) already draws with the built-ins: with v2 off nothing changes.
///
/// (a) Fallback: a pipeline built for the hint OnGpuParamDraw whose stages still read the draw
///     parameter words cb[bank][0x640..0x648) from memory (non-vertex stages, sub-word reads,
///     two-word reads not replaced, dynamic offsets into that bank) carries the bits below
///     (GraphicsPipeline::DrawParamWordReads). The back's built-in draw raises g_builtin_pending
///     around its indirect draw; PipelineCache::CurrentGraphicsPipeline, finding such a pipeline,
///     returns no pipeline (the draw is not recorded) and sets g_builtin_skipped; GpuParamDraw
///     then draws the call again through v1's per-record constant buffer copies (a group: one
///     copied draw per record, the records still in GPU memory).
/// (b) LDC.64: a two-word read cb[bank][0x640] (IR GetCbufU32x2) in a vertex stage becomes
///     (BaseVertex, BaseInstance) like the one-word reads.
/// (c) Multi: macro calls with a draw count and N records whose constant buffer words are the
///     current record's base vertex / instance at every draw (classifier "per-draw" plan) draw
///     with one vkCmdDrawIndexedIndirectCount and the built-ins.
namespace VideoCommon::MT::GpuParam {

// ---------------------------------------------------------------------------------- switches

/// EDEN_NX2_GPUPARAM2_FALLBACK (=0/1/a, default on): (a).
[[nodiscard]] inline bool FallbackOn() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_GPUPARAM2_FALLBACK");
    return Tegra::Nx2Switch(mode, true);
}
/// EDEN_NX2_GPUPARAM2_FALLBACK_DYNAMIC (=0/1, default on): (a) also for pipelines whose only
/// such reads are of that bank at a dynamic offset (they may or may not land on the words).
[[nodiscard]] inline bool FallbackDynamicOn() {
    static const bool on = Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_GPUPARAM2_FALLBACK_DYNAMIC"),
                                            true);
    return on;
}
/// EDEN_NX2_GPUPARAM2_LDC64 (=0/1, default on): (b). Read when a pipeline's shaders are
/// translated (runtime and disk cache); pipeline keys are unchanged.
[[nodiscard]] inline bool Ldc64On() {
    static const bool on = Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_GPUPARAM2_LDC64"), true);
    return on;
}
/// EDEN_NX2_GPUPARAM2_MULTI (=0/1/a, default on): (c).
[[nodiscard]] inline bool MultiOn() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_GPUPARAM2_MULTI");
    return Tegra::Nx2Switch(mode, true);
}

// ------------------------------------------------------------------------- pipeline flags

/// GraphicsPipeline::DrawParamWordReads bits.
inline constexpr u8 DrawParamReadsFixed = 1;   ///< reads of cb[bank][0x640..0x648) kept
inline constexpr u8 DrawParamReadsDynamic = 2; ///< reads of that bank at a dynamic offset

[[nodiscard]] inline u8 FallbackMask() {
    if (!FallbackOn()) {
        return 0;
    }
    return static_cast<u8>(DrawParamReadsFixed | (FallbackDynamicOn() ? DrawParamReadsDynamic : 0));
}

/// The back's built-in draw is being looked up (set and cleared by Maxwell3D::GpuParamDraw
/// around its synchronous indirect draw; read by PipelineCache::CurrentGraphicsPipeline on the
/// same thread, or on the committer for a Serial draw the sequencer waits for). The mask of
/// DrawParamReads* bits that make the lookup refuse the pipeline; 0: no built-in draw.
inline std::atomic<u8> g_builtin_pending{};
/// The lookup refused the pipeline (GpuParamDraw takes it and draws with copies instead).
inline std::atomic<bool> g_builtin_skipped{};
/// The "no pipeline" report that follows the refused lookup is not a failure.
inline std::atomic<bool> g_builtin_report_suppress{};

/// CurrentGraphicsPipeline's gate: whether this pipeline (DrawParamWordReads bits) must not
/// draw the pending built-in draw.
[[nodiscard]] inline bool RefuseBuiltinPipeline(u8 reads) {
    const u8 mask = g_builtin_pending.load(std::memory_order_relaxed);
    if (mask == 0 || (reads & mask) == 0) {
        return false;
    }
    g_builtin_skipped.store(true, std::memory_order_relaxed);
    g_builtin_report_suppress.store(true, std::memory_order_relaxed);
    return true;
}

// --------------------------------------------------------------------------- device support

/// vkCmdDraw*IndirectCount available: 0 unknown (assumed, as before), 1 yes, 2 no (set by the
/// Vulkan pipeline cache from the device's dispatch table). Multi-draw calls need it.
inline std::atomic<u8> g_count_draw_support{};
[[nodiscard]] inline bool CountDrawSupported() {
    return g_count_draw_support.load(std::memory_order_relaxed) != 2;
}

// ------------------------------------------------------------------------------ statistics

inline std::atomic<u64> g_r1_fallback_draws{};    ///< (a) built-in draws redone with copies
inline std::atomic<u64> g_r1_fallback_records{};  ///< (a) records those took
inline std::atomic<u64> g_r1_multi_unfixable{};   ///< (a) per-draw multi calls drawn as before
inline std::atomic<u64> g_r1_flagged_pipelines{}; ///< (a) pipelines built with reads kept
inline std::atomic<u64> g_r1_ldc64_reads{};       ///< (b) two-word reads replaced (builds)
inline std::atomic<u64> g_r1_ldc64_pipelines{};   ///< (b) pipelines with at least one
inline std::atomic<u64> g_r1_count_draws{};       ///< (c) vkCmdDrawIndexedIndirectCount draws
inline std::atomic<u64> g_r1_count_max_records{}; ///< (c) their maxDrawCount sum

/// Appended to "GPU params in 10 s" (front; counters cleared).
[[nodiscard]] inline std::string FormatR1(u64 multi_copy_free, u64 multi_per_draw_waiting,
                                          u64 multi_no_count) {
    const auto take = [](std::atomic<u64>& counter) {
        return counter.exchange(0, std::memory_order_relaxed);
    };
    const u64 fallback_draws = take(g_r1_fallback_draws);
    const u64 fallback_records = take(g_r1_fallback_records);
    const u64 unfixable = take(g_r1_multi_unfixable);
    const u64 flagged = take(g_r1_flagged_pipelines);
    const u64 ldc64_reads = take(g_r1_ldc64_reads);
    const u64 ldc64_pipelines = take(g_r1_ldc64_pipelines);
    const u64 count_draws = take(g_r1_count_draws);
    const u64 count_records = take(g_r1_count_max_records);
    return fmt::format("; R1: memory-word fallbacks {} draws ({} records), per-draw multi "
                       "unfixable {}, pipelines reading the words {}; LDC.64 replaced {} reads "
                       "({} pipelines); count draws {} (max {} records), multi copy-free {}, "
                       "per-draw multi waiting {}, no count draw {}",
                       fallback_draws, fallback_records, unfixable, flagged, ldc64_reads,
                       ldc64_pipelines, count_draws, count_records, multi_copy_free,
                       multi_per_draw_waiting, multi_no_count);
}

} // namespace VideoCommon::MT::GpuParam
