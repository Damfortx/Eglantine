// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Multithreaded GPU back, phase 4 (docs/mt-plan.md, docs/mt-phase4-spec.md): the split's back
// thread becomes the sequencer (engines, guest memory, captures) and a committer thread owns the
// Vulkan scheduler and records. Included by vk_rasterizer.cpp only.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <limits>
#include <unordered_map>
#include <boost/container/small_vector.hpp>
#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu_split.h"
#include "video_core/mt/thread_exit.h"
#include "video_core/mt/capture.h"
#include "video_core/mt/draw_packet.h"
#include "video_core/mt/engine_ptr.h"
#include "video_core/mt/epoch.h"
#include "video_core/mt/maxwell3d_reg_names.h"
#include "video_core/mt/lite_switches.h"
#include "video_core/mt/lite2_capture_switches.h"
#include "video_core/mt/lite2_tracker_switches.h"
#include "video_core/mt/mt_switches.h"
#include "video_core/mt/perf_switches.h"
#include "video_core/mt/packet_compact.h"
#include "video_core/mt/reg_delta.h"
#include "video_core/mt/resolve_packet.h"
#include "video_core/mt/resolve_sync.h"
#include "video_core/mt/role.h"
#include "video_core/mt/spmc_stream.h"
#include "video_core/mt/tracker_lock.h"
#include "video_core/mt/wait_watch.h"
#include "video_core/mt/xlate_cache.h"
#include "video_core/cache_types.h"
#include "video_core/dirty_flags.h"
#include "video_core/nx2.h"
#include "video_core/shader_handle_program.h"
#include "video_core/textures/texture.h"
#include "video_core/texture_cache/util.h"
#include "shader_recompiler/exception.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_mt_resolve_types.h"

namespace Vulkan {

class RasterizerVulkan;

namespace MT {

class Resolver;
using VideoCommon::MT::StreamEntry;

/// What a Serial or Command entry is, for the statistics.
enum class Kind : u32 {
    External, // RunOnCommitter / PostToCommitter from outside the rasterizer
    Draw,
    DrawIndirect,
    DrawTexture,
    Clear,
    Dispatch,
    FlushRegion,
    InvalidateRegion,
    InvalidateGPUCache,
    Unmap,
    Channel,
    ConditionalRendering,
    Blit,
    Dma,
    InlineToMemory,
    Pipeline,
    Query,
    Fence,
    Barrier,
    Uniform,
    Other,
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Kind::Count)> KindNames{
    "external",  "draw",       "draw-indirect", "draw-texture", "clear",   "dispatch",
    "flush",     "invalidate", "gpu-cache",     "unmap",        "channel", "cond-render",
    "blit",      "dma",        "inline",        "pipeline",     "query",   "fence",
    "barrier",   "uniform",    "other"};

/// Counters (relaxed atomics: written by both threads, read by the logger).
struct Counters {
    std::array<std::atomic<u64>, static_cast<size_t>(Kind::Count)> serials{};
    std::array<std::atomic<u64>, static_cast<size_t>(Kind::Count)> commands{};
    std::atomic<u64> packets{};
    std::atomic<u64> compute_packets{};
    std::atomic<u64> compute_failures{};
    std::atomic<u64> packet_words{};
    std::atomic<u64> marks{};
    std::atomic<u64> settled_shortcuts{};
    std::atomic<u64> wait_committed{};
    std::atomic<u64> write_barriers{};
    std::atomic<u64> delta_words{};
    std::atomic<u64> serial_wait_ns{};
    std::atomic<u64> verify_checks{};
    std::atomic<u64> verify_mismatches{};
    std::atomic<u64> verify_cpu_writes{};
    std::atomic<u64> seq_mutex_writes{};
    std::atomic<u64> dma_serial{};
    std::atomic<u64> ubo_gen_hits{};
    std::atomic<u64> ubo_gen_misses{};
    std::atomic<u64> ubo_gen_unprotected{};
    // EDEN_NX2_MT_FLUSHCMD (vk_mt_seq.h).
    std::atomic<u64> flush_skipped{};      ///< FlushRegion on the sequencer: nothing to flush
    std::atomic<u64> flush_commands{};     ///< ... queued (the front drains for it)
    std::atomic<u64> flush_serials{};      ///< ... waited for (Serial)
    std::atomic<u64> inval_sequencer{};    ///< InvalidateRegion handled on the sequencer
    std::atomic<u64> inval_tex_commands{}; ///< ... with a texture cache Command
    std::atomic<u64> inval_owner_serials{}; ///< SeqInvalidateBuffers' Serial (GPU data pages)
    // Draw packet bytes by part (EDEN_NX2_MT_STATS), and the EDEN_NX2_MT_COMPACT decisions.
    std::atomic<u64> pkt_header_bytes{};
    std::atomic<u64> pkt_overlay_bytes{}; ///< entries, bytes, slot references and inserts
    std::atomic<u64> pkt_ubo_entry_bytes{};
    std::atomic<u64> pkt_ubo_data_bytes{};
    std::atomic<u64> pkt_upload_bytes{}; ///< entries and blob bytes (not heap ones)
    std::atomic<u64> pkt_other_bytes{};  ///< ssbo/geometry/tic-valid arrays, inline indices, padding
    std::atomic<u64> compact_packets{};
    std::atomic<u64> ubo_same{};
    std::atomic<u64> ubo_patched{};
    std::atomic<u64> ubo_full{};
    std::atomic<u64> desc_hits{};
    std::atomic<u64> desc_misses{};
    std::atomic<u64> overlay_memo_hits{}; ///< EDEN_NX2_MT_LITE_CAPMEMO
    // Stage times (EDEN_NX2_MT_STATS), ns.
    std::atomic<u64> seq_capture_ns{};
    std::atomic<u64> wait_committed_ns{};
    std::atomic<u64> committer_idle_ns{};
    std::array<std::atomic<u64>, 16> forced_serial{};
    // Phase 5.
    std::atomic<u64> ring_waits{};
    std::atomic<u64> batches{};
    std::atomic<u64> commit_wait_ns{};
    std::atomic<u64> resolved_ok{};      ///< results that validated
    std::atomic<u64> resolved_slow{};    ///< results the resolver gave up on
    std::atomic<u64> resolved_missing{}; ///< draws without a result (wait aborted, overrun)
    std::array<std::atomic<u64>, ValidateFailCount> validate_fail{};
    std::atomic<u64> shadow_checks{};
    std::atomic<u64> shadow_mismatches{};
    std::atomic<u64> shadow_unvalidated{};
    std::atomic<u64> barriers{};
    std::atomic<u64> resolved_committed{};
    std::atomic<u64> resolved_stolen{}; ///< EDEN_NX2_MT_STEAL_US: recorded without the result
    std::atomic<u64> resolved_buffers{}; ///< EDEN_NX2_MT_RBUF: committed with the buffer half too
    std::atomic<u64> uniform_id_mismatches{};
};

inline constexpr std::array<const char*, 16> ForcedSerialNames{
    "debug-serial", "transform-feedback", "texture-buffers", "handle-program",
    "multi-range-ssbo", "draw-capture", "too-big", "plan", "mesh", "no-index-range",
    "vertex-range", "ssbo-count", "indirect/collision", "", "", ""};

} // namespace MT

/// The committer pipeline (one per rasterizer, created when EDEN_NX2_MT is on).
class Nx2DrawPipeline final : public VideoCommon::MtBackInterface {
    using Maxwell3D = Tegra::Engines::Maxwell3D;
    using KeplerCompute = Tegra::Engines::KeplerCompute;
    using SpmcStream = VideoCommon::MT::SpmcStream;

public:
    explicit Nx2DrawPipeline(RasterizerVulkan& rasterizer_) : rasterizer{rasterizer_} {}

    ~Nx2DrawPipeline() override;

    Nx2DrawPipeline(const Nx2DrawPipeline&) = delete;
    Nx2DrawPipeline& operator=(const Nx2DrawPipeline&) = delete;

    // ---- MtBackInterface ----

    void Start() override;
    void Stop() override;
    void SeqMark(u64 position) override;
    void RequestHook() override {
        hook_requested.store(true, std::memory_order_seq_cst);
        stream.WakeAll();
    }
    [[nodiscard]] bool Quiet() const override {
        return committer_quiet.load(std::memory_order_acquire);
    }

    // ---- sequencer ----

    /// Runs `func` on the committer once everything emitted before is committed, with the
    /// sequencer waiting: the old code against the real engines. Returns its result.
    template <typename Func>
    decltype(auto) Serial(MT::Kind kind, Func&& func) {
        using Result = std::invoke_result_t<Func&>;
        struct Context {
            Func* func;
            std::conditional_t<std::is_void_v<Result>, char,
                               std::optional<std::remove_reference_t<Result>>>
                result;
            std::exception_ptr error;
        } context{&func, {}, {}};
        SerialRaw(
            kind,
            [](void* arg) {
                auto& ctx = *static_cast<Context*>(arg);
                try {
                    if constexpr (std::is_void_v<Result>) {
                        (*ctx.func)();
                    } else {
                        ctx.result.emplace((*ctx.func)());
                    }
                } catch (...) {
                    ctx.error = std::current_exception();
                }
            },
            &context);
        if (context.error) {
            std::rethrow_exception(context.error);
        }
        if constexpr (!std::is_void_v<Result>) {
            return Result(std::move(*context.result));
        }
    }

    /// Queues `func` (trivially copyable) to run on the committer in stream order.
    template <typename Func>
    void Command(MT::Kind kind, const Func& func) {
        static_assert(std::is_trivially_copyable_v<Func> && alignof(Func) <= alignof(u64));
        constexpr u32 functor_words = static_cast<u32>((sizeof(Func) + 7) / 8);
        // A uniform bind only records the binding in the committer's channel state (no cache
        // structure, no wait on the GPU or the front): it may sit inside a batch. Closing on it
        // too made batches 1.1 draws long in MKW (a bind before nearly every draw).
        const bool in_batch = StaysInBatch(kind);
        if (!in_batch) {
            CloseBatch();
        }
        EmitDeltas();
        u64* const out = stream.Reserve(functor_words + 3);
        out[0] = SpmcStream::MakeHeader(static_cast<u8>(MT::StreamEntry::Command), functor_words + 3,
                                        static_cast<u32>(kind));
        out[1] = reinterpret_cast<u64>(&CommandThunk<Func>);
        out[2] = 0;
        std::memcpy(out + 3, &func, sizeof(Func));
        stream.Commit();
        if (!in_batch) {
            stream.Flush(); // EDEN_NX2_MT_LAZY_WAKE: a sleeping committer runs it now
        }
        if (WritesGuestMemory(kind)) {
            // EDEN_NX2_MT_DMA_CONSUMED: front reads after this entry wait for the committer.
            // EDEN_NX2_MT_LITE_WRITERS: semaphore/report words and fences are not bulk writes.
            VideoCommon::GpuSplit::Get().NoteCommitterWrite(kind != MT::Kind::Query &&
                                                            kind != MT::Kind::Fence);
        }
        Count(counters.commands, kind);
        NoteAsyncWork(kind);
        if (lockstep) {
            WaitCommitted();
        }
    }

    /// Commands that may sit inside an open batch: they only record a binding or work the
    /// committer runs in stream order anyway (EDEN_NX2_MT_CMD_IN_BATCH for the latter).
    [[nodiscard]] static bool StaysInBatch(MT::Kind kind) {
        switch (kind) {
        case MT::Kind::Uniform:
            return true;
        case MT::Kind::Query:
        case MT::Kind::Fence:
        case MT::Kind::Barrier:
        case MT::Kind::Clear:
        case MT::Kind::ConditionalRendering:
            return VideoCommon::MT::CommandsInBatch();
        default:
            return false;
        }
    }

    /// Commands whose committer side may write guest memory the front reads (conservative: all
    /// but those that only record or invalidate).
    [[nodiscard]] static bool WritesGuestMemory(MT::Kind kind) {
        switch (kind) {
        case MT::Kind::Uniform:
        case MT::Kind::Barrier:
        case MT::Kind::Clear:
        case MT::Kind::ConditionalRendering:
        case MT::Kind::InvalidateRegion:
        case MT::Kind::InvalidateGPUCache:
            return false;
        default:
            return true;
        }
    }

    /// Before the sequencer writes guest memory over pages images or command lists may use.
    void BeforeWrite(u64 device_addr, u64 size);

    /// Waits until the committer consumed everything emitted so far.
    void WaitCommitted();

    /// Sends the registers written since the last call (and the binds kept outside them).
    void EmitDeltas();

    using IndirectParams = Maxwell3D::DrawManager::IndirectParams;
    /// RasterizerVulkan::Draw on the sequencer: capture and emit a DrawPacket (or a Serial).
    /// `indirect`: DrawIndirect's parameters (EDEN_NX2_MT_INDIRECT).
    void SeqDraw(bool is_indexed, u32 instance_count, const IndirectParams* indirect = nullptr);

    // Phase 7 (vk_mt_seq.h): fewer Serial items.
    /// AccelerateConditionalRendering: a Command when the answer is known here (EDEN_NX2_MT_COND).
    bool SeqConditionalRendering();
    /// DrawIndirect as a draw packet (EDEN_NX2_MT_INDIRECT).
    void SeqDrawIndirect();
    /// DrawTexture as a Command with its TIC/TSC entries (EDEN_NX2_MT_DRAWTEX).
    void SeqDrawTexture();
    /// AccelerateDMA::BufferCopy/BufferClear as Commands (EDEN_NX2_MT_DMA=packet).
    bool SeqDmaCopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount);
    bool SeqDmaClear(GPUVAddr dst_address, u64 amount, u32 value);
    /// RasterizerVulkan::DispatchCompute on the sequencer: Serial, or a ComputePacket with
    /// EDEN_NX2_MT_COMPUTE=packet.
    void SeqDispatch();
    /// RasterizerVulkan::FlushRegion on the sequencer (EDEN_NX2_MT_FLUSHCMD): nothing when no
    /// cache can hold GPU data to download there, a Command when `may_queue` and the front
    /// drains for it (t_flush_async), else a Serial.
    void SeqFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which, bool may_queue);
    /// RasterizerVulkan::InvalidateRegion on the sequencer (EDEN_NX2_MT_FLUSHCMD): the buffer
    /// tracker and shader cache here, the texture and query caches as ordered Commands, a Serial
    /// only for pages holding GPU-written buffer data (SeqInvalidateBuffers).
    void SeqInvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which);
    /// RasterizerVulkan::InnerInvalidation on the sequencer (FlushCaching).
    void SeqInnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences);

    /// The buffer cache part of an invalidation on the sequencer.
    void SeqInvalidateBuffers(std::span<const std::pair<DAddr, std::size_t>> regions);
    /// RasterizerVulkan::OnCacheInvalidation(s) on the sequencer (guest CPU writes gathered at
    /// a fence): images only once the committer recorded everything before.
    void SeqCacheInvalidations(std::span<const std::pair<DAddr, std::size_t>> regions);

    /// RasterizerVulkan::AccelerateInlineToMemory on the sequencer: the write and its deferred
    /// invalidation here when it lands outside GPU-written buffer data; false to run it Serial.
    bool SeqInlineToMemory(GPUVAddr address, std::optional<DAddr> cpu_addr, size_t copy_size,
                           std::span<const u8> memory);

    void ForceSerialReason(u32 reason) {
        counters.forced_serial[reason].fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] bool Running() const noexcept {
        return running;
    }

    // ---- committer ----

    /// The committer's mirror of the bound channel's 3D engine.
    [[nodiscard]] Maxwell3D* Mirror3D() const noexcept {
        return current != nullptr ? current->mirror3d.get() : nullptr;
    }

    MT::Counters counters;

private:
    friend class MT::Resolver;

    struct ChannelMirrors {
        Maxwell3D* real3d{};
        KeplerCompute* real_compute{};
        std::unique_ptr<Maxwell3D> mirror3d;
        std::unique_ptr<KeplerCompute> mirror_compute;
    };

    /// The draw state a packet carries (DrawManager::State without its vector).
    struct DrawStateCopy {
        Maxwell3D::Regs::PrimitiveTopology topology;
        Maxwell3D::DrawManager::DrawMode draw_mode;
        bool draw_indexed;
        u32 draw_element_count;
        u32 base_index;
        Maxwell3D::Regs::VertexBuffer vertex_buffer;
        Maxwell3D::Regs::IndexBuffer index_buffer;
        u32 base_instance;
        u32 instance_count;
        u32 drawn_instances;
    };
    struct DrawHeader {
        u64 seq;
        GraphicsPipeline* pipeline;
        DrawStateCopy draw_state;
        IndirectParams indirect_params;
        u32 instance_count;
        u8 is_indexed;
        u8 native_turing;
        u8 indirect;
        u8 pad;
        u32 inline_index_offset;
        u32 inline_index_size;
        u32 num_overlay;
        u32 num_ubo;
        u32 num_upload;
        u32 blob_size;
        // v2 (phase 5): resolver inputs.
        u64 batch;
        u64 layout_gen;
        u32 num_ssbo;
        u32 num_geometry;
        u32 tic_valid_words;
        u32 pad2;
    };
    static constexpr u32 DrawHeaderWords = static_cast<u32>((sizeof(DrawHeader) + 7) / 8);
    /// EDEN_NX2_MT_COMPACT draw packets (stream header aux 1): this header, the indirect
    /// parameters only for indirect draws, the arrays of the v2 layout, then the TIC/TSC slot
    /// references (u16, padded to 8 bytes) and inserts (u64 addresses), then the blob.
    /// ParsePacket rebuilds the DrawHeader and the overlay the old layout carried.
    struct CompactDrawHeader {
        u64 seq;
        GraphicsPipeline* pipeline;
        u64 batch;
        u64 layout_gen;
        u32 blob_size;
        u32 instance_count;
        u32 num_overlay;
        u32 num_upload;
        u16 num_desc_refs;
        u16 num_desc_inserts;
        u8 num_ubo;
        u8 num_ssbo;
        u8 num_geometry;
        u8 tic_valid_words;
        u8 flags;
        u8 draw_mode;
        u16 pad;
        u32 topology;
        u32 inline_index_offset;
        u32 inline_index_size;
        u32 draw_element_count;
        u32 base_index;
        Maxwell3D::Regs::VertexBuffer vertex_buffer;
        Maxwell3D::Regs::IndexBuffer index_buffer;
        u32 base_instance;
        u32 draw_instance_count;
        u32 drawn_instances;
        enum Flags : u8 {
            IsIndexed = 1 << 0,
            NativeTuring = 1 << 1,
            Indirect = 1 << 2,
            DrawIndexed = 1 << 3,
        };
    };
    static constexpr u32 CompactHeaderWords =
        static_cast<u32>((sizeof(CompactDrawHeader) + 7) / 8);
    static constexpr u32 IndirectWords = static_cast<u32>((sizeof(IndirectParams) + 7) / 8);
    static constexpr u32 CompactAux = 1;
    /// A draw packet's batch (resolvers pick their packets by it), either layout.
    [[nodiscard]] static u64 PacketBatch(const u64* entry) noexcept {
        u64 batch;
        const size_t offset = SpmcStream::HeaderAux(entry[0]) == CompactAux
                                  ? offsetof(CompactDrawHeader, batch)
                                  : offsetof(DrawHeader, batch);
        std::memcpy(&batch, reinterpret_cast<const u8*>(entry + 1) + offset, sizeof(batch));
        return batch;
    }
    /// A reader that does not parse a draw packet (a resolver, other batches) must still apply
    /// its TIC/TSC inserts to its replica.
    static void SkipPacket(const u64* entry, VideoCommon::MT::PacketDecoder& decoder);
    /// Resolved draw slots (by seq), see vk_mt_resolve.h.
    static constexpr u32 ResolvedSlots = 4096;

    /// A draw packet's arrays, as both the committer and the resolvers read them.
    struct PacketView {
        DrawHeader header;
        std::span<const VideoCommon::MT::OverlayEntry> overlay;
        std::span<const VideoCommon::MT::UboEntry> ubos;
        std::span<const VideoCommon::MT::UploadEntry> uploads;
        std::span<const VideoCommon::MT::SsboEntry> ssbos;
        std::span<const VideoCommon::MT::GeometryEntry> geometry;
        std::span<const u64> tic_valid;
        const u8* blob;
        /// The overlay's bytes: `blob` (old layout), or the decoder's (compact).
        const u8* overlay_blob;
    };
    /// Both layouts. `decoder`: the calling reader's own (compact packets update its replica,
    /// and the overlay spans point into it until its next call).
    static PacketView ParsePacket(const u64* entry, VideoCommon::MT::PacketDecoder& decoder);

    /// A guest read the committer will repeat (its bytes go into the overlay).
    struct RawRead {
        u64 gpu_addr;
        u32 size;
        u32 offset;
        bool descriptor; ///< a TIC/TSC entry (SeqReadDescriptor)
    };

    /// Reads guest memory for the capture and records it.
    template <typename T>
    T SeqRead(u64 gpu_addr);
    /// SeqRead as a memory manager (EvaluateTextureHandle).
    struct SeqRecorder {
        Nx2DrawPipeline* self;
        template <typename T>
        T Read(u64 gpu_addr) const {
            return self->SeqRead<T>(gpu_addr);
        }
    };
    void SeqReadDescriptor(u64 gpu_addr);
    /// A streamed uniform buffer's bytes into the blob, or (EDEN_NX2_MT_UBO_GEN) a reference to
    /// the committer's copy while its pages are unchanged (vk_mt_seq.h). Tracker lock held.
    void SeqStageUniform(VideoCommon::MT::UboEntry& entry);
    /// Committer: keeps the bytes of memo entries before anything else reads the packet.
    void KeepUniformMemos(std::span<const VideoCommon::MT::UboEntry> ubos, const u8* blob);
    /// Copies the CPU-modified parts of a range the draw binds into the packet.
    void SeqTakeUploads(DAddr device_addr, u32 size);
    /// EDEN_NX2_MT_LITE_SEQLOCK: SeqTakeUploads from here on records its pieces only; their
    /// bytes are read by SeqReadDeferredUploads, after the tracker lock is released.
    void SeqDeferUploadReads() {
        defer_upload_reads = lite_seq_lock;
        deferred_uploads_first = uploads.size();
    }
    void SeqReadDeferredUploads();
    /// False when no page of the range was ever GPU-written (EDEN_NX2_MT_LITE_SEQLOCK), so
    /// MtPageHoldsGpuData is false without its lookup; true otherwise.
    bool SeqPagesMayHoldGpuData(DAddr device_addr, u64 size) const;
    /// GpuToCpuAddress for captures (EDEN_NX2_MT_LITE_XLATE: through seq_xlate).
    std::optional<DAddr> SeqTranslate(GPUVAddr gpu_addr);
    /// -1, or why this draw goes as a Serial (MT::ForcedSerialNames).
    int CaptureDraw(GraphicsPipeline* pipeline, bool is_indexed, const IndirectParams* indirect);
    /// The captured reads, sorted and merged, into the overlay (bytes in the blob).
    void FinishOverlay();
    /// EDEN_NX2_MT_COMPACT: FinishOverlay, but TIC/TSC entries the replicas hold unchanged
    /// become slot references (desc_refs), and the others replace their slot (desc_inserts).
    void FinishOverlayCompact();
    /// FinishOverlayCompact once raw_reads are sorted and desc_eligible is set.
    void FinishOverlayCompactSlots();
    void ClearCapture();
    struct ComputeHeader {
        u64 seq;
        ComputePipeline* pipeline;
        u64 native_key;
        KeplerCompute::LaunchParams qmd;
        KeplerCompute::MtDispatchState state;
        u32 num_overlay;
        u32 num_ubo;
        u32 num_upload;
        u32 blob_size;
    };
    static constexpr u32 ComputeHeaderWords = static_cast<u32>((sizeof(ComputeHeader) + 7) / 8);
    int CaptureCompute(ComputePipeline* pipeline);
    void EmitCompute(ComputePipeline* pipeline, u64 native_key);
    void CommitCompute(const u64* entry);
    /// `overlay_blob`: the overlay's bytes (null: `blob`).
    void VerifyCaptures(u64 seq, std::span<const VideoCommon::MT::OverlayEntry> overlay,
                        std::span<const VideoCommon::MT::UboEntry> ubos,
                        std::span<const VideoCommon::MT::UploadEntry> uploads, const u8* blob,
                        const u8* overlay_blob = nullptr);
    void ReportView(u64 seq, const VideoCommon::MT::CaptureView& view);
    void EmitDraw(GraphicsPipeline* pipeline, bool is_indexed, u32 instance_count,
                  const IndirectParams* indirect);
    /// The CPU-modified parts of [device_addr, +size) as heap uploads for a Command (phase 7).
    struct HeapUploads {
        std::vector<VideoCommon::MT::UploadEntry> uploads;
        std::vector<u8> blob;
    };
    [[nodiscard]] HeapUploads* TakeHeapUploads(std::span<const std::pair<DAddr, u32>> ranges);
    void ApplyHeapUploads(HeapUploads* heap_uploads);
    void CommitDraw(const u64* entry);
    void VerifyDraw(const DrawHeader& header, std::span<const VideoCommon::MT::OverlayEntry> overlay,
                    std::span<const VideoCommon::MT::UboEntry> ubos,
                    std::span<const VideoCommon::MT::UploadEntry> uploads, const u8* blob,
                    const u8* overlay_blob);
    void VerifyReport(u64 key, const std::string& message);

    template <typename Func>
    static void CommandThunk(Nx2DrawPipeline&, const u64* functor) {
        (*reinterpret_cast<const Func*>(functor))();
    }

    static constexpr size_t KindNamesCount() {
        return MT::KindNames.size();
    }
    static const char* KindName(u32 kind) {
        return kind < MT::KindNames.size() ? MT::KindNames[kind] : "?";
    }

    static void Count(std::array<std::atomic<u64>, static_cast<size_t>(MT::Kind::Count)>& array,
                      MT::Kind kind) {
        array[static_cast<size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
    }

    void SerialRaw(MT::Kind kind, void (*fn)(void*), void* arg);

    /// Sequencer: ends the open batch (resolvers publish at its BatchEnd). Called before every
    /// stream entry that is not a draw or register state, so a batch never stays open while the
    /// sequencer waits or idles.
    void CloseBatch();
    /// Whether draws are grouped into batches for resolvers.
    [[nodiscard]] bool Batching() const noexcept {
        return resolver_count != 0;
    }

    void CommitterLoop(std::stop_token stop);
    /// Applies a Deltas, ComputeDeltas, CbBind or RootTable entry to mirrors (committer and
    /// resolvers). False for other kinds.
    static bool ApplyMirrorEntry(Maxwell3D* mirror3d, KeplerCompute* mirror_compute,
                                 const u64* entry);
    /// Committer: the resolver result of a draw packet (waits for its batch), or null.
    MT::ResolvedDraw* WaitResolved(const DrawHeader& header);
    /// Committer: I4, under both cache locks.
    MT::ValidateFail Validate(const MT::ResolvedDraw& res, const DrawHeader& header) const;
    /// Committer: EDEN_NX2_MT_SHADOW comparison after the phase-4 path recorded the draw.
    void ShadowCompare(const MT::ResolvedDraw& res, const DrawHeader& header,
                       const GraphicsPipeline::MtShadowSink& sink);
    void StartResolvers();
    void StopResolvers();
    /// Committer: a validated result as ConfigureImpl's texture half, with the views' intents
    /// (LRU touch, blacklist rating, storage writes) applied.
    void PrepareResolvedTextures(const MT::ResolvedDraw& res,
                                 GraphicsPipeline::MtResolvedTextures& out);
    /// Committer (EDEN_NX2_MT_RALIAS, vk_mt_resolve.h): before Validate, SynchronizeAliases for
    /// the result's views marked `alias` (texture mutex held inside). A rescale it causes bumps
    /// the image version, so Validate then sends the draw down the slow path.
    void SyncResolvedAliases(const MT::ResolvedDraw& res);
    /// Committer (EDEN_NX2_MT_RBUF), inside ConfigureResolved: the buffer intents of a result.
    void ApplyResolvedBuffers(const MT::ResolvedDraw& res, bool is_indexed);
    void RunEntry(const u64* entry);
    /// Committer (EDEN_NX2_MT_UBIND_FOLD): a CbBind entry's binds into the buffer cache.
    void ApplyUniformBinds(const u64* binds, u32 count);
    void RunSerial(const u64* entry);
    void Idle(std::stop_token& stop);

    /// Committer: the mirrors of the rasterizer's bound channel (created from the real engines
    /// while the sequencer waits), installed as this thread's engine overrides.
    void BindCurrentChannel();
    ChannelMirrors* FindMirrors(const Maxwell3D* real3d);
    void VerifyMirror(const char* where);
    void LogStats();
    void LogResolverStats(double seconds);

    RasterizerVulkan& rasterizer;
    SpmcStream stream{size_t{1} << 21}; // 16 MiB
    u32 consumer{};
    std::jthread committer;
    bool running = false;
    const bool lockstep = VideoCommon::MT::Lockstep() || VideoCommon::MT::Verify();
    const bool verify = VideoCommon::MT::Verify();
    // EDEN_NX2_MT_LITE pieces (lite_switches.h, docs/mt-lite-plan.md).
    const bool lite_loop = VideoCommon::MT::LiteCommitLoop();
    const bool lite_quiet = VideoCommon::MT::LiteQuiet();
    const bool lite_no_tic_valid = VideoCommon::MT::LiteNoTicValid();
    // Iteration 2, capture (lite2_capture_switches.h).
    const bool lite_seq_lock = VideoCommon::MT::LiteSeqOneLock();
    const bool lite_cap_memo = VideoCommon::MT::LiteCaptureMemo();
    const bool lite_xlate = VideoCommon::MT::LiteXlateCache();

    // Sequencer-owned.
    std::vector<u64> delta_scratch;
    u64 next_seq = 1;
    u64 batch_id = 0;
    u32 batch_draws = 0;
    bool batch_open = false;
    std::vector<VideoCommon::MT::SsboEntry> ssbo_entries;
    std::vector<VideoCommon::MT::GeometryEntry> geometry_entries;
    std::vector<u64> tic_valid;
    u32 tic_valid_count = 0;
    /// IsValidEntry memo by TIC bytes and GPU page table layout.
    struct TicValidMemo {
        std::array<u64, 4> raw{};
        u64 layout_gen = 0;
        const Tegra::MemoryManager* manager = nullptr; ///< channels have their own generations
        bool valid = false;
    };
    std::array<TicValidMemo, 256> tic_valid_memo{};
    bool SeqTicValid(const u8* tic_bytes);
    /// EDEN_NX2_MT_CONT_MEMO: MemoryManager::IsContinuousRange memo by (address, size) and GPU
    /// page table layout generation.
    struct ContinuousMemo {
        u64 gpu_addr = 0;
        u64 size = 0;
        u64 layout_gen = 0;
        const Tegra::MemoryManager* manager = nullptr; ///< channels have their own generations
        bool continuous = false;
    };
    std::array<ContinuousMemo, 128> continuous_memo{};
    bool SeqIsContinuous(u64 gpu_addr, u64 size);
    std::vector<RawRead> raw_reads;
    std::vector<u8> raw_bytes;
    std::vector<VideoCommon::MT::OverlayEntry> overlay;
    std::vector<VideoCommon::MT::UboEntry> ubos;
    std::vector<VideoCommon::MT::UploadEntry> uploads;
    std::vector<u8> blob;
    std::vector<std::pair<DAddr, u32>> taken_ranges;
    bool defer_upload_reads = false;
    size_t deferred_uploads_first = 0;
    VideoCommon::MT::TranslationCache seq_xlate;
    /// EDEN_NX2_MT_LITE_CAPMEMO: FinishOverlayCompact's structural work for a list of reads (in
    /// capture order): the sorted list and the slot eligibility of each sorted read.
    struct OverlayMemo {
        u64 hash = 0;
        std::vector<RawRead> reads;
        std::vector<RawRead> sorted;
        std::vector<u8> eligible;
    };
    std::array<OverlayMemo, 64> overlay_memo{};
    std::vector<u8> dma_scratch;
    struct UniformMemo {
        DAddr device_addr;
        u32 size;
        u64 generation; ///< 0: none
    };
    std::array<std::array<UniformMemo, VideoCommon::MT::CaptureView::UboMemoSlots>,
               VideoCommon::MT::CaptureView::UboMemoStages>
        seq_ubo_memo{};
    // EDEN_NX2_MT_COMPACT (sequencer).
    const bool compact = VideoCommon::MT::Compact();
    /// This capture goes out in the compact layout (its counts fit; set by CaptureDraw).
    bool compact_packet = false;
    /// The bytes the committer keeps per uniform slot (ubo_memo_bytes), as of the stream's end.
    std::unique_ptr<u8[]> seq_ubo_bytes;
    std::array<u8, VideoCommon::MT::CaptureView::UboMemoBytes> ubo_scratch{};
    /// The TIC/TSC table the readers replicate.
    VideoCommon::MT::DescriptorReplica seq_desc;
    std::vector<u16> desc_refs;
    std::vector<u64> desc_inserts;
    std::vector<u8> desc_eligible;
    /// Blob bytes of the capture by part (EDEN_NX2_MT_STATS).
    u32 blob_ubo_bytes = 0;
    u32 blob_upload_bytes = 0;
    u64 last_query_position = 0;
    /// EDEN_NX2_MT_FLUSHCMD: stream end after the last entry the committer runs later that may
    /// add GPU-written data the sequencer can't see yet. Buffers: Commands other than the
    /// kinds below (draw and compute packets mark their written ranges at capture). Images:
    /// those and every draw/compute packet (render targets and storage images are registered
    /// and marked GPU-modified at commit). Serial items are done when they return.
    u64 seq_async_buffer_position = 0;
    u64 seq_async_image_position = 0;
    void NoteAsyncWork(MT::Kind kind) {
        switch (kind) {
        case MT::Kind::Uniform:          // records a binding
        case MT::Kind::Query:            // query cache (guarded by last_query_position)
        case MT::Kind::Barrier:          // barriers, flushes of the scheduler
        case MT::Kind::FlushRegion:      // downloads (only clear GPU-modified state)
        case MT::Kind::InvalidateRegion: // marks images CPU-modified
            return;
        default:
            seq_async_buffer_position = stream.WritePosition();
            seq_async_image_position = seq_async_buffer_position;
            return;
        }
    }
    void NoteAsyncImageWork() {
        seq_async_image_position = stream.WritePosition();
    }

public:
    /// Sequencer: a query report was just emitted.
    void NoteQueryEmitted() {
        last_query_position = stream.WritePosition();
    }

private:
    std::chrono::steady_clock::time_point last_stats{std::chrono::steady_clock::now()};

    /// Reclamation for the caches' slots and twin maps (EDEN_NX2_MT_EBR): retired by the
    /// committer, collected by the committer, read under guards by the resolvers.
    VideoCommon::MT::EpochManager epochs;
    bool epochs_attached = false;
    u32 collect_countdown = 0;
    void AttachEpochs(bool attach);
    /// Committer: run reclaim callbacks whose grace period passed.
    void CollectEpochs() {
        // EDEN_NX2_MT_EBR_LAZY: nothing retired, no locks (a retire racing with this check,
        // e.g. a resolver's sampler map growth, is collected at the next call).
        if (epochs_attached &&
            (!VideoCommon::MT::LazyEpochCollect() || epochs.MaybePending())) {
            std::scoped_lock lock{rasterizer.buffer_cache.mutex, rasterizer.texture_cache.mutex};
            epochs.Collect();
        }
    }

    // Committer-owned.
    std::unique_ptr<u8[]> ubo_memo_bytes;
    /// EDEN_NX2_MT_COMPACT: the committer's TIC/TSC replica and decoded overlay.
    VideoCommon::MT::PacketDecoder commit_decoder;
    std::vector<std::unique_ptr<ChannelMirrors>> mirrors;
    ChannelMirrors* current{};
    u64 last_mark{};

    // Shared.
    /// Resolver threads running (0: phase 4, the committer resolves every draw).
    u32 resolver_count = 0;
    std::vector<std::unique_ptr<MT::Resolver>> resolvers;
    VideoCommon::MT::SeqSlotRing<MT::ResolvedDraw, ResolvedSlots> resolved;
    /// Bumped by the committer at each channel bind; resolvers copy it at the barrier.
    std::atomic<u32> channel_gen{0};
    const u32 shadow = VideoCommon::MT::Shadow();
    const bool resolved_commit = VideoCommon::MT::ResolvedCommit();
    GraphicsPipeline::MtShadowSink shadow_sink;
    boost::container::small_vector<VideoCommon::ImageViewInOut, 160> commit_views;
    boost::container::small_vector<VideoCommon::SamplerId, 96> commit_samplers;
    /// Committer: the seq of the last committed draw packet (sequencer back-pressure).
    alignas(64) std::atomic<u64> committed_seq{0};
    /// Committer: 1 + the batch it waits for (EDEN_NX2_MT_ADAPTIVE: the sequencer closes it
    /// after its next draw).
    alignas(64) std::atomic<u64> wanted_batch{0};
    const bool adaptive_batches = VideoCommon::MT::AdaptiveBatches();
    const u32 steal_us = VideoCommon::MT::StealMicros();

    alignas(64) std::atomic<bool> hook_requested{};
    std::atomic<bool> committer_quiet{true};
    alignas(64) std::atomic<u32> serial_word{};
    alignas(64) std::atomic<u32> commit_word{};
    std::atomic<bool> commit_waiting{};
    std::atomic<bool> start_ready{};

    // Watchdog (EDEN_NX2_MT_STATS / EDEN_NX2_MT_WATCHDOG, vk_mt_resolve.h).
    VideoCommon::MT::WaitWatch watch;
    std::jthread watchdog;
    /// Sequencer mirrors for the watchdog's report: last emitted draw seq, next batch id.
    std::atomic<u64> emitted_seq{0};
    /// EDEN_NX2_MT_VERIFY (lockstep): GuestCpuWrites() when the sequencer began the capture of
    /// the draw the committer verifies.
    std::atomic<u64> verify_cpu_writes_since{0};
    std::atomic<u64> open_batch{0};
    void WatchdogLoop(std::stop_token stop);
    /// Logs the pipeline's positions (`why` first).
    void WatchdogReport(const std::string& why);
    /// Wakes every waiter of the pipeline (they all re-check their condition).
    void KickWaiters();
};

} // namespace Vulkan

// Resolver threads (phase 5); they use the declaration above.
#include "video_core/renderer_vulkan/vk_mt_resolve.h"

// ---------------------------------------------------------------------------------------------
// Implementation (needs RasterizerVulkan complete).

namespace Vulkan {

namespace MTDetail {
/// The sequencer's lock for the buffer tracker: tracker_mutex with EDEN_NX2_MT_TRAPLOCK, else
/// the buffer cache mutex (phase 4).
class SeqTrackerLock {
public:
    explicit SeqTrackerLock(BufferCache& buffer_cache)
        : mutex{VideoCommon::MT::TrapLock() ? &buffer_cache.MtTrackerMutex()
                                            : &buffer_cache.mutex} {
        mutex->lock();
    }
    ~SeqTrackerLock() {
        mutex->unlock();
    }
    SeqTrackerLock(const SeqTrackerLock&) = delete;
    SeqTrackerLock& operator=(const SeqTrackerLock&) = delete;

private:
    Common::SpinRecursiveMutex* mutex;
};

inline void Pause() {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}
} // namespace MTDetail

inline void Nx2DrawPipeline::Start() {
    if (running) {
        return;
    }
    consumer = stream.AddConsumer();
    stream.SetLazyWake(VideoCommon::MT::LazyWake());
    auto& hooks = VideoCommon::MT::g_hooks;
    hooks.context = this;
    hooks.run_serial = [](void* context, void (*fn)(void*), void* arg) {
        static_cast<Nx2DrawPipeline*>(context)->SerialRaw(MT::Kind::External, fn, arg);
    };
    hooks.post_command = [](void* context, void (*fn)(void*, void*), void* a, void* b) {
        struct Call {
            void (*fn)(void*, void*);
            void* a;
            void* b;
            void operator()() const {
                fn(a, b);
            }
        };
        static_cast<Nx2DrawPipeline*>(context)->Command(MT::Kind::External, Call{fn, a, b});
    };
    hooks.before_write = [](void* context, u64 device_addr, u64 size) {
        static_cast<Nx2DrawPipeline*>(context)->BeforeWrite(device_addr, size);
    };
    AttachEpochs(VideoCommon::MT::DeferredFree());
    if (verify) {
        VideoCommon::MT::g_cpu_write_log_on.store(true, std::memory_order_seq_cst);
    }
    if (VideoCommon::MT::UboGen() || compact) {
        using View = VideoCommon::MT::CaptureView;
        seq_ubo_memo = {};
        // Value-initialized (zero) on both sides: the committer's kept bytes and the
        // sequencer's copy of them start equal.
        ubo_memo_bytes = std::make_unique<u8[]>(View::UboMemoStages * View::UboMemoSlots *
                                                View::UboMemoBytes);
        if (compact) {
            seq_ubo_bytes = std::make_unique<u8[]>(View::UboMemoStages * View::UboMemoSlots *
                                                   View::UboMemoBytes);
        }
    }
    if (VideoCommon::MT::UboGen()) {
        VideoCommon::MT::g_ubo_gen_on.store(true, std::memory_order_seq_cst);
    }
    // The TIC/TSC table and the committer's replica (resolvers make theirs at StartResolvers).
    seq_desc.Reset();
    commit_decoder.table.Reset();
    start_ready.store(false, std::memory_order_relaxed);
    committer = std::jthread([this](std::stop_token stop) {
        VideoCommon::MT::RunLoudThread("GPUCommit", stop, [&] { CommitterLoop(stop); });
    });
    while (!start_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    StartResolvers();
    if (watch.on) {
        watchdog = std::jthread([this](std::stop_token stop) {
            VideoCommon::MT::RunLoudThread("MTWatchdog", stop, [&] { WatchdogLoop(stop); });
        });
    }
    running = true;
    VideoCommon::MT::g_write_hook_on.store(true, std::memory_order_seq_cst);
    VideoCommon::MT::g_active.store(true, std::memory_order_seq_cst);
    if (VideoCommon::MT::Check()) {
        VideoCommon::MT::g_check.store(true, std::memory_order_seq_cst);
        VideoCommon::GpuSplit::check_owner = true;
    }
    LOG_INFO(Render_Vulkan,
             "MT: committer started (lockstep {}, verify {}, compute {}, resolvers {}, shadow {})",
             lockstep, verify, VideoCommon::MT::ComputePackets() ? "packet" : "serial",
             resolver_count, shadow);
    LOG_INFO(Render_Vulkan,
             "MT lite: {} (ubo {}, loop {}, ticvalid {}, drain {}, quiet {}, writers {}, "
             "dmapartial {}, resume {}, wmark {}, ctrack {}, seqlock {}, capmemo {}, xlate {}; "
             "resolvers {})",
             VideoCommon::MT::Lite(), VideoCommon::MT::LiteUboFromCapture(), lite_loop,
             lite_no_tic_valid, VideoCommon::MT::LiteDrain(), lite_quiet,
             VideoCommon::MT::LiteWritersSettle(), VideoCommon::MT::LiteDmaPartial(),
             VideoCommon::MT::LiteAcquireNoDrain(), VideoCommon::MT::LiteWriteMark(),
             VideoCommon::MT::LiteCommitTracker(), lite_seq_lock, lite_cap_memo, lite_xlate,
             resolver_count);
}

inline void Nx2DrawPipeline::Stop() {
    if (!running) {
        return;
    }
    // The sequencer is stopped: this thread may produce. A batch left open would keep the
    // committer waiting for its resolver.
    CloseBatch();
    stream.Flush();
    // The committer drains first (it needs the resolvers' last batches), then the resolvers.
    committer.request_stop();
    stream.WakeAll();
    committer.join();
    // The watchdog reads the resolvers: it stops first.
    if (watchdog.joinable()) {
        watchdog.request_stop();
        watchdog.join();
    }
    StopResolvers();
    VideoCommon::MT::g_active.store(false, std::memory_order_seq_cst);
    VideoCommon::MT::g_check.store(false, std::memory_order_seq_cst);
    VideoCommon::MT::g_write_hook_on.store(false, std::memory_order_seq_cst);
    VideoCommon::MT::g_ubo_gen_on.store(false, std::memory_order_seq_cst);
    VideoCommon::MT::g_cpu_write_log_on.store(false, std::memory_order_seq_cst);
    VideoCommon::MT::g_hooks = {};
    stream.RemoveConsumer(consumer);
    AttachEpochs(false);
    running = false;
    LOG_INFO(Render_Vulkan, "MT: committer stopped");
}

inline void Nx2DrawPipeline::AttachEpochs(bool attach) {
    if (attach == epochs_attached) {
        return;
    }
    std::scoped_lock lock{rasterizer.buffer_cache.mutex, rasterizer.texture_cache.mutex};
    if (!attach) {
        // No reader is left: everything retired runs now, then erase destroys at once again.
        epochs.Collect();
    }
    epochs.SetAutoCollect(false);
    VideoCommon::MT::EpochManager* const manager = attach ? &epochs : nullptr;
    rasterizer.texture_cache.MtSetEpochManager(manager);
    rasterizer.buffer_cache.MtSetEpochManager(manager);
    epochs_attached = attach;
}

inline void Nx2DrawPipeline::SeqMark(u64 position) {
    if (VideoCommon::MT::Stats()) [[unlikely]] {
        LogStats();
    }
    CloseBatch();
    auto& split = VideoCommon::GpuSplit::Get();
    // Nothing of ours in flight: the position is settled already.
    if (stream.ConsumerPosition(consumer) == stream.WritePosition()) {
        split.StoreSettled(position);
        counters.settled_shortcuts.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 payload = position;
    stream.Push(static_cast<u8>(MT::StreamEntry::Mark), 0, &payload, 1);
    // The front may wait for this position, the sequencer may go idle: wake the consumers.
    stream.Flush();
    counters.marks.fetch_add(1, std::memory_order_relaxed);
}

inline void Nx2DrawPipeline::EmitDeltas() {
    Maxwell3D* const real = rasterizer.maxwell3d.Real();
    if (real != nullptr) {
        if (real->mt_written.Any()) {
            const u32 marked = real->mt_written.Count();
            u64* const out = stream.Reserve(marked + 1);
            const u32 count =
                real->mt_written.Drain(real->regs.reg_array.data(), Maxwell3D::Regs::NUM_REGS, out + 1);
            for (u32 i = count; i < marked; ++i) {
                out[1 + i] = 0;
            }
            out[0] = SpmcStream::MakeHeader(static_cast<u8>(MT::StreamEntry::Deltas), marked + 1, count);
            stream.Commit();
            counters.delta_words.fetch_add(count, std::memory_order_relaxed);
        }
        if (!real->mt_cb_binds.empty()) {
            const u32 count = static_cast<u32>(real->mt_cb_binds.size());
            static_assert(sizeof(VideoCommon::MT::CbBindDelta) == 16);
            u64* const out = stream.Reserve(count * 2 + 1);
            out[0] = SpmcStream::MakeHeader(static_cast<u8>(MT::StreamEntry::CbBind), count * 2 + 1, count);
            std::memcpy(out + 1, real->mt_cb_binds.data(), count * 16);
            stream.Commit();
            real->mt_cb_binds.clear();
        }
        if (!real->mt_root_ops.empty()) {
            const u32 count = static_cast<u32>(real->mt_root_ops.size());
            u64* const out = stream.Reserve(count + 1);
            out[0] = SpmcStream::MakeHeader(static_cast<u8>(MT::StreamEntry::RootTable), count + 1, count);
            std::memcpy(out + 1, real->mt_root_ops.data(), count * 8);
            stream.Commit();
            real->mt_root_ops.clear();
        }
    }
    KeplerCompute* const compute = rasterizer.kepler_compute.Real();
    if (compute != nullptr && compute->mt_written.Any()) {
        const u32 marked = compute->mt_written.Count();
        u64* const out = stream.Reserve(marked + 1);
        const u32 count = compute->mt_written.Drain(compute->regs.reg_array.data(),
                                                    KeplerCompute::Regs::NUM_REGS, out + 1);
        for (u32 i = count; i < marked; ++i) {
            out[1 + i] = 0;
        }
        out[0] =
            SpmcStream::MakeHeader(static_cast<u8>(MT::StreamEntry::ComputeDeltas), marked + 1, count);
        stream.Commit();
    }
}

inline void Nx2DrawPipeline::CloseBatch() {
    if (!batch_open) {
        return;
    }
    const u64 payload[2]{batch_id, batch_draws};
    stream.Push(static_cast<u8>(MT::StreamEntry::BatchEnd), 0, payload, 2);
    // The batch's resolver (and a committer waiting for it) may sleep.
    stream.Flush();
    ++batch_id;
    open_batch.store(batch_id, std::memory_order_relaxed);
    batch_draws = 0;
    batch_open = false;
}

inline void Nx2DrawPipeline::SerialRaw(MT::Kind kind, void (*fn)(void*), void* arg) {
    CloseBatch();
    EmitDeltas();
    std::atomic<u32> done{0};
    // A channel bind also stops the resolvers: they copy the new channel's state before the
    // sequencer touches the engines again.
    VideoCommon::MT::BarrierAcks acks;
    const bool barrier = kind == MT::Kind::Channel && resolver_count != 0;
    u64* const out = stream.Reserve(5);
    out[0] = SpmcStream::MakeHeader(static_cast<u8>(MT::StreamEntry::Serial), 5, static_cast<u32>(kind));
    out[1] = reinterpret_cast<u64>(fn);
    out[2] = reinterpret_cast<u64>(arg);
    out[3] = reinterpret_cast<u64>(&done);
    out[4] = barrier ? reinterpret_cast<u64>(&acks) : 0;
    stream.Commit();
    stream.Flush();
    Count(counters.serials, kind);
    const bool timed = VideoCommon::MT::Stats();
    const auto begin = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (u32 spins = 0; spins < 8192; ++spins) {
        if (done.load(std::memory_order_acquire) != 0) {
            break;
        }
        MTDetail::Pause();
    }
    std::optional<VideoCommon::MT::WaitWatch::Scope> watch_scope;
    if (done.load(std::memory_order_acquire) == 0) {
        watch_scope.emplace(watch, VideoCommon::MT::WaitSite::SeqSerial, stream.WritePosition());
    }
    while (done.load(std::memory_order_acquire) == 0) {
        const u32 seen = serial_word.load(std::memory_order_seq_cst);
        if (done.load(std::memory_order_seq_cst) != 0) {
            break;
        }
        serial_word.wait(seen, std::memory_order_seq_cst);
    }
    watch_scope.reset();
    if (barrier) {
        counters.barriers.fetch_add(1, std::memory_order_relaxed);
        VideoCommon::MT::WaitWatch::Scope acks_scope{watch, VideoCommon::MT::WaitSite::SeqBarrierAcks,
                                                    resolver_count};
        for (u32 spins = 0; acks.acks.load(std::memory_order_acquire) != resolver_count; ++spins) {
            if (spins < 4096) {
                MTDetail::Pause();
            } else {
                std::this_thread::yield();
            }
        }
    }
    if (timed) {
        counters.serial_wait_ns.fetch_add(
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - begin)
                                 .count()),
            std::memory_order_relaxed);
    }
}

inline void Nx2DrawPipeline::WaitCommitted() {
    CloseBatch();
    stream.Flush();
    const u64 target = stream.WritePosition();
    if (stream.ConsumerPosition(consumer) >= target) {
        return;
    }
    counters.wait_committed.fetch_add(1, std::memory_order_relaxed);
    const bool timed = VideoCommon::MT::Stats();
    const auto begin = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    SCOPE_EXIT {
        if (timed) {
            counters.wait_committed_ns.fetch_add(
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count()),
                std::memory_order_relaxed);
        }
    };
    for (u32 spins = 0; spins < 4096; ++spins) {
        if (stream.ConsumerPosition(consumer) >= target) {
            return;
        }
        MTDetail::Pause();
    }
    VideoCommon::MT::WaitWatch::Scope watch_scope{watch, VideoCommon::MT::WaitSite::SeqWaitCommitted,
                                                  target};
    commit_waiting.store(true, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (lite_loop) {
        // EDEN_NX2_MT_LITE_LOOP: the committer checks commit_waiting without a fence while it
        // runs entries (it may miss this store until its Idle): poll with short sleeps instead
        // of a futex wait that only its notify could end.
        for (u32 rounds = 0; stream.ConsumerPosition(consumer) < target; ++rounds) {
            if (rounds < 64) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds{20});
            }
        }
        commit_waiting.store(false, std::memory_order_relaxed);
        return;
    }
    while (true) {
        const u32 seen = commit_word.load(std::memory_order_seq_cst);
        if (stream.ConsumerPosition(consumer) >= target) {
            break;
        }
        commit_word.wait(seen, std::memory_order_seq_cst);
    }
    commit_waiting.store(false, std::memory_order_relaxed);
}

inline void Nx2DrawPipeline::BeforeWrite(u64 device_addr, u64 size) {
    if (VideoCommon::MT::StrictTextures() || Tegra::Nx2MayHoldTexture(device_addr, size) ||
        Tegra::Nx2AnyCommandListPage(device_addr, size)) {
        if (stream.ConsumerPosition(consumer) != stream.WritePosition()) {
            counters.write_barriers.fetch_add(1, std::memory_order_relaxed);
            WaitCommitted();
        }
    }
}

inline Nx2DrawPipeline::ChannelMirrors* Nx2DrawPipeline::FindMirrors(const Maxwell3D* real3d) {
    for (const auto& entry : mirrors) {
        if (entry->real3d == real3d) {
            return entry.get();
        }
    }
    return nullptr;
}

inline void Nx2DrawPipeline::BindCurrentChannel() {
    Maxwell3D* const real3d = rasterizer.maxwell3d.Real();
    KeplerCompute* const real_compute = rasterizer.kepler_compute.Real();
    if (real3d == nullptr) {
        current = nullptr;
        return;
    }
    ChannelMirrors* pair = FindMirrors(real3d);
    if (pair == nullptr) {
        auto created = std::make_unique<ChannelMirrors>();
        created->real3d = real3d;
        created->real_compute = real_compute;
        created->mirror3d = std::make_unique<Maxwell3D>(real3d->GetMemoryManager());
        Maxwell3D& m = *created->mirror3d;
        m.regs = real3d->regs;
        m.state = real3d->state;
        m.root_tables = real3d->root_tables;
        m.dirty.tables = real3d->dirty.tables;
        m.draw_manager.draw_state = real3d->draw_manager.draw_state;
        m.dirty.flags.set();
        auto& overrides3d = VideoCommon::MT::g_engine_overrides<Maxwell3D>;
        ASSERT(overrides3d.count < overrides3d.pairs.size());
        overrides3d.pairs[overrides3d.count++] = {real3d, created->mirror3d.get()};
        if (real_compute != nullptr) {
            created->mirror_compute = std::make_unique<KeplerCompute>(real_compute->GetMemoryManager());
            created->mirror_compute->regs = real_compute->regs;
            created->mirror_compute->launch_description = real_compute->launch_description;
            auto& overrides = VideoCommon::MT::g_engine_overrides<KeplerCompute>;
            ASSERT(overrides.count < overrides.pairs.size());
            overrides.pairs[overrides.count++] = {real_compute, created->mirror_compute.get()};
        }
        pair = created.get();
        mirrors.push_back(std::move(created));
        LOG_INFO(Render_Vulkan, "MT: mirrors for 3D engine {} / compute {}", fmt::ptr(real3d),
                 fmt::ptr(real_compute));
    } else {
        // Registers that changed while the channel was not bound (none expected): applied as
        // deltas so their dirty flags are set.
        Maxwell3D& m = *pair->mirror3d;
        for (u32 i = 0; i < Maxwell3D::Regs::NUM_REGS; ++i) {
            if (m.regs.reg_array[i] != real3d->regs.reg_array[i]) {
                VideoCommon::MT::MirrorApplier<Maxwell3D>::Apply(m, i, real3d->regs.reg_array[i]);
            }
        }
        m.state = real3d->state;
        m.root_tables = real3d->root_tables;
        m.dirty.flags.set();
        if (pair->mirror_compute && real_compute != nullptr) {
            pair->mirror_compute->regs = real_compute->regs;
        }
    }
    real3d->mt_record = true;
    real3d->mt_written.Clear();
    real3d->mt_cb_binds.clear();
    real3d->mt_root_ops.clear();
    if (real_compute != nullptr) {
        real_compute->mt_written.Clear();
    }
    pair->mirror3d->GetNativeMacroEngine().native_turing =
        real3d->GetNativeMacroEngine().native_turing;
    current = pair;
    channel_gen.fetch_add(1, std::memory_order_release);
    rasterizer.state_tracker.SetFlags(&pair->mirror3d->dirty.flags);
    rasterizer.state_tracker.InvalidateState();
}

inline void Nx2DrawPipeline::VerifyMirror(const char* where) {
    if (current == nullptr) {
        return;
    }
    counters.verify_checks.fetch_add(1, std::memory_order_relaxed);
    const auto& real_regs = current->real3d->regs.reg_array;
    const auto& mirror_regs = current->mirror3d->regs.reg_array;
    if (std::memcmp(real_regs.data(), mirror_regs.data(), sizeof(real_regs)) == 0) {
        return;
    }
    u32 first = 0;
    u32 differing = 0;
    for (u32 i = Maxwell3D::Regs::NUM_REGS; i-- > 0;) {
        if (real_regs[i] != mirror_regs[i]) {
            first = i;
            ++differing;
        }
    }
    counters.verify_mismatches.fetch_add(1, std::memory_order_relaxed);
    static u32 reported{};
    if (reported++ < 32) {
        LOG_WARNING(Render_Vulkan,
                    "MT verify: mirror register {:#x} {} real {:#x} mirror {:#x} ({} differ) at {}",
                    first, VideoCommon::MT::Maxwell3DRegName(first), real_regs[first],
                    mirror_regs[first], differing, where);
    }
    // Keep checking for other misses.
    for (u32 i = 0; i < Maxwell3D::Regs::NUM_REGS; ++i) {
        if (real_regs[i] != mirror_regs[i]) {
            VideoCommon::MT::MirrorApplier<Maxwell3D>::Apply(*current->mirror3d, i, real_regs[i]);
        }
    }
}

inline void Nx2DrawPipeline::CommitterLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("GPUCommit");
    Tegra::Nx2ApplyTimerSlack();
    VideoCommon::MT::g_role = VideoCommon::MT::Role::Committer;
    auto& split = VideoCommon::GpuSplit::Get();
    VideoCommon::GpuSplit::MarkCommitterThread(true);
    split.committer_running.store(true, std::memory_order_seq_cst);
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
    split.RegisterHostThread();
    BindCurrentChannel();
    start_ready.store(true, std::memory_order_release);
    while (true) {
        const u64* const entry = stream.Peek(consumer);
        if (entry == nullptr) {
            if (stop.stop_requested()) {
                break;
            }
            Idle(stop);
            continue;
        }
        if (lite_loop) {
            // EDEN_NX2_MT_LITE_LOOP: no locked instruction per entry. `committer_quiet` only
            // changes here when Idle set it; a WaitCommitted is answered in Idle (where this
            // thread goes once the waiting sequencer stops producing) or by its own timed polls.
            if (committer_quiet.load(std::memory_order_relaxed)) {
                committer_quiet.store(false, std::memory_order_seq_cst);
            }
            RunEntry(entry);
            stream.Advance(consumer, entry);
            if (commit_waiting.load(std::memory_order_relaxed)) {
                commit_word.fetch_add(1, std::memory_order_seq_cst);
                commit_word.notify_all();
            }
        } else {
            committer_quiet.store(false, std::memory_order_seq_cst);
            RunEntry(entry);
            stream.Advance(consumer, entry);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (commit_waiting.load(std::memory_order_relaxed)) {
                commit_word.fetch_add(1, std::memory_order_seq_cst);
                commit_word.notify_all();
            }
        }
        if (hook_requested.load(std::memory_order_relaxed)) [[unlikely]] {
            hook_requested.store(false, std::memory_order_seq_cst);
            rasterizer.scheduler.FlushDeferred();
        }
    }
    // EDEN_NX2_MT_REPLAY: the open segment belongs to this thread.
    rasterizer.scheduler.EndReplaySegment(ReplayCloseReason::Idle);
    rasterizer.scheduler.FlushDeferred();
    // Back to the real engines for whoever records next.
    for (const auto& pair : mirrors) {
        pair->real3d->mt_record = false;
        pair->real3d->mt_written.Clear();
        pair->real3d->mt_cb_binds.clear();
        pair->real3d->mt_root_ops.clear();
        if (pair->real_compute != nullptr) {
            pair->real_compute->mt_written.Clear();
        }
    }
    if (current != nullptr) {
        rasterizer.state_tracker.SetFlags(&current->real3d->dirty.flags);
        rasterizer.state_tracker.InvalidateState();
    }
    VideoCommon::MT::g_engine_overrides<Maxwell3D>.count = 0;
    VideoCommon::MT::g_engine_overrides<KeplerCompute>.count = 0;
    current = nullptr;
    mirrors.clear();
    committer_quiet.store(true, std::memory_order_seq_cst);
    split.committer_running.store(false, std::memory_order_seq_cst);
    split.StoreSettled(last_mark);
    VideoCommon::GpuSplit::MarkCommitterThread(false);
    VideoCommon::MT::g_role = VideoCommon::MT::Role::None;
}

inline void Nx2DrawPipeline::Idle(std::stop_token& stop) {
    const bool timed = VideoCommon::MT::Stats();
    const auto begin = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    SCOPE_EXIT {
        if (timed) {
            counters.committer_idle_ns.fetch_add(
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count()),
                std::memory_order_relaxed);
        }
    };
    if (lite_loop) {
        // EDEN_NX2_MT_LITE_LOOP: the Dekker half the entry loop leaves out (a sequencer in
        // WaitCommitted stops producing, so this thread gets here once it passed the target).
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (commit_waiting.load(std::memory_order_relaxed)) {
            commit_word.fetch_add(1, std::memory_order_seq_cst);
            commit_word.notify_all();
        }
    }
    bool hook_break = false;
    for (u32 spins = 0; spins < 4096; ++spins) {
        if (stream.Peek(consumer) != nullptr || stop.stop_requested()) {
            return;
        }
        // EDEN_NX2_MT_LITE_QUIET: a front Drain waits for this thread to go quiet: do it now.
        if (lite_quiet && hook_requested.load(std::memory_order_relaxed)) {
            hook_break = true;
            break;
        }
        MTDetail::Pause();
    }
    // EDEN_NX2_MT_REPLAY: going to sleep, end the open segment (it is recorded in the chunk
    // like the closures it replaces would be; FlushDeferred/DispatchWork hand it over).
    rasterizer.scheduler.EndReplaySegment(ReplayCloseReason::Idle);
    if (!committer_quiet.load(std::memory_order_relaxed) ||
        hook_requested.load(std::memory_order_relaxed)) {
        hook_requested.store(false, std::memory_order_seq_cst);
        CollectEpochs();
        rasterizer.scheduler.FlushDeferred();
        committer_quiet.store(true, std::memory_order_seq_cst);
    }
    if (hook_break) {
        // Quiet now; spin the rest of the usual time for the next entry before sleeping.
        for (u32 spins = 0; spins < 4096; ++spins) {
            if (stream.Peek(consumer) != nullptr || stop.stop_requested() ||
                hook_requested.load(std::memory_order_relaxed)) {
                return;
            }
            MTDetail::Pause();
        }
    }
    stream.WaitForData(consumer, [&] {
        return stop.stop_requested() || hook_requested.load(std::memory_order_seq_cst);
    });
}

inline bool Nx2DrawPipeline::ApplyMirrorEntry(Maxwell3D* mirror3d, KeplerCompute* mirror_compute,
                                               const u64* entry) {
    const u64 head = entry[0];
    const u32 aux = SpmcStream::HeaderAux(head);
    switch (static_cast<MT::StreamEntry>(SpmcStream::HeaderKind(head))) {
    case MT::StreamEntry::Deltas:
        VideoCommon::MT::MirrorApplier<Maxwell3D>::ApplyPairs(*mirror3d, entry + 1, aux);
        return true;
    case MT::StreamEntry::ComputeDeltas:
        if (mirror_compute != nullptr) {
            VideoCommon::MT::MirrorApplier<KeplerCompute>::ApplyPairs(*mirror_compute, entry + 1,
                                                                      aux);
        }
        return true;
    case MT::StreamEntry::CbBind:
        for (u32 i = 0; i < aux; ++i) {
            VideoCommon::MT::CbBindDelta bind;
            std::memcpy(&bind, entry + 1 + i * 2, sizeof(bind));
            Maxwell3D::ApplyCbBind(mirror3d->state, bind);
        }
        return true;
    case MT::StreamEntry::RootTable:
        for (u32 i = 0; i < aux; ++i) {
            const u64 op = entry[1 + i];
            Maxwell3D::ApplyRootTableMethod(mirror3d->root_tables, static_cast<u32>(op),
                                            static_cast<u32>(op >> 32));
        }
        return true;
    default:
        return false;
    }
}

inline void Nx2DrawPipeline::RunEntry(const u64* entry) {
    const u64 head = entry[0];
    const u32 aux = SpmcStream::HeaderAux(head);
    switch (static_cast<MT::StreamEntry>(SpmcStream::HeaderKind(head))) {
    case MT::StreamEntry::Deltas:
    case MT::StreamEntry::ComputeDeltas:
    case MT::StreamEntry::RootTable:
        if (current != nullptr) {
            ApplyMirrorEntry(current->mirror3d.get(), current->mirror_compute.get(), entry);
        }
        break;
    case MT::StreamEntry::CbBind:
        if (current != nullptr) {
            ApplyMirrorEntry(current->mirror3d.get(), current->mirror_compute.get(), entry);
            if (VideoCommon::MT::UniformBindFold()) {
                // EDEN_NX2_MT_UBIND_FOLD: what the "uniform" Command did per bind (the
                // sequencer skipped it for binds its engine recorded).
                ApplyUniformBinds(entry + 1, aux);
            }
        }
        break;
    case MT::StreamEntry::Command: {
        const auto thunk = reinterpret_cast<void (*)(Nx2DrawPipeline&, const u64*)>(entry[1]);
        try {
            thunk(*this, entry + 3);
        } catch (const std::exception& e) {
            LOG_ERROR(Render_Vulkan, "MT: command {} threw: {}", KindName(aux), e.what());
        }
        break;
    }
    case MT::StreamEntry::Serial:
        RunSerial(entry);
        break;
    case MT::StreamEntry::DrawPacket:
        CommitDraw(entry);
        break;
    case MT::StreamEntry::ComputePacket:
        CommitCompute(entry);
        break;
    case MT::StreamEntry::BatchEnd:
        counters.batches.fetch_add(1, std::memory_order_relaxed);
        if (resolver_count != 0) {
            // The batch's draws are in command buffers up to the current tick: its streamed
            // uniforms can go once that tick (ReuseWindow batches later) completes.
            resolvers[entry[1] % resolver_count]->StampBatch(entry[1],
                                                             rasterizer.scheduler.CurrentTick());
        }
        // Objects retired while resolvers looked at this batch can go once they left it.
        CollectEpochs();
        break;
    case MT::StreamEntry::Mark:
        last_mark = entry[1];
        VideoCommon::GpuSplit::Get().StoreSettled(entry[1]);
        break;
    default:
        break;
    }
}

inline void Nx2DrawPipeline::ApplyUniformBinds(const u64* binds, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        VideoCommon::MT::CbBindDelta bind;
        std::memcpy(&bind, binds + i * 2, sizeof(bind));
        if (bind.enabled != 0) {
            rasterizer.buffer_cache.BindGraphicsUniformBuffer(bind.stage, bind.slot, bind.gpu_addr,
                                                              bind.size);
        } else {
            rasterizer.buffer_cache.DisableGraphicsUniformBuffer(bind.stage, bind.slot);
        }
    }
}

inline void Nx2DrawPipeline::RunSerial(const u64* entry) {
    const auto fn = reinterpret_cast<void (*)(void*)>(entry[1]);
    void* const arg = reinterpret_cast<void*>(entry[2]);
    auto* const done = reinterpret_cast<std::atomic<u32>*>(entry[3]);
    ChannelMirrors* const pair = current;
    if (verify && pair != nullptr) {
        VerifyMirror("serial");
    }
    // The old code against the real engines (the sequencer waits), with this thread's dirty
    // flags: they live in the mirror between Serials.
    auto& overrides3d = VideoCommon::MT::g_engine_overrides<Maxwell3D>;
    auto& overrides_compute = VideoCommon::MT::g_engine_overrides<KeplerCompute>;
    const size_t saved3d = overrides3d.count;
    const size_t saved_compute = overrides_compute.count;
    overrides3d.count = 0;
    overrides_compute.count = 0;
    if (pair != nullptr) {
        pair->mirror3d->dirty.flags[VideoCommon::Dirty::Shaders] = true;
        std::swap(pair->real3d->dirty.flags, pair->mirror3d->dirty.flags);
        rasterizer.state_tracker.SetFlags(&pair->real3d->dirty.flags);
    }
    fn(arg);
    if (pair != nullptr) {
        std::swap(pair->real3d->dirty.flags, pair->mirror3d->dirty.flags);
    }
    overrides3d.count = saved3d;
    overrides_compute.count = saved_compute;
    if (rasterizer.maxwell3d.Real() != (pair != nullptr ? pair->real3d : nullptr)) {
        BindCurrentChannel();
    } else if (pair != nullptr) {
        rasterizer.state_tracker.SetFlags(&pair->mirror3d->dirty.flags);
    }
    done->store(1, std::memory_order_seq_cst);
    serial_word.fetch_add(1, std::memory_order_seq_cst);
    serial_word.notify_all();
}

inline void Nx2DrawPipeline::LogStats() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_stats < std::chrono::seconds{10}) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - last_stats).count();
    last_stats = now;
    const auto take = [](std::atomic<u64>& value) {
        return value.exchange(0, std::memory_order_relaxed);
    };
    std::string serials;
    std::string commands;
    u64 serial_total = 0;
    for (size_t i = 0; i < KindNamesCount(); ++i) {
        if (const u64 value = take(counters.serials[i]); value != 0) {
            serials += fmt::format(" {} {:.0f}", MT::KindNames[i], static_cast<double>(value) / seconds);
            serial_total += value;
        }
        if (const u64 value = take(counters.commands[i]); value != 0) {
            commands += fmt::format(" {} {:.0f}", MT::KindNames[i], static_cast<double>(value) / seconds);
        }
    }
    std::string forced;
    for (size_t i = 0; i < counters.forced_serial.size(); ++i) {
        if (const u64 value = take(counters.forced_serial[i]); value != 0) {
            forced += fmt::format(" {} {:.0f}", MT::ForcedSerialNames[i], static_cast<double>(value) / seconds);
        }
    }
    const u64 packets = take(counters.packets);
    const u64 compute_packets = take(counters.compute_packets);
    const u64 words = take(counters.packet_words);
    LOG_INFO(Render_Vulkan,
             "MT stats/s: draw packets {:.0f}, compute packets {:.0f} ({} failed) ({:.0f} B avg) "
             "| serial {:.0f} ({:.1f} ms wait):{} | "
             "commands:{} | forced serial:{} | marks {:.0f}, shortcuts {:.0f}, wait-committed "
             "{:.0f}, write barriers {:.0f}, delta words {:.0f} | verify {:.0f} checks, {} "
             "mismatches, {} concurrent CPU writes",
             static_cast<double>(packets) / seconds, static_cast<double>(compute_packets) / seconds,
             counters.compute_failures.load(std::memory_order_relaxed),
             packets + compute_packets != 0
                 ? static_cast<double>(words * 8) / static_cast<double>(packets + compute_packets)
                 : 0.0,
             static_cast<double>(serial_total) / seconds,
             static_cast<double>(take(counters.serial_wait_ns)) / 1e6 / seconds, serials, commands,
             forced, static_cast<double>(take(counters.marks)) / seconds,
             static_cast<double>(take(counters.settled_shortcuts)) / seconds,
             static_cast<double>(take(counters.wait_committed)) / seconds,
             static_cast<double>(take(counters.write_barriers)) / seconds,
             static_cast<double>(take(counters.delta_words)) / seconds,
             static_cast<double>(take(counters.verify_checks)) / seconds,
             counters.verify_mismatches.load(std::memory_order_relaxed),
             counters.verify_cpu_writes.load(std::memory_order_relaxed));
    LogResolverStats(seconds);
    // Per-stage time (ms per second of wall time) and the phase 6/7 paths.
    const auto ms = [&](std::atomic<u64>& value) {
        return static_cast<double>(take(value)) / 1e6 / seconds;
    };
    const double stream_ms = static_cast<double>(std::exchange(stream.space_wait_ns, 0)) / 1e6 / seconds;
    LOG_INFO(Render_Vulkan,
             "MT stages ms/s: sequencer draws {:.1f} (waits: wait-committed {:.1f}, stream full "
             "{:.1f}) | committer idle {:.1f} | buffer-mutex invalidations {:.0f}/s, dma serial "
             "{:.0f}/s | ubo gen: hits {:.0f}/s, misses {:.0f}/s, unprotected {:.0f}/s | flush: "
             "skipped {:.0f}/s, commands {:.0f}/s, serial {:.0f}/s | invalidate: sequencer "
             "{:.0f}/s, texture commands {:.0f}/s, gpu-page serial {:.0f}/s",
             ms(counters.seq_capture_ns), ms(counters.wait_committed_ns), stream_ms,
             ms(counters.committer_idle_ns),
             static_cast<double>(take(counters.seq_mutex_writes)) / seconds,
             static_cast<double>(take(counters.dma_serial)) / seconds,
             static_cast<double>(take(counters.ubo_gen_hits)) / seconds,
             static_cast<double>(take(counters.ubo_gen_misses)) / seconds,
             static_cast<double>(take(counters.ubo_gen_unprotected)) / seconds,
             static_cast<double>(take(counters.flush_skipped)) / seconds,
             static_cast<double>(take(counters.flush_commands)) / seconds,
             static_cast<double>(take(counters.flush_serials)) / seconds,
             static_cast<double>(take(counters.inval_sequencer)) / seconds,
             static_cast<double>(take(counters.inval_tex_commands)) / seconds,
             static_cast<double>(take(counters.inval_owner_serials)) / seconds);
    {
        // Average draw packet bytes by part (EDEN_NX2_MT_COMPACT decisions per second).
        const double draws = packets != 0 ? static_cast<double>(packets) : 1.0;
        const auto per_draw = [&](std::atomic<u64>& value) {
            return static_cast<double>(take(value)) / draws;
        };
        const auto rate = [&](std::atomic<u64>& value) {
            return static_cast<double>(take(value)) / seconds;
        };
        LOG_INFO(Render_Vulkan,
                 "MT draw packet B: header {:.0f}, overlay {:.0f}, ubo entries {:.0f}, ubo bytes "
                 "{:.0f}, uploads {:.0f}, other {:.0f} | compact {:.0f}/s: ubo same {:.0f}/s, "
                 "patched {:.0f}/s, full {:.0f}/s, descriptors referenced {:.0f}/s, sent {:.0f}/s, "
                 "overlay memo hits {:.0f}/s",
                 per_draw(counters.pkt_header_bytes), per_draw(counters.pkt_overlay_bytes),
                 per_draw(counters.pkt_ubo_entry_bytes), per_draw(counters.pkt_ubo_data_bytes),
                 per_draw(counters.pkt_upload_bytes), per_draw(counters.pkt_other_bytes),
                 rate(counters.compact_packets), rate(counters.ubo_same),
                 rate(counters.ubo_patched), rate(counters.ubo_full), rate(counters.desc_hits),
                 rate(counters.desc_misses), rate(counters.overlay_memo_hits));
    }
}

} // namespace Vulkan

// ---- Draw packets ----

namespace Vulkan {

template <typename T>
T Nx2DrawPipeline::SeqRead(u64 gpu_addr) {
    T value;
    const u8* host = nullptr;
    if constexpr (sizeof(T) != sizeof(u32)) {
        // EDEN_NX2_MT_LITE_XLATE: MemoryManager::Read without its own walk (only u32 reads may
        // hit the syncpoint shim; unmapped addresses still go to Read, which reports them).
        if (lite_xlate) {
            if (const auto device_addr = SeqTranslate(gpu_addr)) {
                host = rasterizer.device_memory.GetPointer<u8>(*device_addr);
            }
        }
    }
    if (host != nullptr) {
        std::memcpy(&value, host, sizeof(T));
    } else {
        value = rasterizer.gpu_memory->template Read<T>(gpu_addr);
    }
    const u32 offset = static_cast<u32>(raw_bytes.size());
    raw_bytes.resize(raw_bytes.size() + sizeof(T));
    std::memcpy(raw_bytes.data() + offset, &value, sizeof(T));
    raw_reads.push_back({gpu_addr, static_cast<u32>(sizeof(T)), offset, false});
    return value;
}

inline std::optional<DAddr> Nx2DrawPipeline::SeqTranslate(GPUVAddr gpu_addr) {
    if (lite_xlate) {
        return seq_xlate.Translate(*rasterizer.gpu_memory, gpu_addr);
    }
    return rasterizer.gpu_memory->GpuToCpuAddress(gpu_addr);
}

inline void Nx2DrawPipeline::SeqReadDescriptor(u64 gpu_addr) {
    // DescriptorTable::Read's read of a TIC or TSC entry.
    constexpr u32 Size = 32;
    const u32 offset = static_cast<u32>(raw_bytes.size());
    raw_bytes.resize(raw_bytes.size() + Size);
    u8* const out = raw_bytes.data() + offset;
    const bool one_page =
        (gpu_addr & Core::Memory::YUZU_PAGEMASK) + Size <= Core::Memory::YUZU_PAGESIZE;
    const auto pointer = [&]() -> const u8* {
        if (!lite_xlate) {
            return rasterizer.gpu_memory->GetPointer(gpu_addr);
        }
        // EDEN_NX2_MT_LITE_XLATE: MemoryManager::GetPointer through the translation cache.
        const auto device_addr = SeqTranslate(gpu_addr);
        return device_addr ? rasterizer.device_memory.GetPointer<u8>(*device_addr)
                           : nullptr;
    };
    if (const u8* const host = one_page ? pointer() : nullptr) {
        std::memcpy(out, host, Size);
    } else {
        rasterizer.gpu_memory->ReadBlockUnsafe(gpu_addr, out, Size);
    }
    raw_reads.push_back({gpu_addr, Size, offset, true});
}

inline void Nx2DrawPipeline::SeqTakeUploads(DAddr device_addr, u32 size) {
    if (device_addr == 0 || size == 0) {
        return;
    }
    for (const auto& [addr, taken_size] : taken_ranges) {
        if (addr == device_addr && taken_size == size) {
            return;
        }
    }
    taken_ranges.emplace_back(device_addr, size);
    rasterizer.buffer_cache.MtTakeUploads(device_addr, size, [&](DAddr start, u64 length) {
        VideoCommon::MT::UploadEntry entry{
            .range_addr = device_addr,
            .range_size = size,
            .size = static_cast<u32>(length),
            .device_addr = start,
            .data = 0,
            .heap = 0,
            .pad = 0,
        };
        if (defer_upload_reads) {
            // EDEN_NX2_MT_LITE_SEQLOCK: the bytes are read once the tracker lock is released
            // (SeqReadDeferredUploads).
            uploads.push_back(entry);
            return;
        }
        if (length > 64 * 1024 || blob.size() > 1024 * 1024) {
            u8* const heap = new u8[length];
            rasterizer.device_memory.ReadBlockUnsafe(start, heap, length);
            entry.data = reinterpret_cast<u64>(heap);
            entry.heap = 1;
        } else {
            const size_t offset = blob.size();
            blob.resize(offset + length);
            rasterizer.device_memory.ReadBlockUnsafe(start, blob.data() + offset, length);
            entry.data = offset;
            blob_upload_bytes += static_cast<u32>(length);
        }
        uploads.push_back(entry);
    });
}

inline void Nx2DrawPipeline::SeqReadDeferredUploads() {
    // The tracker bits were taken under the lock. A guest write since then traps (and marks the
    // range CPU-modified again) before it lands, so bytes read now are at least as new as at the
    // take and the next capture takes the range again: as with a read under the lock, which
    // the trap only waited for.
    if (!defer_upload_reads) {
        return;
    }
    defer_upload_reads = false;
    for (size_t index = deferred_uploads_first; index < uploads.size(); ++index) {
        VideoCommon::MT::UploadEntry& entry = uploads[index];
        const u64 length = entry.size;
        if (length > 64 * 1024 || blob.size() > 1024 * 1024) {
            u8* const heap = new u8[length];
            rasterizer.device_memory.ReadBlockUnsafe(entry.device_addr, heap, length);
            entry.data = reinterpret_cast<u64>(heap);
            entry.heap = 1;
        } else {
            const size_t offset = blob.size();
            blob.resize(offset + length);
            rasterizer.device_memory.ReadBlockUnsafe(entry.device_addr, blob.data() + offset,
                                                     length);
            entry.data = offset;
            blob_upload_bytes += static_cast<u32>(length);
        }
    }
}

inline int Nx2DrawPipeline::CaptureDraw(GraphicsPipeline* pipeline, bool is_indexed,
                                         const IndirectParams* indirect) {
    using Maxwell = Maxwell3D::Regs;
    Maxwell3D& real = *rasterizer.maxwell3d.Real();
    Tegra::MemoryManager& memory = *rasterizer.gpu_memory;
    const auto& regs = real.regs;
    const auto& infos = pipeline->StageInfos();
    ClearCapture();
    if (verify) {
        verify_cpu_writes_since.store(VideoCommon::MT::GuestCpuWrites().Snapshot(),
                                      std::memory_order_release);
    }

    // Pass 1: what forces the old path, and the reads ConfigureImpl will repeat.
    if (regs.transform_feedback_enabled != 0) {
        return 1;
    }
    static const bool draw_capture = std::getenv("EDEN_NATIVE_DRAW_CAPTURE") != nullptr;
    if (draw_capture) {
        return 5;
    }
    const bool native = real.GetNativeMacroEngine().native_turing;
    for (u32 index = 0; index < Maxwell::NumVertexArrays; ++index) {
        if (native && regs.vertex_streams[index].enable != 0 &&
            regs.NativeVertexStreamSize(index) > (std::numeric_limits<u32>::max)()) {
            return 10;
        }
    }
    for (const Shader::Info& info : infos) {
        if (!info.texture_buffer_descriptors.empty() || !info.image_buffer_descriptors.empty()) {
            return 2;
        }
        for (const auto& desc : info.storage_buffers_descriptors) {
            if (desc.count != 1) {
                return 11;
            }
        }
    }
    const bool via_header_index = regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding;
    const u32 tic_limit = regs.tex_header.limit;
    const u32 tsc_limit = via_header_index ? tic_limit : regs.tex_sampler.limit;
    const bool native_tiling = memory.UsesNativeTiling();
    const SeqRecorder recorder{this};
    struct SsboCapture {
        DAddr device_addr;
        u32 size;
        bool is_written;
        size_t stage;
    };
    boost::container::small_vector<SsboCapture, 32> ssbos;
    const bool prefers_sparse = rasterizer.buffer_cache.MtPrefersSparseSources();
    for (size_t stage = 0; stage < infos.size(); ++stage) {
        const Shader::Info& info = infos[stage];
        const auto& cbufs = real.state.shader_stages[stage].const_buffers;
        u8 ssbo_index = 0;
        for (const auto& desc : info.storage_buffers_descriptors) {
            VideoCommon::MT::SsboEntry& ssbo_entry = ssbo_entries.emplace_back();
            ssbo_entry = VideoCommon::MT::SsboEntry{
                .stage = static_cast<u8>(stage),
                .index = ssbo_index++,
                .is_written = static_cast<u8>(desc.is_written ? 1 : 0),
                .skipped = 1,
                .size = 0,
                .device_addr = 0,
                .gpu_addr = 0,
            };
            const GPUVAddr ssbo_addr = cbufs[desc.cbuf_index].address + desc.cbuf_offset;
            const GPUVAddr gpu_addr = SeqRead<u64>(ssbo_addr);
            if (gpu_addr == 0) {
                continue;
            }
            const u64 next_qword = SeqRead<u64>(ssbo_addr + 8);
            const VideoCommon::Binding binding = rasterizer.buffer_cache.MtStorageBinding(
                gpu_addr, next_qword, desc.cbuf_index, desc.is_written);
            ssbo_entry.gpu_addr = binding.gpu_addr;
            ssbo_entry.size = binding.size;
            ssbo_entry.device_addr = binding.device_addr;
            if (binding.size == 0 || binding.device_addr == 0) {
                continue;
            }
            ssbo_entry.skipped = 0;
            if ((!desc.is_written || prefers_sparse) &&
                !SeqIsContinuous(binding.gpu_addr, binding.size)) {
                return 4;
            }
            ssbos.push_back({binding.device_addr, binding.size, desc.is_written, stage});
        }
        const auto read_handle = [&](const auto& desc, u32 index) -> std::pair<u32, u32> {
            if (desc.handle_program != ~0U) {
                return Tegra::Texture::TexturePair(
                    VideoCommon::EvaluateTextureHandle(info, desc.handle_program, recorder,
                                                       [&](u32 cbuf) -> GPUVAddr {
                                                           return cbuf < cbufs.size() &&
                                                                          cbufs[cbuf].enabled
                                                                      ? cbufs[cbuf].address
                                                                      : GPUVAddr{};
                                                       }),
                    via_header_index);
            }
            const u32 index_offset{index << desc.size_shift};
            const u32 offset{desc.cbuf_offset + index_offset};
            const GPUVAddr addr{cbufs[desc.cbuf_index].address + offset};
            if constexpr (requires { desc.has_secondary; }) {
                if (desc.has_secondary) {
                    const u32 second_offset{desc.secondary_cbuf_offset + index_offset};
                    const GPUVAddr separate_addr{cbufs[desc.secondary_cbuf_index].address +
                                                 second_offset};
                    const u32 lhs_raw{SeqRead<u32>(addr) << desc.shift_left};
                    const u32 rhs_raw{SeqRead<u32>(separate_addr) << desc.secondary_shift_left};
                    return Tegra::Texture::TexturePair(lhs_raw | rhs_raw, via_header_index);
                }
            }
            return Tegra::Texture::TexturePair(SeqRead<u32>(addr), via_header_index);
        };
        const auto capture_view = [&](u32 handle) {
            const u32 index = Tegra::Texture::TextureHeaderIndex(handle, tic_limit, native_tiling);
            if (lite_no_tic_valid && resolver_count == 0) {
                // EDEN_NX2_MT_LITE_TICVALID: the validity bits are for resolvers only.
                if (index <= tic_limit) {
                    SeqReadDescriptor(regs.tex_header.Address() + u64{index} * 32);
                }
                return;
            }
            const u32 bit = tic_valid_count++;
            if (bit / 64 >= tic_valid.size()) {
                tic_valid.push_back(0);
            }
            if (index <= tic_limit) {
                const size_t offset = raw_bytes.size();
                SeqReadDescriptor(regs.tex_header.Address() + u64{index} * 32);
                if (SeqTicValid(raw_bytes.data() + offset)) {
                    tic_valid[bit / 64] |= u64{1} << (bit % 64);
                }
            }
        };
        for (const auto& desc : info.texture_descriptors) {
            for (u32 index = 0; index < desc.count; ++index) {
                const auto handle = read_handle(desc, index);
                capture_view(handle.first);
                if (handle.second <= tsc_limit) {
                    SeqReadDescriptor(regs.tex_sampler.Address() + u64{handle.second} * 32);
                }
            }
        }
        for (const auto& desc : info.image_descriptors) {
            for (u32 index = 0; index < desc.count; ++index) {
                capture_view(read_handle(desc, index).first);
            }
        }
    }

    // Pass 2: the buffer tracker, in the order the committer synchronizes the bindings.
    std::optional<MTDetail::SeqTrackerLock> lock{std::in_place, rasterizer.buffer_cache};
    SeqDeferUploadReads();
    const auto& draw_state = real.draw_manager.draw_state;
    if (indirect != nullptr) {
        // BindHostDrawIndirectBuffers' synchronizations (the count first).
        const auto take = [&](GPUVAddr gpu_addr, u64 size) {
            if (const auto device_addr = SeqTranslate(gpu_addr); device_addr && size != 0) {
                SeqTakeUploads(*device_addr, static_cast<u32>(size));
            }
        };
        if (indirect->include_count) {
            take(indirect->count_start_address, sizeof(u32));
        }
        take(indirect->indirect_start_address, indirect->buffer_size);
    }
    if (is_indexed && draw_state.inline_index_draw_indexes.empty()) {
        const auto& index_buffer = draw_state.index_buffer;
        const GPUVAddr begin = index_buffer.StartAddress();
        const u32 address_size = static_cast<u32>(index_buffer.EndAddress() - begin);
        const u32 draw_size = (index_buffer.count + index_buffer.first) *
                              static_cast<u32>(index_buffer.FormatSizeInBytes());
        const u32 size = (std::min)(address_size, draw_size);
        const auto device_addr = SeqTranslate(begin);
        geometry_entries.push_back({
            .kind = VideoCommon::MT::GeometryEntry::Index,
            .stream = 0,
            .pad = 0,
            .size = device_addr ? size : 0,
            .device_addr = device_addr && size != 0 ? *device_addr : 0,
        });
        if (device_addr && size != 0) {
            SeqTakeUploads(*device_addr, size);
        }
    } else if (is_indexed) {
        geometry_entries.push_back({
            .kind = VideoCommon::MT::GeometryEntry::InlineIndex,
            .stream = 0,
            .pad = 0,
            .size = static_cast<u32>(draw_state.inline_index_draw_indexes.size()),
            .device_addr = 0,
        });
    }
    for (u32 index = 0; index < Maxwell::NumVertexArrays; ++index) {
        const auto& array = regs.vertex_streams[index];
        if (array.enable == 0) {
            continue;
        }
        const GPUVAddr begin = array.Address();
        const GPUVAddr end = native ? begin + regs.NativeVertexStreamSize(index)
                                    : regs.vertex_stream_limits[index].Address() + 1;
        const auto device_addr = SeqTranslate(begin);
        u32 size = static_cast<u32>(end - begin);
        auto& geometry = geometry_entries.emplace_back();
        geometry = VideoCommon::MT::GeometryEntry{
            .kind = VideoCommon::MT::GeometryEntry::Vertex,
            .stream = static_cast<u8>(index),
            .pad = 0,
            .size = 0,
            .device_addr = 0,
        };
        if (size == 0 || !device_addr) {
            continue;
        }
        if (!memory.IsWithinGPUAddressRange(end) || size >= 64 * 1024 * 1024) {
            size = static_cast<u32>(memory.MaxContinuousRange(begin, size));
        }
        geometry.size = size;
        geometry.device_addr = *device_addr;
        SeqTakeUploads(*device_addr, size);
    }
    const u32 alignment = rasterizer.buffer_cache.MtUniformBufferAlignment();
    const u32 skip_size = rasterizer.buffer_cache.MtSkipCacheSize();
    const auto& sizes = pipeline->UniformSizes();
    size_t ssbo_cursor = 0;
    for (size_t stage = 0; stage < infos.size(); ++stage) {
        const auto& cbufs = real.state.shader_stages[stage].const_buffers;
        u32 mask = pipeline->EnabledUniformBufferMasks()[stage];
        while (mask != 0) {
            const u32 index = static_cast<u32>(std::countr_zero(mask));
            mask &= mask - 1;
            const auto& cbuf = cbufs[index];
            DAddr device_addr = 0;
            u32 binding_size = 0;
            if (cbuf.enabled) {
                if (const auto translated = SeqTranslate(cbuf.address)) {
                    device_addr = *translated;
                    binding_size = cbuf.size;
                }
            }
            if (index == 17 && real.root_tables.loaded && binding_size == 0) {
                continue; // streamed from the mirror's root table 0
            }
            const u32 size = (std::min)(binding_size, sizes[stage][index]);
            const bool has_host = rasterizer.buffer_cache.MtHasHostBuffer(device_addr);
            const bool needs_alignment = has_host && alignment > 1 && device_addr % alignment != 0;
            const bool fast = needs_alignment ||
                              (has_host && size <= skip_size &&
                               !rasterizer.buffer_cache.MtIsRegionGpuModified(device_addr, size));
            VideoCommon::MT::UboEntry entry{
                .stage = static_cast<u8>(stage),
                .index = static_cast<u8>(index),
                .staged = static_cast<u8>(fast ? 1 : 0),
                .memo = 0,
                .size = size,
                .device_addr = device_addr,
                .offset = 0,
                .pad2 = 0,
            };
            if (fast) {
                SeqStageUniform(entry);
            } else {
                SeqTakeUploads(device_addr, size);
            }
            ubos.push_back(entry);
        }
        // Then the stage's storage buffers: synchronized, then marked written.
        for (; ssbo_cursor < ssbos.size() && ssbos[ssbo_cursor].stage == stage; ++ssbo_cursor) {
            const SsboCapture& ssbo = ssbos[ssbo_cursor];
            SeqTakeUploads(ssbo.device_addr, ssbo.size);
            if (ssbo.is_written) {
                rasterizer.buffer_cache.MtCaptureWrittenRange(ssbo.device_addr, ssbo.size);
            }
        }
    }

    if (lite_seq_lock) {
        // EDEN_NX2_MT_LITE_SEQLOCK: the rest (upload bytes, overlay) is the sequencer's own.
        lock.reset();
        SeqReadDeferredUploads();
    }
    // EDEN_NX2_MT_COMPACT: the compact header's narrow counts must hold this draw's (else the
    // old layout, which every reader still parses; it touches no replica).
    compact_packet = compact && ubos.size() <= 0xff && ssbo_entries.size() <= 0xff &&
                     geometry_entries.size() <= 0xff && tic_valid.size() <= 0xff &&
                     raw_reads.size() <= 0xffff;
    if (compact_packet) {
        FinishOverlayCompact();
    } else {
        FinishOverlay();
    }
    return -1;
}


inline void Nx2DrawPipeline::FinishOverlay() {
    // The overlay: reads sorted by address, overlapping ones merged.
    std::sort(raw_reads.begin(), raw_reads.end(),
              [](const RawRead& a, const RawRead& b) { return a.gpu_addr < b.gpu_addr; });
    for (const RawRead& read : raw_reads) {
        if (!overlay.empty()) {
            auto& last = overlay.back();
            const u64 last_end = last.gpu_addr + last.size;
            if (read.gpu_addr <= last_end) {
                const u64 read_end = read.gpu_addr + read.size;
                if (read_end > last_end) {
                    const u32 extra = static_cast<u32>(read_end - last_end);
                    blob.insert(blob.end(), raw_bytes.begin() + read.offset + (read.size - extra),
                                raw_bytes.begin() + read.offset + read.size);
                    last.size += extra;
                }
                continue;
            }
            blob.resize((blob.size() + 7) & ~size_t{7});
        }
        overlay.push_back({read.gpu_addr, read.size, static_cast<u32>(blob.size())});
        blob.insert(blob.end(), raw_bytes.begin() + read.offset,
                    raw_bytes.begin() + read.offset + read.size);
    }
}

inline void Nx2DrawPipeline::FinishOverlayCompact() {
    using Replica = VideoCommon::MT::DescriptorReplica;
    const size_t count = raw_reads.size();
    std::vector<u8>& eligible = desc_eligible;
    // EDEN_NX2_MT_LITE_CAPMEMO: the order and the eligibility depend only on the reads' addresses,
    // sizes, kinds and offsets, not on their bytes: an identical list (same pipeline and
    // bindings, the common case) reuses them.
    OverlayMemo* memo = nullptr;
    if (lite_cap_memo) {
        u64 hash = count * 0x9E3779B97F4A7C15ull;
        for (const RawRead& read : raw_reads) {
            hash = (hash ^ read.gpu_addr) * 0x100000001B3ull;
            hash = (hash ^ (u64{read.size} << 33 | u64{read.offset} << 1 | u64{read.descriptor})) *
                   0x100000001B3ull;
        }
        memo = &overlay_memo[(hash >> 40) & (overlay_memo.size() - 1)];
        const auto same = [](const RawRead& a, const RawRead& b) {
            return a.gpu_addr == b.gpu_addr && a.size == b.size && a.offset == b.offset &&
                   a.descriptor == b.descriptor;
        };
        if (memo->hash == hash && memo->reads.size() == count &&
            std::equal(raw_reads.begin(), raw_reads.end(), memo->reads.begin(), same)) {
            raw_reads.assign(memo->sorted.begin(), memo->sorted.end());
            eligible.assign(memo->eligible.begin(), memo->eligible.end());
            if (VideoCommon::MT::Stats()) {
                counters.overlay_memo_hits.fetch_add(1, std::memory_order_relaxed);
            }
            FinishOverlayCompactSlots();
            return;
        }
        memo->hash = hash;
        memo->reads.assign(raw_reads.begin(), raw_reads.end());
    }
    // Same order as FinishOverlay (address), ties broken so duplicates are adjacent.
    std::sort(raw_reads.begin(), raw_reads.end(), [](const RawRead& a, const RawRead& b) {
        return a.gpu_addr != b.gpu_addr ? a.gpu_addr < b.gpu_addr : a.size < b.size;
    });
    // A TIC/TSC read may travel as a slot reference only when it stands alone in the overlay:
    // no other read overlaps it, and no other kind of read touches it (FinishOverlay merges
    // touching reads, and a reader could read across them). Descriptor reads are exactly the
    // 32-byte entries the readers ask for, so touching ones may stay separate entries.
    eligible.assign(count, 0);
    u64 max_end_plain = 0; // end of the reads before, by kind
    u64 max_end_desc = 0;
    for (size_t i = 0; i < count; ++i) {
        const RawRead& read = raw_reads[i];
        const u64 end = read.gpu_addr + read.size;
        if (read.descriptor && read.size == Replica::EntryBytes) {
            const bool duplicate = i > 0 && raw_reads[i - 1].descriptor &&
                                   raw_reads[i - 1].gpu_addr == read.gpu_addr &&
                                   raw_reads[i - 1].size == read.size;
            bool alone = duplicate ? eligible[i - 1] != 0
                                   : max_end_plain < read.gpu_addr && max_end_desc <= read.gpu_addr;
            if (alone && !duplicate) {
                for (size_t j = i + 1; j < count && raw_reads[j].gpu_addr <= end; ++j) {
                    const RawRead& next = raw_reads[j];
                    const bool same = next.descriptor && next.gpu_addr == read.gpu_addr &&
                                      next.size == read.size;
                    if (same) {
                        continue;
                    }
                    if (next.descriptor ? next.gpu_addr < end : next.gpu_addr <= end) {
                        alone = false;
                        break;
                    }
                }
            }
            eligible[i] = alone ? 1 : 0;
            max_end_desc = (std::max)(max_end_desc, end);
        } else {
            max_end_plain = (std::max)(max_end_plain, end);
        }
    }
    if (memo != nullptr) {
        memo->sorted.assign(raw_reads.begin(), raw_reads.end());
        memo->eligible.assign(eligible.begin(), eligible.end());
    }
    FinishOverlayCompactSlots();
}

inline void Nx2DrawPipeline::FinishOverlayCompactSlots() {
    using Replica = VideoCommon::MT::DescriptorReplica;
    const size_t count = raw_reads.size();
    const std::vector<u8>& eligible = desc_eligible;
    // Pass 1, against the table as the readers have it before this packet: references.
    // Pass 2: the misses replace their slots (in this order on every reader).
    desc_refs.clear();
    desc_inserts.clear();
    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
        const RawRead read = raw_reads[i];
        if (eligible[i] != 0) {
            if (i > 0 && eligible[i - 1] != 0 && raw_reads[i - 1].gpu_addr == read.gpu_addr) {
                continue; // a duplicate of the entry just handled
            }
            const u32 slot = Replica::SlotOf(read.gpu_addr);
            const Replica::Slot& entry = seq_desc.slots[slot];
            if (entry.gpu_addr == read.gpu_addr &&
                std::memcmp(entry.bytes.data(), raw_bytes.data() + read.offset,
                            Replica::EntryBytes) == 0) {
                desc_refs.push_back(static_cast<u16>(slot));
                continue;
            }
            desc_inserts.push_back(read.gpu_addr);
        }
        raw_reads[kept++] = read;
    }
    raw_reads.resize(kept);
    for (const u64 gpu_addr : desc_inserts) {
        Replica::Slot& entry = seq_desc.slots[Replica::SlotOf(gpu_addr)];
        // The bytes this packet carries for it (its read is among the kept ones).
        for (const RawRead& read : raw_reads) {
            if (read.descriptor && read.gpu_addr == gpu_addr) {
                entry.gpu_addr = gpu_addr;
                std::memcpy(entry.bytes.data(), raw_bytes.data() + read.offset,
                            Replica::EntryBytes);
                break;
            }
        }
    }
    if (VideoCommon::MT::Stats()) {
        counters.desc_hits.fetch_add(desc_refs.size(), std::memory_order_relaxed);
        counters.desc_misses.fetch_add(desc_inserts.size(), std::memory_order_relaxed);
    }
    // The rest as FinishOverlay does (raw_reads stay sorted).
    FinishOverlay();
}

inline void Nx2DrawPipeline::EmitDraw(GraphicsPipeline* pipeline, bool is_indexed,
                                      u32 instance_count, const IndirectParams* indirect) {
    Maxwell3D& real = *rasterizer.maxwell3d.Real();
    const auto& state = real.draw_manager.draw_state;
    const size_t captured_blob = blob.size();
    const u32 inline_offset = static_cast<u32>((blob.size() + 7) & ~size_t{7});
    const u32 inline_size = static_cast<u32>(state.inline_index_draw_indexes.size());
    if (inline_size != 0) {
        blob.resize(inline_offset);
        blob.insert(blob.end(), state.inline_index_draw_indexes.begin(),
                    state.inline_index_draw_indexes.end());
    }
    blob.resize((blob.size() + 7) & ~size_t{7});
    EmitDeltas();
    const u32 overlay_words = static_cast<u32>(overlay.size() * sizeof(overlay[0]) / 8);
    const u32 ubo_words = static_cast<u32>(ubos.size() * sizeof(ubos[0]) / 8);
    const u32 upload_words = static_cast<u32>(uploads.size() * sizeof(uploads[0]) / 8);
    const u32 blob_words = static_cast<u32>(blob.size() / 8);
    const u32 ssbo_words = static_cast<u32>(ssbo_entries.size() * sizeof(ssbo_entries[0]) / 8);
    const u32 geometry_words =
        static_cast<u32>(geometry_entries.size() * sizeof(geometry_entries[0]) / 8);
    const u32 tic_words = static_cast<u32>(tic_valid.size());
    const bool packed = compact_packet;
    const u32 header_words =
        packed ? CompactHeaderWords + (indirect != nullptr ? IndirectWords : 0) : DrawHeaderWords;
    const u32 ref_words = packed ? static_cast<u32>((desc_refs.size() * 2 + 7) / 8) : 0;
    const u32 insert_words = packed ? static_cast<u32>(desc_inserts.size()) : 0;
    const u32 words = 1 + header_words + overlay_words + ubo_words + upload_words + ssbo_words +
                      geometry_words + tic_words + ref_words + insert_words + blob_words;
    const u64 seq = next_seq;
    if (Batching()) {
        // Back-pressure: the resolved slot of `seq` must have been committed. One wait for
        // everything emitted is enough (every draw before is committed then); a loop on
        // committed_seq spun forever when compute packets (which take seqs too, but do not
        // store committed_seq) followed the last draw.
        if (!VideoCommon::MT::SeqSlotRing<char, ResolvedSlots>::HasRoom(
                seq, committed_seq.load(std::memory_order_acquire),
                2 * VideoCommon::MT::BatchSize())) {
            counters.ring_waits.fetch_add(1, std::memory_order_relaxed);
            VideoCommon::MT::WaitWatch::Scope watch_scope{
                watch, VideoCommon::MT::WaitSite::SeqBackPressure, seq};
            WaitCommitted();
        }
        batch_open = true;
    }
    u64* const out = stream.Reserve(words);
    out[0] = SpmcStream::MakeHeader(static_cast<u8>(MT::StreamEntry::DrawPacket), words,
                                    packed ? CompactAux : 0);
    u64* cursor = out + 1 + header_words;
    if (packed) {
        CompactDrawHeader header{};
        header.seq = next_seq++;
        emitted_seq.store(header.seq, std::memory_order_relaxed);
        header.pipeline = pipeline;
        header.batch = Batching() ? batch_id : 0;
        header.layout_gen = rasterizer.gpu_memory->LayoutGeneration();
        header.blob_size = static_cast<u32>(blob.size());
        header.instance_count = instance_count;
        header.num_overlay = static_cast<u32>(overlay.size());
        header.num_upload = static_cast<u32>(uploads.size());
        header.num_desc_refs = static_cast<u16>(desc_refs.size());
        header.num_desc_inserts = static_cast<u16>(desc_inserts.size());
        header.num_ubo = static_cast<u8>(ubos.size());
        header.num_ssbo = static_cast<u8>(ssbo_entries.size());
        header.num_geometry = static_cast<u8>(geometry_entries.size());
        header.tic_valid_words = static_cast<u8>(tic_words);
        header.flags = static_cast<u8>(
            (is_indexed ? CompactDrawHeader::IsIndexed : 0) |
            (real.GetNativeMacroEngine().native_turing ? CompactDrawHeader::NativeTuring : 0) |
            (indirect != nullptr ? CompactDrawHeader::Indirect : 0) |
            (state.draw_indexed ? CompactDrawHeader::DrawIndexed : 0));
        header.draw_mode = static_cast<u8>(state.draw_mode);
        header.topology = static_cast<u32>(state.topology);
        header.inline_index_offset = inline_offset;
        header.inline_index_size = inline_size;
        header.draw_element_count = state.draw_element_count;
        header.base_index = state.base_index;
        header.vertex_buffer = state.vertex_buffer;
        header.index_buffer = state.index_buffer;
        header.base_instance = state.base_instance;
        header.draw_instance_count = state.instance_count;
        header.drawn_instances = state.drawn_instances;
        std::memset(out + 1, 0, header_words * 8);
        std::memcpy(out + 1, &header, sizeof(header));
        if (indirect != nullptr) {
            std::memcpy(out + 1 + CompactHeaderWords, indirect, sizeof(*indirect));
        }
    } else {
        DrawHeader header{};
        header.seq = next_seq++;
        emitted_seq.store(header.seq, std::memory_order_relaxed);
        header.pipeline = pipeline;
        header.draw_state = DrawStateCopy{
            .topology = state.topology,
            .draw_mode = state.draw_mode,
            .draw_indexed = state.draw_indexed,
            .draw_element_count = state.draw_element_count,
            .base_index = state.base_index,
            .vertex_buffer = state.vertex_buffer,
            .index_buffer = state.index_buffer,
            .base_instance = state.base_instance,
            .instance_count = state.instance_count,
            .drawn_instances = state.drawn_instances,
        };
        header.instance_count = instance_count;
        header.is_indexed = is_indexed ? 1 : 0;
        if (indirect != nullptr) {
            header.indirect = 1;
            header.indirect_params = *indirect;
        }
        header.native_turing = real.GetNativeMacroEngine().native_turing ? 1 : 0;
        header.inline_index_offset = inline_offset;
        header.inline_index_size = inline_size;
        header.num_overlay = static_cast<u32>(overlay.size());
        header.num_ubo = static_cast<u32>(ubos.size());
        header.num_upload = static_cast<u32>(uploads.size());
        header.blob_size = static_cast<u32>(blob.size());
        header.batch = Batching() ? batch_id : 0;
        header.layout_gen = rasterizer.gpu_memory->LayoutGeneration();
        header.num_ssbo = static_cast<u32>(ssbo_entries.size());
        header.num_geometry = static_cast<u32>(geometry_entries.size());
        header.tic_valid_words = tic_words;
        std::memset(out + 1, 0, DrawHeaderWords * 8);
        std::memcpy(out + 1, &header, sizeof(header));
    }
    std::memcpy(cursor, overlay.data(), overlay_words * 8);
    cursor += overlay_words;
    std::memcpy(cursor, ubos.data(), ubo_words * 8);
    cursor += ubo_words;
    std::memcpy(cursor, uploads.data(), upload_words * 8);
    cursor += upload_words;
    std::memcpy(cursor, ssbo_entries.data(), ssbo_words * 8);
    cursor += ssbo_words;
    std::memcpy(cursor, geometry_entries.data(), geometry_words * 8);
    cursor += geometry_words;
    std::memcpy(cursor, tic_valid.data(), tic_words * 8);
    cursor += tic_words;
    if (packed) {
        if (ref_words != 0) {
            cursor[ref_words - 1] = 0;
            std::memcpy(cursor, desc_refs.data(), desc_refs.size() * sizeof(u16));
        }
        cursor += ref_words;
        std::memcpy(cursor, desc_inserts.data(), insert_words * 8);
        cursor += insert_words;
    }
    std::memcpy(cursor, blob.data(), blob.size());
    stream.Commit();
    NoteAsyncImageWork();
    counters.packets.fetch_add(1, std::memory_order_relaxed);
    counters.packet_words.fetch_add(words, std::memory_order_relaxed);
    if (VideoCommon::MT::Stats()) {
        // Where the bytes go (blob: uploads and uniform bytes during pass 2, then the overlay's
        // bytes with their alignment, then inline indices).
        const u64 overlay_blob_bytes = captured_blob - blob_ubo_bytes - blob_upload_bytes;
        const u64 header_bytes = 8 + u64{header_words} * 8;
        const u64 overlay_bytes = u64{overlay_words + ref_words + insert_words} * 8 +
                                  overlay_blob_bytes;
        const u64 ubo_entry_bytes = u64{ubo_words} * 8;
        const u64 upload_bytes = u64{upload_words} * 8 + blob_upload_bytes;
        counters.pkt_header_bytes.fetch_add(header_bytes, std::memory_order_relaxed);
        counters.pkt_overlay_bytes.fetch_add(overlay_bytes, std::memory_order_relaxed);
        counters.pkt_ubo_entry_bytes.fetch_add(ubo_entry_bytes, std::memory_order_relaxed);
        counters.pkt_ubo_data_bytes.fetch_add(blob_ubo_bytes, std::memory_order_relaxed);
        counters.pkt_upload_bytes.fetch_add(upload_bytes, std::memory_order_relaxed);
        counters.pkt_other_bytes.fetch_add(u64{words} * 8 - header_bytes - overlay_bytes -
                                               ubo_entry_bytes - blob_ubo_bytes - upload_bytes,
                                           std::memory_order_relaxed);
        if (packed) {
            counters.compact_packets.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // EDEN_NX2_MT_BATCH_MIN: a waiting committer gets the batch once it holds a few draws (a
    // close after every draw made 1.4-draw batches, each a BatchEnd, a resolver wake-up and an
    // epoch collection).
    if (Batching() && (++batch_draws >= VideoCommon::MT::BatchSize() ||
                       (adaptive_batches && batch_draws >= VideoCommon::MT::AdaptiveBatchMin() &&
                        wanted_batch.load(std::memory_order_relaxed) == batch_id + 1))) {
        CloseBatch();
    }
    if (lockstep) {
        WaitCommitted();
    }
}

inline void Nx2DrawPipeline::SeqDraw(bool is_indexed, u32 instance_count,
                                     const IndirectParams* indirect) {
    RasterizerVulkan& r = rasterizer;
    // The draw's sync requests (CPU reads, composites) run on the committer after it.
    SCOPE_EXIT {
        r.gpu.TickWork();
    };
    const bool timed = VideoCommon::MT::Stats();
    const auto begin = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    SCOPE_EXIT {
        if (timed) {
            // Capture and emit (Serial waits included; they are also counted on their own).
            counters.seq_capture_ns.fetch_add(
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count()),
                std::memory_order_relaxed);
        }
    };
    const auto serial_draw = [&] {
        if (indirect != nullptr) {
            Serial(MT::Kind::DrawIndirect, [&] { r.DrawIndirect(); });
            return;
        }
        Serial(MT::Kind::Draw, [&] {
            r.PrepareDraw(is_indexed, [&] { r.RecordDraw(is_indexed, instance_count); });
        });
    };
    if (VideoCommon::MT::SerialDraws()) {
        ForceSerialReason(0);
        serial_draw();
        return;
    }
    r.gpu_memory->FlushCaching();
    GraphicsPipeline* const pipeline = r.pipeline_cache.CurrentGraphicsPipeline();
    if (pipeline == nullptr) {
        r.ReportDrawFailure("no pipeline");
        return;
    }
    if (!is_indexed && r.TryNx2IndirectReset()) {
        return;
    }
    if (!r.MeshDrawAllowed(pipeline)) {
        return;
    }
    const int reason = CaptureDraw(pipeline, is_indexed, indirect);
    if (reason >= 0) {
        ForceSerialReason(static_cast<u32>(reason));
        serial_draw();
        return;
    }
    EmitDraw(pipeline, is_indexed, instance_count, indirect);
}

inline Nx2DrawPipeline::PacketView Nx2DrawPipeline::ParsePacket(
    const u64* entry, VideoCommon::MT::PacketDecoder& decoder) {
    PacketView view{};
    const auto take = [](const u64*& cursor, auto& out, u32 count) {
        using T = typename std::remove_reference_t<decltype(out)>::value_type;
        out = std::span<const T>{reinterpret_cast<const T*>(cursor), count};
        cursor += count * sizeof(T) / 8;
    };
    if (SpmcStream::HeaderAux(entry[0]) != CompactAux) {
        std::memcpy(&view.header, entry + 1, sizeof(view.header));
        const DrawHeader& header = view.header;
        const u64* cursor = entry + 1 + DrawHeaderWords;
        take(cursor, view.overlay, header.num_overlay);
        take(cursor, view.ubos, header.num_ubo);
        take(cursor, view.uploads, header.num_upload);
        take(cursor, view.ssbos, header.num_ssbo);
        take(cursor, view.geometry, header.num_geometry);
        take(cursor, view.tic_valid, header.tic_valid_words);
        view.blob = reinterpret_cast<const u8*>(cursor);
        view.overlay_blob = view.blob;
        return view;
    }
    // EDEN_NX2_MT_COMPACT: rebuild the header the old layout carried, and the overlay.
    CompactDrawHeader packed;
    std::memcpy(&packed, entry + 1, sizeof(packed));
    const bool indirect = (packed.flags & CompactDrawHeader::Indirect) != 0;
    DrawHeader& header = view.header;
    header.seq = packed.seq;
    header.pipeline = packed.pipeline;
    header.draw_state = DrawStateCopy{
        .topology = static_cast<Maxwell3D::Regs::PrimitiveTopology>(packed.topology),
        .draw_mode = static_cast<Maxwell3D::DrawManager::DrawMode>(packed.draw_mode),
        .draw_indexed = (packed.flags & CompactDrawHeader::DrawIndexed) != 0,
        .draw_element_count = packed.draw_element_count,
        .base_index = packed.base_index,
        .vertex_buffer = packed.vertex_buffer,
        .index_buffer = packed.index_buffer,
        .base_instance = packed.base_instance,
        .instance_count = packed.draw_instance_count,
        .drawn_instances = packed.drawn_instances,
    };
    if (indirect) {
        std::memcpy(&header.indirect_params, entry + 1 + CompactHeaderWords,
                    sizeof(header.indirect_params));
    }
    header.instance_count = packed.instance_count;
    header.is_indexed = (packed.flags & CompactDrawHeader::IsIndexed) != 0 ? 1 : 0;
    header.native_turing = (packed.flags & CompactDrawHeader::NativeTuring) != 0 ? 1 : 0;
    header.indirect = indirect ? 1 : 0;
    header.inline_index_offset = packed.inline_index_offset;
    header.inline_index_size = packed.inline_index_size;
    header.num_ubo = packed.num_ubo;
    header.num_upload = packed.num_upload;
    header.blob_size = packed.blob_size;
    header.batch = packed.batch;
    header.layout_gen = packed.layout_gen;
    header.num_ssbo = packed.num_ssbo;
    header.num_geometry = packed.num_geometry;
    header.tic_valid_words = packed.tic_valid_words;
    const u64* cursor = entry + 1 + CompactHeaderWords + (indirect ? IndirectWords : 0);
    std::span<const VideoCommon::MT::OverlayEntry> wire;
    take(cursor, wire, packed.num_overlay);
    take(cursor, view.ubos, packed.num_ubo);
    take(cursor, view.uploads, packed.num_upload);
    take(cursor, view.ssbos, packed.num_ssbo);
    take(cursor, view.geometry, packed.num_geometry);
    take(cursor, view.tic_valid, packed.tic_valid_words);
    const std::span<const u16> refs{reinterpret_cast<const u16*>(cursor), packed.num_desc_refs};
    cursor += (size_t{packed.num_desc_refs} * 2 + 7) / 8;
    std::span<const u64> inserts;
    take(cursor, inserts, packed.num_desc_inserts);
    view.blob = reinterpret_cast<const u8*>(cursor);
    // References first (the replica as it was before this packet), then this packet's inserts.
    decoder.Decode(wire, view.blob, refs);
    decoder.ApplyInserts(wire, view.blob, inserts);
    view.overlay = decoder.overlay;
    view.overlay_blob = decoder.bytes.data();
    header.num_overlay = static_cast<u32>(view.overlay.size());
    return view;
}

inline void Nx2DrawPipeline::SkipPacket(const u64* entry,
                                        VideoCommon::MT::PacketDecoder& decoder) {
    using VideoCommon::MT::GeometryEntry;
    using VideoCommon::MT::OverlayEntry;
    using VideoCommon::MT::SsboEntry;
    using VideoCommon::MT::UboEntry;
    using VideoCommon::MT::UploadEntry;
    if (SpmcStream::HeaderAux(entry[0]) != CompactAux) {
        // The old layout touches no descriptor replica; its uniform memos still count.
        if (!decoder.uniform_kept) {
            return;
        }
        DrawHeader header;
        std::memcpy(&header, entry + 1, sizeof(header));
        const u64* cursor = entry + 1 + DrawHeaderWords + header.num_overlay * sizeof(OverlayEntry) / 8;
        const std::span<const UboEntry> ubos{reinterpret_cast<const UboEntry*>(cursor),
                                             header.num_ubo};
        cursor += (size_t{header.num_ubo} * sizeof(UboEntry) +
                   size_t{header.num_upload} * sizeof(UploadEntry) +
                   size_t{header.num_ssbo} * sizeof(SsboEntry) +
                   size_t{header.num_geometry} * sizeof(GeometryEntry)) /
                      8 +
                  header.tic_valid_words;
        decoder.ApplyUniforms(ubos, reinterpret_cast<const u8*>(cursor));
        return;
    }
    CompactDrawHeader packed;
    std::memcpy(&packed, entry + 1, sizeof(packed));
    if (packed.num_desc_inserts == 0 && !decoder.uniform_kept) {
        return;
    }
    const bool indirect = (packed.flags & CompactDrawHeader::Indirect) != 0;
    const u64* cursor = entry + 1 + CompactHeaderWords + (indirect ? IndirectWords : 0);
    const std::span<const OverlayEntry> wire{reinterpret_cast<const OverlayEntry*>(cursor),
                                             packed.num_overlay};
    cursor += size_t{packed.num_overlay} * sizeof(OverlayEntry) / 8;
    const std::span<const UboEntry> ubos{reinterpret_cast<const UboEntry*>(cursor),
                                         packed.num_ubo};
    cursor += (size_t{packed.num_ubo} * sizeof(UboEntry) +
               size_t{packed.num_upload} * sizeof(UploadEntry) +
               size_t{packed.num_ssbo} * sizeof(SsboEntry) +
               size_t{packed.num_geometry} * sizeof(GeometryEntry)) /
                  8 +
              packed.tic_valid_words + (size_t{packed.num_desc_refs} * 2 + 7) / 8;
    const std::span<const u64> inserts{cursor, packed.num_desc_inserts};
    cursor += packed.num_desc_inserts;
    const u8* const blob_data = reinterpret_cast<const u8*>(cursor);
    decoder.ApplyInserts(wire, blob_data, inserts);
    decoder.ApplyUniforms(ubos, blob_data);
}

inline void Nx2DrawPipeline::CommitDraw(const u64* entry) {
    const PacketView packet = ParsePacket(entry, commit_decoder);
    const DrawHeader& header = packet.header;
    MT::ResolvedDraw* const res = WaitResolved(header);
    SCOPE_EXIT {
        if (res != nullptr && res->result_end != 0) {
            resolvers[res->resolver]->results.Release(res->result_end);
        }
        // The sequencer may reuse this draw's resolved slot from here on.
        committed_seq.store(header.seq, std::memory_order_release);
    };
    const auto overlay_span = packet.overlay;
    const auto ubo_span = packet.ubos;
    const auto upload_span = packet.uploads;
    const u8* const blob_data = packet.blob;
    KeepUniformMemos(ubo_span, blob_data);
    if (current == nullptr) {
        return;
    }
    RasterizerVulkan& r = rasterizer;
    Maxwell3D& mirror = *current->mirror3d;
    auto& state = mirror.draw_manager.draw_state;
    const DrawStateCopy& copy = header.draw_state;
    state.topology = copy.topology;
    state.draw_mode = copy.draw_mode;
    state.draw_indexed = copy.draw_indexed;
    state.draw_element_count = copy.draw_element_count;
    state.base_index = copy.base_index;
    state.vertex_buffer = copy.vertex_buffer;
    state.index_buffer = copy.index_buffer;
    state.base_instance = copy.base_instance;
    state.instance_count = copy.instance_count;
    state.drawn_instances = copy.drawn_instances;
    state.inline_index_draw_indexes.assign(blob_data + header.inline_index_offset,
                                           blob_data + header.inline_index_offset +
                                               header.inline_index_size);
    mirror.GetNativeMacroEngine().native_turing = header.native_turing != 0;
    if (header.is_indexed != 0) {
        // The draw manager dirties the index buffer outside register writes.
        mirror.dirty.flags[VideoCommon::Dirty::IndexBuffer] = true;
    }
    if (verify) {
        VerifyDraw(header, overlay_span, ubo_span, upload_span, blob_data, packet.overlay_blob);
    }
    r.FlushWork();
    VideoCommon::MT::CaptureView view{
        .overlay = overlay_span,
        .ubos = ubo_span,
        .blob = blob_data,
        .verify = verify,
        .ubo_memo = ubo_memo_bytes.get(),
        .cpu_write_since = verify_cpu_writes_since.load(std::memory_order_acquire),
        .overlay_blob = packet.overlay_blob,
    };
    {
        // EDEN_NX2_MT_LOCKSCOPE: the uploads under the buffer mutex, then ConfigureDrawPipeline
        // takes both for the resolve only.
        std::unique_lock buffer_lock{r.buffer_cache.mutex, std::defer_lock};
        std::unique_lock texture_lock{r.texture_cache.mutex, std::defer_lock};
        if (VideoCommon::MT::LockScope()) {
            buffer_lock.lock();
            r.buffer_cache.MtApplyUploads(upload_span, blob_data);
            buffer_lock.unlock();
        } else {
            std::lock(buffer_lock, texture_lock);
            r.buffer_cache.MtApplyUploads(upload_span, blob_data);
        }
        VideoCommon::MT::t_capture = &view;
        r.mt_commit_pipeline = header.pipeline;
        SCOPE_EXIT {
            VideoCommon::MT::t_capture = nullptr;
            r.mt_commit_pipeline = nullptr;
        };
        r.mesh_draw = header.pipeline->IsMeshPipeline();
        const bool is_indexed = header.is_indexed != 0;
        // I4 before anything of this draw changes the caches (uploads were applied: a buffer
        // they joined fails here).
        MT::ValidateFail fail = MT::FailNone;
        if (res != nullptr && res->status == MT::ResolveStatus::Ok) {
            // EDEN_NX2_MT_RALIAS: PrepareImage's alias copies first; a rescale they cause
            // fails the validation below.
            try {
                SyncResolvedAliases(*res);
            } catch (const std::exception& e) {
                LOG_ERROR(Render_Vulkan, "MT: alias synchronization threw: {}", e.what());
            }
            fail = Validate(*res, header);
            counters.validate_fail[fail].fetch_add(1, std::memory_order_relaxed);
            if (fail == MT::FailNone) {
                counters.resolved_ok.fetch_add(1, std::memory_order_relaxed);
            } else if (shadow != 0) {
                counters.shadow_unvalidated.fetch_add(1, std::memory_order_relaxed);
                if (shadow >= 2) {
                    VerifyReport(0x5100 | fail,
                                 fmt::format("MT shadow: draw {} batch {} failed validation: {}",
                                             header.seq, header.batch,
                                             MT::ValidateFailNames[fail]));
                }
            }
        } else if (res != nullptr) {
            counters.resolved_slow.fetch_add(1, std::memory_order_relaxed);
        }
        const bool compare = res != nullptr && shadow != 0 &&
                             res->status == MT::ResolveStatus::Ok && fail == MT::FailNone;
        if (compare) {
            shadow_sink.filled = false;
            shadow_sink.images_recorded = false;
            shadow_sink.stage_image_count.fill(0);
            GraphicsPipeline::mt_shadow_sink = &shadow_sink;
        }
        GraphicsPipeline::MtResolvedTextures resolved_textures;
        GraphicsPipeline::MtResolvedDraw resolved_draw;
        struct BufferContext {
            Nx2DrawPipeline* self;
            const MT::ResolvedDraw* res;
        } buffer_context{this, res};
        const bool indirect = header.indirect != 0;
        if (res != nullptr && resolved_commit && shadow == 0 &&
            res->status == MT::ResolveStatus::Ok && fail == MT::FailNone) {
            PrepareResolvedTextures(*res, resolved_textures);
            counters.resolved_committed.fetch_add(1, std::memory_order_relaxed);
            if (res->buffer_fail == MT::BufNone && !indirect) {
                // EDEN_NX2_MT_RBUF: the buffer half too; the committer only applies intents.
                resolved_draw = GraphicsPipeline::MtResolvedDraw{
                    .textures = &resolved_textures,
                    .payload = res->payload,
                    .num_payload = res->num_payload,
                    .apply_buffers =
                        [](void* context, bool draw_indexed) {
                            const auto& ctx = *static_cast<const BufferContext*>(context);
                            ctx.self->ApplyResolvedBuffers(*ctx.res, draw_indexed);
                        },
                    .context = &buffer_context,
                };
                GraphicsPipeline::mt_resolved_draw = &resolved_draw;
                counters.resolved_buffers.fetch_add(1, std::memory_order_relaxed);
            } else {
                GraphicsPipeline::mt_resolved_textures = &resolved_textures;
            }
        }
        if (indirect) {
            mirror.draw_manager.indirect_state = header.indirect_params;
            r.buffer_cache.SetDrawIndirect(&mirror.draw_manager.indirect_state);
        }
        try {
            if (r.ConfigureDrawPipeline(header.pipeline, is_indexed)) {
                if (indirect) {
                    r.RecordDrawIndirect(mirror.draw_manager.indirect_state);
                } else {
                    r.RecordDraw(is_indexed, header.instance_count);
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR(Render_Vulkan, "MT: draw packet threw: {}", e.what());
        }
        GraphicsPipeline::mt_shadow_sink = nullptr;
        GraphicsPipeline::mt_resolved_textures = nullptr;
        GraphicsPipeline::mt_resolved_draw = nullptr;
        if (compare && shadow_sink.filled) {
            ShadowCompare(*res, header, shadow_sink);
        }
        if (indirect) {
            r.buffer_cache.SetDrawIndirect(nullptr);
        }
    }
    if (verify) {
        ReportView(header.seq, view);
    }
    if (++collect_countdown >= 16) {
        collect_countdown = 0;
        CollectEpochs();
    }
}

inline void Nx2DrawPipeline::ReportView(u64 seq, const VideoCommon::MT::CaptureView& view) {
    struct {
        u64 seq;
    } header{seq};
    counters.verify_cpu_writes.fetch_add(view.concurrent_cpu_syncs, std::memory_order_relaxed);
    {
        const u64 misses = view.overlay_misses + view.ubo_misses + view.decision_mismatches +
                           view.uncaptured_syncs;
        if (misses != 0) {
            VerifyReport(0x100 | (view.overlay_misses != 0 ? 1 : 0) | (view.ubo_misses != 0 ? 2 : 0) |
                             (view.decision_mismatches != 0 ? 4 : 0) |
                             (view.uncaptured_syncs != 0 ? 8 : 0),
                         fmt::format("draw {}: overlay misses {}, uniform misses {}, uniform "
                                     "decisions {}, uncaptured syncs {} (first {:#x} +{})",
                                     header.seq, view.overlay_misses, view.ubo_misses,
                                     view.decision_mismatches, view.uncaptured_syncs,
                                     view.first_uncaptured_addr, view.first_uncaptured_size));
        }
    }
}

inline void Nx2DrawPipeline::VerifyReport(u64 key, const std::string& message) {
    counters.verify_mismatches.fetch_add(1, std::memory_order_relaxed);
    static std::unordered_map<u64, u32> reported;
    if (reported.size() < 512 && reported[key]++ < 4) {
        LOG_WARNING(Render_Vulkan, "MT verify: {}", message);
    }
}

inline void Nx2DrawPipeline::VerifyDraw(const DrawHeader& header,
                                        std::span<const VideoCommon::MT::OverlayEntry> overlay_span,
                                        std::span<const VideoCommon::MT::UboEntry> ubo_span,
                                        std::span<const VideoCommon::MT::UploadEntry> upload_span,
                                        const u8* blob_data, const u8* overlay_blob) {
    // The sequencer waits for this packet (lockstep): its state equals the capture's.
    counters.verify_checks.fetch_add(1, std::memory_order_relaxed);
    VerifyMirror("draw");
    const Maxwell3D& real = *current->real3d;
    const Maxwell3D& mirror = *current->mirror3d;
    for (size_t stage = 0; stage < real.state.shader_stages.size(); ++stage) {
        for (size_t slot = 0; slot < real.state.shader_stages[stage].const_buffers.size(); ++slot) {
            const auto& a = real.state.shader_stages[stage].const_buffers[slot];
            const auto& b = mirror.state.shader_stages[stage].const_buffers[slot];
            if (a.address != b.address || a.size != b.size || a.enabled != b.enabled) {
                VerifyReport(0x1000 + stage * 32 + slot,
                             fmt::format("draw {}: const buffer {}/{} real {:#x}/{:#x}/{} mirror "
                                         "{:#x}/{:#x}/{}",
                                         header.seq, stage, slot, a.address, a.size, a.enabled,
                                         b.address, b.size, b.enabled));
            }
        }
    }
    if (real.root_tables.tables != mirror.root_tables.tables ||
        real.root_tables.loaded != mirror.root_tables.loaded) {
        VerifyReport(0x2000, fmt::format("draw {}: root tables differ", header.seq));
    }
    const auto& state = real.draw_manager.draw_state;
    const DrawStateCopy& copy = header.draw_state;
    if (state.topology != copy.topology || state.draw_mode != copy.draw_mode ||
        state.base_index != copy.base_index || state.base_instance != copy.base_instance ||
        std::memcmp(&state.vertex_buffer, &copy.vertex_buffer, sizeof(copy.vertex_buffer)) != 0 ||
        std::memcmp(&state.index_buffer, &copy.index_buffer, sizeof(copy.index_buffer)) != 0 ||
        state.inline_index_draw_indexes.size() != header.inline_index_size) {
        VerifyReport(0x2001, fmt::format("draw {}: draw state differs", header.seq));
    }
    VerifyCaptures(header.seq, overlay_span, ubo_span, upload_span, blob_data, overlay_blob);
}

inline void Nx2DrawPipeline::VerifyCaptures(
    u64 seq, std::span<const VideoCommon::MT::OverlayEntry> overlay_span,
    std::span<const VideoCommon::MT::UboEntry> ubo_span,
    std::span<const VideoCommon::MT::UploadEntry> upload_span, const u8* blob_data,
    const u8* overlay_blob) {
    struct {
        u64 seq;
    } header{seq};
    std::vector<u8> bytes;
    const u8* const overlay_bytes = overlay_blob != nullptr ? overlay_blob : blob_data;
    // Bytes that differ from the capture are a capture bug unless a guest CPU thread wrote them
    // since (the sequencer waits for this packet; the game's threads don't): its write trap
    // marked them CPU-modified in the buffer tracker.
    const auto written_by_cpu = [&](DAddr device_addr, u64 size) {
        if (VideoCommon::MT::GuestCpuWrites().WrittenSince(
                verify_cpu_writes_since.load(std::memory_order_acquire), device_addr, size)) {
            return true;
        }
        std::scoped_lock lock{rasterizer.buffer_cache.mutex};
        return rasterizer.buffer_cache.IsRegionCpuModified(device_addr, size);
    };
    for (const auto& entry : overlay_span) {
        bytes.resize(entry.size);
        rasterizer.gpu_memory->ReadBlockUnsafe(entry.gpu_addr, bytes.data(), entry.size);
        if (std::memcmp(bytes.data(), overlay_bytes + entry.offset, entry.size) != 0) {
            const auto device_addr = rasterizer.gpu_memory->GpuToCpuAddress(entry.gpu_addr);
            if (device_addr && written_by_cpu(*device_addr, entry.size)) {
                counters.verify_cpu_writes.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            VerifyReport(0x3000, fmt::format("draw {}: guest bytes at {:#x} (+{}) changed since "
                                             "the capture",
                                             header.seq, entry.gpu_addr, entry.size));
        }
    }
    for (const auto& entry : ubo_span) {
        if (entry.staged == 0) {
            continue;
        }
        bytes.resize(entry.size);
        rasterizer.device_memory.ReadBlockUnsafe(entry.device_addr, bytes.data(), entry.size);
        const VideoCommon::MT::CaptureView memo_view{
            .overlay = {}, .ubos = {}, .blob = blob_data, .verify = false,
            .ubo_memo = ubo_memo_bytes.get()};
        if (std::memcmp(bytes.data(), memo_view.UboBytes(entry), entry.size) != 0) {
            if (written_by_cpu(entry.device_addr, entry.size)) {
                counters.verify_cpu_writes.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            VerifyReport(0x3001, fmt::format("draw {}: uniform buffer {}/{} at {:#x} changed",
                                             header.seq, entry.stage, entry.index,
                                             entry.device_addr));
        }
    }
    for (const auto& entry : upload_span) {
        bytes.resize(entry.size);
        rasterizer.device_memory.ReadBlockUnsafe(entry.device_addr, bytes.data(), entry.size);
        const u8* const data = entry.heap != 0 ? reinterpret_cast<const u8*>(entry.data)
                                               : blob_data + entry.data;
        if (std::memcmp(bytes.data(), data, entry.size) != 0) {
            if (written_by_cpu(entry.device_addr, entry.size)) {
                counters.verify_cpu_writes.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            u32 first_diff = 0;
            u32 diff_bytes = 0;
            for (u32 i = entry.size; i-- > 0;) {
                if (bytes[i] != data[i]) {
                    first_diff = i;
                    ++diff_bytes;
                }
            }
            u64 was = 0;
            u64 now = 0;
            std::memcpy(&was, data + (first_diff & ~7u), std::min<u32>(8, entry.size - (first_diff & ~7u)));
            std::memcpy(&now, bytes.data() + (first_diff & ~7u),
                        std::min<u32>(8, entry.size - (first_diff & ~7u)));
            VerifyReport(0x3002, fmt::format("draw {}: upload at {:#x} (+{}) changed: {} bytes "
                                             "from +{:#x} ({:#018x} -> {:#018x})",
                                             header.seq, entry.device_addr, entry.size, diff_bytes,
                                             first_diff, was, now));
        }
    }
}

inline void Nx2DrawPipeline::SeqInnerInvalidation(
    std::span<const std::pair<DAddr, std::size_t>> sequences) {
    RasterizerVulkan& r = rasterizer;
    if (Settings::values.skip_cpu_inner_invalidation.GetValue()) {
        return;
    }
    // EDEN_NX2_MT_INVAL_TEXFILTER: the texture mutex (contended with the committer) only for
    // ranges that may hold an image; I5's BeforeWrite already trusts Nx2MayHoldTexture.
    const bool fast = Tegra::Nx2InvalFast() || VideoCommon::MT::InvalTextureFilter();
    const auto may_hold_texture = [fast](DAddr addr, u64 size) {
        return !fast || Tegra::Nx2MayHoldTexture(addr, size);
    };
    // Textures: the guest write already waited for the committer (BeforeWrite).
    if (std::ranges::any_of(sequences, [&](const auto& seq) {
            return may_hold_texture(seq.first, seq.second);
        })) {
        std::scoped_lock lock{r.texture_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            if (may_hold_texture(addr, size)) {
                r.texture_cache.WriteMemory(addr, size);
            }
        }
    }
    SeqInvalidateBuffers(sequences);
    // Queries: pages that ever held one (set before the entry goes in, so a query the committer
    // has not inserted yet is covered by checking for reports still in flight).
    const bool queries_in_flight = stream.ConsumerPosition(consumer) < last_query_position;
    std::vector<std::pair<DAddr, std::size_t>>* query_ranges = nullptr;
    for (const auto& [addr, size] : sequences) {
        if (queries_in_flight || Tegra::Nx2AnyPage(Tegra::g_nx2_query_pages, addr, size)) {
            if (query_ranges == nullptr) {
                query_ranges = new std::vector<std::pair<DAddr, std::size_t>>();
            }
            query_ranges->emplace_back(addr, size);
        }
    }
    if (query_ranges != nullptr) {
        Command(MT::Kind::Query, [rp = &r, query_ranges] {
            for (const auto& [addr, size] : *query_ranges) {
                rp->query_cache.InvalidateRegion(addr, size);
            }
            delete query_ranges;
        });
    }
    r.pipeline_cache.InvalidateRegions(sequences);
}

} // namespace Vulkan

namespace Vulkan {

inline bool Nx2DrawPipeline::SeqInlineToMemory(GPUVAddr address, std::optional<DAddr> cpu_addr,
                                               size_t copy_size, std::span<const u8> memory) {
    RasterizerVulkan& r = rasterizer;
    if (!cpu_addr) {
        return false;
    }
    // As AccelerateInlineToMemory: writes into GPU-written buffer data are uploaded into the
    // buffer (committer); the rest only invalidate, at the next FlushCaching.
    static const bool defer = [] {
        const char* value = std::getenv("EDEN_NX2_INLINE_DEFER");
        return value == nullptr || value[0] != '0';
    }();
    static const char inline_mode = Tegra::Nx2EnvMode("EDEN_NX2_INLINE_FAST");
    const bool inline_fast = Tegra::Nx2Switch(inline_mode, true);
    if (!defer || !inline_fast || !Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
        return false;
    }
    const u64 last = (*cpu_addr + copy_size + Core::DEVICE_PAGESIZE - 1) &
                     ~u64{Core::DEVICE_PAGEMASK};
    for (u64 page = *cpu_addr & ~u64{Core::DEVICE_PAGEMASK}; page < last;
         page += u64{1} << Tegra::Nx2WrittenPageBits) {
        if (Tegra::Nx2IsGpuWritten(page)) {
            return false;
        }
    }
    // The memory manager's write hook waits for the committer first over image/command-list
    // pages (I5, R4).
    r.gpu_memory->WriteBlockUnsafe(address, memory.data(), copy_size);
    r.buffer_cache.Nx2NoteListHostWrite(*cpu_addr, copy_size);
    if (stream.ConsumerPosition(consumer) < last_query_position ||
        Tegra::Nx2AnyPage(Tegra::g_nx2_query_pages, *cpu_addr, copy_size)) {
        // Pending query results must not land over the new words later.
        Command(MT::Kind::Query, [rp = &r, addr = *cpu_addr, copy_size] {
            rp->query_cache.InvalidateRegion(addr, copy_size);
        });
    }
    r.gpu_memory->AccumulateInvalidation(address, copy_size);
    return true;
}

} // namespace Vulkan

namespace Vulkan {

inline bool Nx2DrawPipeline::SeqPagesMayHoldGpuData(DAddr device_addr, u64 size) const {
    if (!lite_seq_lock || size == 0 || !Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
        return true;
    }
    // EDEN_NX2_MT_LITE_SEQLOCK: MtPageHoldsGpuData asks whether GPU-modified ranges touch the
    // range's pages. Those ranges only lie on pages marked GPU-written (never cleared; the test
    // IsRegionGpuModified and SeqMayHoldGpuBufferData make with EDEN_NX2_GPU_MODIFIED_FAST).
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_GPU_MODIFIED_FAST");
    if (!Tegra::Nx2Switch(mode, true)) {
        return true;
    }
    const DAddr end = Common::AlignUp(device_addr + size, Core::DEVICE_PAGESIZE);
    for (DAddr page = Common::AlignDown(device_addr, Core::DEVICE_PAGESIZE); page < end;
         page += u64{1} << Tegra::Nx2WrittenPageBits) {
        if (Tegra::Nx2IsGpuWritten(page)) {
            return true;
        }
    }
    return false;
}

inline void Nx2DrawPipeline::SeqInvalidateBuffers(
    std::span<const std::pair<DAddr, std::size_t>> regions) {
    RasterizerVulkan& r = rasterizer;
    // GPU data on the written pages is downloaded first, which only the scheduler's owner may
    // do: those ranges go to the committer.
    boost::container::small_vector<std::pair<DAddr, std::size_t>, 8> owner_ranges;
    if (VideoCommon::MT::TrapLock()) {
        // The tracker only; ranges with GPU-written data need the buffer cache mutex.
        using Result = BufferCache::MtTrapResult;
        boost::container::small_vector<std::pair<DAddr, std::size_t>, 8> mutex_ranges;
        {
            // EDEN_NX2_MT_INVAL_ONELOCK: one hold of the (recursive) tracker lock for all the
            // ranges; the calls below take it again cheaply. Nothing here takes a cache mutex.
            // EDEN_NX2_MT_LITE_SEQLOCK: one hold for a single range too (MtPageHoldsGpuData and
            // MtTrapWrite took it twice, the first often contended with the committer).
            std::optional<VideoCommon::MT::TrackerScope> one_lock;
            if ((regions.size() > 1 || (lite_seq_lock && !regions.empty())) &&
                VideoCommon::MT::InvalOneLock()) {
                one_lock.emplace(r.buffer_cache.MtTrackerMutex());
            }
            for (const auto& [addr, size] : regions) {
                if (SeqPagesMayHoldGpuData(addr, size) &&
                    r.buffer_cache.MtPageHoldsGpuData(addr, size)) {
                    owner_ranges.emplace_back(addr, size);
                } else if (r.buffer_cache.MtTrapWrite(addr, size, false) == Result::NeedsMutex) {
                    mutex_ranges.emplace_back(addr, size);
                }
            }
        }
        if (!mutex_ranges.empty()) {
            counters.seq_mutex_writes.fetch_add(mutex_ranges.size(), std::memory_order_relaxed);
            std::scoped_lock lock{r.buffer_cache.mutex};
            for (const auto& [addr, size] : mutex_ranges) {
                r.buffer_cache.WriteMemory(addr, size);
            }
        }
    } else {
        std::scoped_lock lock{r.buffer_cache.mutex};
        for (const auto& [addr, size] : regions) {
            if (r.buffer_cache.MtPageHoldsGpuData(addr, size)) {
                owner_ranges.emplace_back(addr, size);
            } else {
                r.buffer_cache.WriteMemory(addr, size);
            }
        }
    }
    if (!owner_ranges.empty()) {
        counters.inval_owner_serials.fetch_add(1, std::memory_order_relaxed);
        Serial(MT::Kind::InvalidateRegion, [&] {
            std::scoped_lock lock{r.buffer_cache.mutex};
            for (const auto& [addr, size] : owner_ranges) {
                r.buffer_cache.WriteMemory(addr, size);
            }
        });
    }
}

inline void Nx2DrawPipeline::SeqCacheInvalidations(
    std::span<const std::pair<DAddr, std::size_t>> regions) {
    RasterizerVulkan& r = rasterizer;
    if (std::ranges::any_of(regions, [](const auto& region) {
            return region.first != 0 && region.second != 0 &&
                   Tegra::Nx2MayHoldTexture(region.first, region.second);
        })) {
        // Draws captured before these writes must not see them in a refreshed image.
        counters.write_barriers.fetch_add(1, std::memory_order_relaxed);
        WaitCommitted();
        std::scoped_lock lock{r.texture_cache.mutex};
        for (const auto& [addr, size] : regions) {
            if (addr != 0 && size != 0) {
                r.texture_cache.WriteMemory(addr, size);
            }
        }
    }
    SeqInvalidateBuffers(regions);
    r.pipeline_cache.InvalidateRegions(regions);
}

} // namespace Vulkan

namespace Vulkan {

inline void Nx2DrawPipeline::ClearCapture() {
    compact_packet = false;
    defer_upload_reads = false;
    desc_refs.clear();
    desc_inserts.clear();
    blob_ubo_bytes = 0;
    blob_upload_bytes = 0;
    raw_reads.clear();
    raw_bytes.clear();
    overlay.clear();
    ubos.clear();
    uploads.clear();
    blob.clear();
    taken_ranges.clear();
    ssbo_entries.clear();
    geometry_entries.clear();
    tic_valid.clear();
    tic_valid_count = 0;
}

inline bool Nx2DrawPipeline::SeqTicValid(const u8* tic_bytes) {
    TicValidMemo key{};
    std::memcpy(key.raw.data(), tic_bytes, 32);
    key.layout_gen = rasterizer.gpu_memory->LayoutGeneration();
    key.manager = rasterizer.gpu_memory;
    u64 hash = key.layout_gen * 0x9E3779B97F4A7C15ull;
    for (const u64 word : key.raw) {
        hash = (hash ^ word) * 0x100000001B3ull;
    }
    TicValidMemo& memo = tic_valid_memo[(hash >> 32) & (tic_valid_memo.size() - 1)];
    // Only an odd (stable) layout generation is memoized.
    if ((key.layout_gen & 1) != 0 && memo.layout_gen == key.layout_gen && memo.raw == key.raw &&
        memo.manager == key.manager) {
        return memo.valid;
    }
    Tegra::Texture::TICEntry entry;
    std::memcpy(&entry, tic_bytes, sizeof(entry));
    key.valid = VideoCommon::IsValidEntry(*rasterizer.gpu_memory, entry);
    memo = key;
    return key.valid;
}

inline bool Nx2DrawPipeline::SeqIsContinuous(u64 gpu_addr, u64 size) {
    Tegra::MemoryManager& memory = *rasterizer.gpu_memory;
    if (!VideoCommon::MT::ContinuousMemo()) {
        return memory.IsContinuousRange(gpu_addr, size);
    }
    // As SeqTicValid: only an odd (stable) generation read before the walk is memoized; a layout
    // change during the walk leaves an entry no later lookup matches.
    const u64 layout_gen = memory.LayoutGeneration();
    const u64 hash = ((gpu_addr >> 8) ^ (size * 0x9E3779B97F4A7C15ull)) * 0x100000001B3ull;
    ContinuousMemo& memo = continuous_memo[(hash >> 40) & (continuous_memo.size() - 1)];
    if ((layout_gen & 1) != 0 && memo.layout_gen == layout_gen && memo.gpu_addr == gpu_addr &&
        memo.size == size && memo.manager == &memory) {
        return memo.continuous;
    }
    const bool continuous = memory.IsContinuousRange(gpu_addr, size);
    memo = ContinuousMemo{gpu_addr, size, layout_gen, &memory, continuous};
    return continuous;
}

inline int Nx2DrawPipeline::CaptureCompute(ComputePipeline* pipeline) {
    KeplerCompute& real = *rasterizer.kepler_compute.Real();
    Tegra::MemoryManager& memory = *rasterizer.gpu_memory;
    const Shader::Info& info = pipeline->GetInfo();
    const auto& qmd = real.launch_description;
    const auto& cbufs = qmd.const_buffer_config;
    ClearCapture();
    if (verify) {
        verify_cpu_writes_since.store(VideoCommon::MT::GuestCpuWrites().Snapshot(),
                                      std::memory_order_release);
    }
    // DispatchIndirect: the committer reads the arguments from the buffer cache (GPU data, or
    // the CPU-modified bytes taken below).
    DAddr indirect_device_addr = 0;
    if (const auto indirect = real.GetIndirectComputeAddress()) {
        const auto translated = memory.GpuToCpuAddress(*indirect);
        if (!translated || !memory.IsContinuousRange(*indirect, 12)) {
            return 12;
        }
        indirect_device_addr = *translated;
    }
    const auto enabled = [&](u32 cbuf) { return ((qmd.const_buffer_enable_mask >> cbuf) & 1) != 0; };
    const bool native = real.IsNativeMode();
    const bool mapping_sized = real.IsDlssDispatch();
    const bool prefers_sparse = rasterizer.buffer_cache.MtPrefersSparseSources();
    struct SsboCapture {
        DAddr device_addr;
        u32 size;
        bool is_written;
    };
    boost::container::small_vector<SsboCapture, 32> ssbos;
    for (const auto& desc : info.storage_buffers_descriptors) {
        if (desc.count != 1) {
            return 11;
        }
        if (!enabled(desc.cbuf_index)) {
            continue;
        }
        const GPUVAddr ssbo_addr = cbufs[desc.cbuf_index].Address() + desc.cbuf_offset;
        const GPUVAddr gpu_addr = SeqRead<u64>(ssbo_addr);
        if (gpu_addr == 0) {
            continue;
        }
        const u64 next_qword = mapping_sized ? 0 : SeqRead<u64>(ssbo_addr + 8);
        const VideoCommon::Binding binding = rasterizer.buffer_cache.MtStorageBinding(
            gpu_addr, next_qword, desc.cbuf_index, desc.is_written, native, mapping_sized);
        if (binding.size == 0 || binding.device_addr == 0) {
            continue;
        }
        if ((!desc.is_written || prefers_sparse) &&
            !SeqIsContinuous(binding.gpu_addr, binding.size)) {
            return 4;
        }
        ssbos.push_back({binding.device_addr, binding.size, desc.is_written});
    }
    const bool via_header_index = qmd.linked_tsc != 0;
    const u32 tic_limit = real.regs.tic.limit;
    const u32 tsc_limit = via_header_index ? tic_limit : real.regs.tsc.limit;
    const bool native_tiling = memory.UsesNativeTiling();
    const SeqRecorder recorder{this};
    const auto read_handle = [&](const auto& desc, u32 index) -> std::pair<u32, u32> {
        if (desc.handle_program != ~0U) {
            return Tegra::Texture::TexturePair(
                VideoCommon::EvaluateTextureHandle(info, desc.handle_program, recorder,
                                                   [&](u32 cbuf) -> GPUVAddr {
                                                       return cbuf < cbufs.size() && enabled(cbuf)
                                                                  ? cbufs[cbuf].Address()
                                                                  : GPUVAddr{};
                                                   }),
                via_header_index);
        }
        const u32 index_offset{index << desc.size_shift};
        const GPUVAddr addr{cbufs[desc.cbuf_index].Address() + desc.cbuf_offset + index_offset};
        if constexpr (requires { desc.has_secondary; }) {
            if (desc.has_secondary) {
                const GPUVAddr separate_addr{cbufs[desc.secondary_cbuf_index].Address() +
                                             desc.secondary_cbuf_offset + index_offset};
                const u32 lhs_raw{SeqRead<u32>(addr) << desc.shift_left};
                const u32 rhs_raw{SeqRead<u32>(separate_addr) << desc.secondary_shift_left};
                return Tegra::Texture::TexturePair(lhs_raw | rhs_raw, via_header_index);
            }
        }
        return Tegra::Texture::TexturePair(SeqRead<u32>(addr), via_header_index);
    };
    const auto capture_view = [&](u32 handle) {
        const u32 index = Tegra::Texture::TextureHeaderIndex(handle, tic_limit, native_tiling);
        if (index <= tic_limit) {
            SeqReadDescriptor(real.regs.tic.Address() + u64{index} * 32);
        }
    };
    for (const auto& desc : info.texture_descriptors) {
        for (u32 index = 0; index < desc.count; ++index) {
            const auto handle = read_handle(desc, index);
            capture_view(handle.first);
            if (handle.second <= tsc_limit) {
                SeqReadDescriptor(real.regs.tsc.Address() + u64{handle.second} * 32);
            }
        }
    }
    for (const auto& desc : info.image_descriptors) {
        for (u32 index = 0; index < desc.count; ++index) {
            capture_view(read_handle(desc, index).first);
        }
    }
    // Texture and image buffers: their TIC entries, and the CPU-modified bytes of the ranges
    // they name (ComputePipeline::Configure binds them through the buffer cache).
    struct BufferViewCapture {
        DAddr device_addr;
        u32 size;
        bool is_written;
    };
    boost::container::small_vector<BufferViewCapture, 8> buffer_views;
    const auto capture_buffer_view = [&](u32 handle, bool is_written) {
        const u32 index = Tegra::Texture::TextureHeaderIndex(handle, tic_limit, native_tiling);
        if (index > tic_limit) {
            return true;
        }
        const GPUVAddr tic_addr = real.regs.tic.Address() + u64{index} * 32;
        SeqReadDescriptor(tic_addr);
        Tegra::Texture::TICEntry tic{};
        memory.ReadBlockUnsafe(tic_addr, &tic, sizeof(tic));
        const VideoCommon::ImageInfo view_info{tic};
        if (view_info.type != VideoCommon::ImageType::Buffer) {
            return false;
        }
        const GPUVAddr gpu_addr = tic.Address();
        const u32 size = VideoCommon::CalculateGuestSizeInBytes(view_info);
        if (gpu_addr == 0 || size == 0) {
            return true;
        }
        const auto translated = memory.GpuToCpuAddress(gpu_addr);
        if (!translated || !memory.IsContinuousRange(gpu_addr, size)) {
            return false;
        }
        buffer_views.push_back({*translated, size, is_written});
        return true;
    };
    for (const auto& desc : info.texture_buffer_descriptors) {
        for (u32 index = 0; index < desc.count; ++index) {
            if (!capture_buffer_view(read_handle(desc, index).first, false)) {
                return 2;
            }
        }
    }
    for (const auto& desc : info.image_buffer_descriptors) {
        for (u32 index = 0; index < desc.count; ++index) {
            if (!capture_buffer_view(read_handle(desc, index).first, desc.is_written)) {
                return 2;
            }
        }
    }

    std::optional<MTDetail::SeqTrackerLock> lock{std::in_place, rasterizer.buffer_cache};
    SeqDeferUploadReads();
    const u32 alignment = rasterizer.buffer_cache.MtUniformBufferAlignment();
    const u32 skip_size = rasterizer.buffer_cache.MtSkipCacheSize();
    const auto& sizes = pipeline->UniformSizes();
    u32 mask = info.constant_buffer_mask;
    while (mask != 0) {
        const u32 index = static_cast<u32>(std::countr_zero(mask));
        mask &= mask - 1;
        DAddr device_addr = 0;
        u32 binding_size = 0;
        if (enabled(index)) {
            if (const auto translated = SeqTranslate(cbufs[index].Address())) {
                device_addr = *translated;
                binding_size = cbufs[index].size;
            }
        }
        const u32 size = (std::min)(binding_size, sizes[index]);
        const bool has_host = rasterizer.buffer_cache.MtHasHostBuffer(device_addr);
        const bool needs_alignment = has_host && alignment > 1 && device_addr % alignment != 0;
        const bool fast = needs_alignment ||
                          (has_host && size <= skip_size &&
                           !rasterizer.buffer_cache.MtIsRegionGpuModified(device_addr, size));
        VideoCommon::MT::UboEntry entry{
            .stage = 5,
            .index = static_cast<u8>(index),
            .staged = static_cast<u8>(fast ? 1 : 0),
            .memo = 0,
            .size = size,
            .device_addr = device_addr,
            .offset = 0,
            .pad2 = 0,
        };
        if (fast) {
            SeqStageUniform(entry);
        } else {
            SeqTakeUploads(device_addr, size);
        }
        ubos.push_back(entry);
    }
    for (const SsboCapture& ssbo : ssbos) {
        SeqTakeUploads(ssbo.device_addr, ssbo.size);
        if (ssbo.is_written) {
            rasterizer.buffer_cache.MtCaptureWrittenRange(ssbo.device_addr, ssbo.size);
        }
    }
    for (const BufferViewCapture& view : buffer_views) {
        SeqTakeUploads(view.device_addr, view.size);
        if (view.is_written) {
            rasterizer.buffer_cache.MtCaptureWrittenRange(view.device_addr, view.size);
        }
    }
    if (indirect_device_addr != 0) {
        SeqTakeUploads(indirect_device_addr, 12);
    }
    if (lite_seq_lock) {
        // EDEN_NX2_MT_LITE_SEQLOCK: as CaptureDraw.
        lock.reset();
        SeqReadDeferredUploads();
    }
    FinishOverlay();
    return -1;
}

inline void Nx2DrawPipeline::EmitCompute(ComputePipeline* pipeline, u64 native_key) {
    KeplerCompute& real = *rasterizer.kepler_compute.Real();
    blob.resize((blob.size() + 7) & ~size_t{7});
    CloseBatch();
    EmitDeltas();
    const u32 overlay_words = static_cast<u32>(overlay.size() * sizeof(overlay[0]) / 8);
    const u32 ubo_words = static_cast<u32>(ubos.size() * sizeof(ubos[0]) / 8);
    const u32 upload_words = static_cast<u32>(uploads.size() * sizeof(uploads[0]) / 8);
    const u32 blob_words = static_cast<u32>(blob.size() / 8);
    const u32 words = 1 + ComputeHeaderWords + overlay_words + ubo_words + upload_words + blob_words;
    u64* const out = stream.Reserve(words);
    out[0] = SpmcStream::MakeHeader(static_cast<u8>(MT::StreamEntry::ComputePacket), words, 0);
    ComputeHeader header{};
    header.seq = next_seq++;
    header.pipeline = pipeline;
    header.native_key = native_key;
    header.qmd = real.launch_description;
    header.state = real.MtGetDispatchState();
    header.num_overlay = static_cast<u32>(overlay.size());
    header.num_ubo = static_cast<u32>(ubos.size());
    header.num_upload = static_cast<u32>(uploads.size());
    header.blob_size = static_cast<u32>(blob.size());
    std::memset(out + 1, 0, ComputeHeaderWords * 8);
    std::memcpy(out + 1, &header, sizeof(header));
    u64* cursor = out + 1 + ComputeHeaderWords;
    std::memcpy(cursor, overlay.data(), overlay_words * 8);
    cursor += overlay_words;
    std::memcpy(cursor, ubos.data(), ubo_words * 8);
    cursor += ubo_words;
    std::memcpy(cursor, uploads.data(), upload_words * 8);
    cursor += upload_words;
    std::memcpy(cursor, blob.data(), blob.size());
    stream.Commit();
    NoteAsyncImageWork();
    counters.compute_packets.fetch_add(1, std::memory_order_relaxed);
    counters.packet_words.fetch_add(words, std::memory_order_relaxed);
    if (lockstep) {
        WaitCommitted();
    }
}

inline void Nx2DrawPipeline::SeqDispatch() {
    RasterizerVulkan& r = rasterizer;
    const auto serial_dispatch = [&] { Serial(MT::Kind::Dispatch, [&] { r.DispatchCompute(); }); };
    if (!VideoCommon::MT::ComputePackets()) {
        serial_dispatch();
        return;
    }
    KeplerCompute& real = *r.kepler_compute.Real();
    r.gpu_memory->FlushCaching();
    u64 native_key{};
    ComputePipeline* const pipeline = r.pipeline_cache.CurrentComputePipeline(native_key);
    if (pipeline == nullptr) {
        if (real.IsNativeMode()) {
            throw Shader::RuntimeError("Native compute pipeline unavailable");
        }
        return;
    }
    // The collision map program publishes an image into guest memory after it runs.
    constexpr u64 CollisionMapProgram = 0x6e0ae200ee9aa220ULL;
    const int reason = native_key == CollisionMapProgram ? 12 : CaptureCompute(pipeline);
    if (reason >= 0) {
        ForceSerialReason(static_cast<u32>(reason));
        serial_dispatch();
        return;
    }
    EmitCompute(pipeline, native_key);
}

inline void Nx2DrawPipeline::CommitCompute(const u64* entry) {
    ComputeHeader header;
    std::memcpy(&header, entry + 1, sizeof(header));
    const u64* cursor = entry + 1 + ComputeHeaderWords;
    const std::span<const VideoCommon::MT::OverlayEntry> overlay_span{
        reinterpret_cast<const VideoCommon::MT::OverlayEntry*>(cursor), header.num_overlay};
    cursor += header.num_overlay * sizeof(VideoCommon::MT::OverlayEntry) / 8;
    const std::span<const VideoCommon::MT::UboEntry> ubo_span{
        reinterpret_cast<const VideoCommon::MT::UboEntry*>(cursor), header.num_ubo};
    cursor += header.num_ubo * sizeof(VideoCommon::MT::UboEntry) / 8;
    const std::span<const VideoCommon::MT::UploadEntry> upload_span{
        reinterpret_cast<const VideoCommon::MT::UploadEntry*>(cursor), header.num_upload};
    cursor += header.num_upload * sizeof(VideoCommon::MT::UploadEntry) / 8;
    const u8* const blob_data = reinterpret_cast<const u8*>(cursor);
    KeepUniformMemos(ubo_span, blob_data);
    if (current == nullptr || !current->mirror_compute) {
        return;
    }
    RasterizerVulkan& r = rasterizer;
    KeplerCompute& mirror = *current->mirror_compute;
    mirror.launch_description = header.qmd;
    mirror.MtSetDispatchState(header.state);
    if (verify) {
        counters.verify_checks.fetch_add(1, std::memory_order_relaxed);
        const KeplerCompute& real = *current->real_compute;
        if (std::memcmp(&real.regs, &mirror.regs, sizeof(real.regs)) != 0) {
            u32 first = 0;
            for (u32 i = KeplerCompute::Regs::NUM_REGS; i-- > 0;) {
                if (real.regs.reg_array[i] != mirror.regs.reg_array[i]) {
                    first = i;
                }
            }
            VerifyReport(0x4000 + first,
                         fmt::format("dispatch {}: compute register {:#x} real {:#x} mirror {:#x}",
                                     header.seq, first, real.regs.reg_array[first],
                                     mirror.regs.reg_array[first]));
            mirror.regs = real.regs;
        }
        VerifyCaptures(header.seq, overlay_span, ubo_span, upload_span, blob_data);
    }
    r.FlushWork();
    VideoCommon::MT::CaptureView view{
        .overlay = overlay_span,
        .ubos = ubo_span,
        .blob = blob_data,
        .verify = verify,
        .ubo_memo = ubo_memo_bytes.get(),
        .cpu_write_since = verify_cpu_writes_since.load(std::memory_order_acquire),
    };
    {
        // DispatchComputeBody takes both cache mutexes itself.
        std::unique_lock buffer_lock{r.buffer_cache.mutex, std::defer_lock};
        std::unique_lock texture_lock{r.texture_cache.mutex, std::defer_lock};
        if (VideoCommon::MT::LockScope()) {
            buffer_lock.lock();
            r.buffer_cache.MtApplyUploads(upload_span, blob_data);
            buffer_lock.unlock();
        } else {
            std::lock(texture_lock, buffer_lock);
            r.buffer_cache.MtApplyUploads(upload_span, blob_data);
        }
        VideoCommon::MT::t_capture = &view;
        SCOPE_EXIT {
            VideoCommon::MT::t_capture = nullptr;
        };
        try {
            r.DispatchComputeBody(header.pipeline, header.native_key);
        } catch (const std::exception& e) {
            // The sequencer went on as if dispatched (native_execution_failed has no reader).
            counters.compute_failures.fetch_add(1, std::memory_order_relaxed);
            static u32 logged{};
            if (logged++ < 16) {
                LOG_WARNING(Render_Vulkan, "MT: compute packet {} failed: {}", header.seq,
                            e.what());
            }
        }
    }
    if (verify) {
        ReportView(header.seq, view);
    }
}

} // namespace Vulkan
