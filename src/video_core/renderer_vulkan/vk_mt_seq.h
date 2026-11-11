// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Phase 7 of docs/mt-plan.md ("sequencer diet"): items phase 4 ran as Serial (the sequencer
// waiting for the committer) become Commands or draw packets carrying what the sequencer read.
// Included by vk_rasterizer.cpp after vk_mt_pipeline.h.

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "common/scope_exit.h"
#include "video_core/control/channel_state.h"
#include "video_core/mt/c11_serial.h"
#include "video_core/mt/seq_diet.h"
#include "video_core/renderer_vulkan/vk_mt_pipeline.h"

namespace Vulkan {

namespace MTDetail {
/// Released before returning: never hold it across a Serial (the committer takes it).
inline bool SeqGpuModified(BufferCache& buffer_cache, DAddr device_addr, u64 size) {
    SeqTrackerLock lock{buffer_cache};
    return buffer_cache.IsRegionGpuModified(device_addr, size);
}
/// Whether the buffer cache may hold GPU-modified data in the range now. On Switch 2 the page
/// bits are checked first without the lock (IsRegionGpuModified's own EDEN_NX2_GPU_MODIFIED_FAST
/// test: GPU-modified ranges only lie on pages marked GPU-written, never cleared; the sequencer
/// marks them itself at capture, the committer before a Serial returns or a Command the caller
/// guards with seq_async_buffer_position).
inline bool SeqMayHoldGpuBufferData(BufferCache& buffer_cache, DAddr device_addr, u64 size) {
    if (Tegra::g_nx2_ampere.load(std::memory_order_relaxed) && size != 0) {
        static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_GPU_MODIFIED_FAST");
        if (Tegra::Nx2Switch(mode, true)) {
            bool written = false;
            for (DAddr page = device_addr & ~DAddr{Core::DEVICE_PAGEMASK};
                 page < device_addr + size; page += Core::DEVICE_PAGESIZE) {
                if (Tegra::Nx2IsGpuWritten(page)) {
                    written = true;
                    break;
                }
            }
            if (!written) {
                return false;
            }
        }
    }
    return SeqGpuModified(buffer_cache, device_addr, size);
}
} // namespace MTDetail

inline Nx2DrawPipeline::HeapUploads* Nx2DrawPipeline::TakeHeapUploads(
    std::span<const std::pair<DAddr, u32>> ranges) {
    ClearCapture();
    {
        MTDetail::SeqTrackerLock lock{rasterizer.buffer_cache};
        for (const auto& [device_addr, size] : ranges) {
            SeqTakeUploads(device_addr, size);
        }
    }
    if (uploads.empty()) {
        return nullptr;
    }
    auto* const heap_uploads = new HeapUploads{uploads, blob};
    ClearCapture();
    return heap_uploads;
}

inline void Nx2DrawPipeline::ApplyHeapUploads(HeapUploads* heap_uploads) {
    if (heap_uploads == nullptr) {
        return;
    }
    {
        std::scoped_lock lock{rasterizer.buffer_cache.mutex};
        rasterizer.buffer_cache.MtApplyUploads(heap_uploads->uploads, heap_uploads->blob.data());
    }
    delete heap_uploads;
}

// ---- Uniform buffer write generations (EDEN_NX2_MT_UBO_GEN) ----

inline void Nx2DrawPipeline::SeqStageUniform(VideoCommon::MT::UboEntry& entry) {
    u64 generation = 0;
    if (!SeqStageUniformGen(entry, generation)) {
        SeqStageUniformBytes(entry, generation);
    }
}

inline bool Nx2DrawPipeline::SeqStageUniformGen(VideoCommon::MT::UboEntry& entry,
                                                u64& generation) {
    using View = VideoCommon::MT::CaptureView;
    const DAddr device_addr = entry.device_addr;
    const u32 size = entry.size;
    generation = 0;
    if (!VideoCommon::MT::UboGen() || size == 0 || size > View::UboMemoBytes ||
        entry.stage >= View::UboMemoStages || entry.index >= View::UboMemoSlots) {
        return false;
    }
    // EDEN_NX2_MT_UBO_GEN skips reading the bytes while the generations allow (compact or not).
    // Guest CPU writes reach the generations only through write traps: the pages must be
    // protected (not CPU-modified in the buffer tracker) now and when the memo was taken
    // (dropping the protection bumps them). Query reports write guest memory directly.
    const UniformMemo& memo = seq_ubo_memo[entry.stage][entry.index];
    const bool protected_pages =
        !rasterizer.buffer_cache.IsRegionCpuModified(device_addr, size) &&
        !Tegra::Nx2AnyPage(Tegra::g_nx2_query_pages, device_addr, size);
    // Read before the bytes: a write in between only makes the next capture miss.
    generation =
        protected_pages ? VideoCommon::MT::UboWriteGenerations().Read(device_addr, size) : 0;
    if (generation != 0 && memo.generation == generation && memo.device_addr == device_addr &&
        memo.size == size) {
        entry.staged = 2;
        counters.ubo_gen_hits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

inline void Nx2DrawPipeline::SeqStageUniformBytes(VideoCommon::MT::UboEntry& entry,
                                                  u64 generation) {
    using View = VideoCommon::MT::CaptureView;
    const DAddr device_addr = entry.device_addr;
    const u32 size = entry.size;
    const bool memo_slot = size != 0 && size <= View::UboMemoBytes &&
                           entry.stage < View::UboMemoStages && entry.index < View::UboMemoSlots;
    if (compact && memo_slot) {
        // EDEN_NX2_MT_COMPACT: the committer keeps every slot's last streamed bytes (each
        // staged == 1 entry here has `memo`), and seq_ubo_bytes holds the same bytes as of the
        // end of the stream. Unchanged bytes go as staged == 2, a few changed chunks as a
        // patch (staged == 3); both are read from the committer's copy.
        UniformMemo& memo = seq_ubo_memo[entry.stage][entry.index];
        u8* const kept = seq_ubo_bytes.get() +
                         (size_t{entry.stage} * View::UboMemoSlots + entry.index) *
                             View::UboMemoBytes;
        u8* const now = ubo_scratch.data();
        rasterizer.device_memory.ReadBlockUnsafe(device_addr, now, size);
        // The kept bytes are now this range's at `generation` (read before the bytes).
        memo = UniformMemo{device_addr, size, generation};
        u32 changed_bytes = 0;
        const u64 mask = VideoCommon::MT::UniformPatchMask(now, kept, size, changed_bytes);
        if (mask == 0) {
            entry.staged = 2;
            counters.ubo_same.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const u32 patch_size = static_cast<u32>(sizeof(u64)) + changed_bytes;
        entry.offset = static_cast<u32>(blob.size());
        if (patch_size < size) {
            entry.staged = 3;
            const u32 padded = (patch_size + 7) & ~7U;
            blob.resize(blob.size() + padded);
            VideoCommon::MT::WriteUniformPatch(blob.data() + entry.offset, now, size, mask);
            blob_ubo_bytes += padded;
            counters.ubo_patched.fetch_add(1, std::memory_order_relaxed);
        } else {
            entry.memo = 1;
            const u32 padded = (size + 7) & ~7U;
            blob.resize(blob.size() + padded);
            std::memcpy(blob.data() + entry.offset, now, size);
            blob_ubo_bytes += padded;
            counters.ubo_full.fetch_add(1, std::memory_order_relaxed);
        }
        std::memcpy(kept, now, size);
        return;
    }
    if (VideoCommon::MT::UboGen() && memo_slot) {
        UniformMemo& memo = seq_ubo_memo[entry.stage][entry.index];
        memo = UniformMemo{device_addr, size, generation};
        if (generation != 0) {
            entry.memo = 1;
            counters.ubo_gen_misses.fetch_add(1, std::memory_order_relaxed);
        } else {
            counters.ubo_gen_unprotected.fetch_add(1, std::memory_order_relaxed);
        }
    }
    entry.offset = static_cast<u32>(blob.size());
    blob.resize(blob.size() + ((size + 7) & ~7U));
    rasterizer.device_memory.ReadBlockUnsafe(device_addr, blob.data() + entry.offset, size);
    blob_ubo_bytes += (size + 7) & ~7U;
}

inline void Nx2DrawPipeline::KeepUniformMemos(std::span<const VideoCommon::MT::UboEntry> entries,
                                              const u8* blob_data) {
    using View = VideoCommon::MT::CaptureView;
    if (!ubo_memo_bytes) {
        return;
    }
    const auto kept_bytes = [&](const VideoCommon::MT::UboEntry& entry) {
        return ubo_memo_bytes.get() +
               (size_t{entry.stage} * View::UboMemoSlots + entry.index) * View::UboMemoBytes;
    };
    for (const VideoCommon::MT::UboEntry& entry : entries) {
        if (entry.staged == 1 && entry.memo != 0) {
            std::memcpy(kept_bytes(entry), blob_data + entry.offset, entry.size);
        } else if (entry.staged == 3) {
            // EDEN_NX2_MT_COMPACT: the changed chunks (UboBytes then reads the kept bytes).
            VideoCommon::MT::ApplyUniformPatch(kept_bytes(entry), blob_data + entry.offset,
                                               entry.size);
        }
    }
}

// ---- Conditional rendering ----

inline bool Nx2DrawPipeline::SeqConditionalRendering() {
    RasterizerVulkan& r = rasterizer;
    const auto serial = [&] {
        return Serial(MT::Kind::ConditionalRendering,
                      [&] { return r.AccelerateConditionalRendering(); });
    };
    if (!VideoCommon::MT::CondCommands()) {
        return serial();
    }
    // The old body's invalidation, on the sequencer that owns the accumulator.
    r.gpu_memory->FlushCaching();
    using Regs = Maxwell3D::Regs;
    const Regs& regs = r.maxwell3d.Real()->regs;
    // The committer redoes the query cache's part (host conditional rendering state) against its
    // mirror, whose registers equal these at the command. `heap_uploads`: the bytes the resolve
    // pass reads from the buffer cache, as they were here.
    struct HostConditional {
        Nx2DrawPipeline* self;
        HeapUploads* heap_uploads;
        void operator()() const {
            self->ApplyHeapUploads(heap_uploads);
            VideoCommon::MT::CaptureView view{};
            VideoCommon::MT::t_capture = &view; // no guest reads behind the sequencer's back
            SCOPE_EXIT {
                VideoCommon::MT::t_capture = nullptr;
            };
            (void)self->rasterizer.query_cache.AccelerateHostConditionalRendering();
        }
    };
    const auto mode = regs.render_enable.mode;
    if (regs.render_enable_override != Regs::RenderEnable::Override::UseRenderEnable ||
        mode == Regs::RenderEnable::Mode::True || mode == Regs::RenderEnable::Mode::False) {
        // Ends host conditional rendering; false.
        Command(MT::Kind::ConditionalRendering, HostConditional{this, nullptr});
        return false;
    }
    if ((mode != Regs::RenderEnable::Mode::Conditional && mode != Regs::RenderEnable::Mode::IfEqual &&
         mode != Regs::RenderEnable::Mode::IfNotEqual) ||
        !r.device.IsExtConditionalRendering()) {
        // false without touching anything.
        return false;
    }
    const GPUVAddr address = regs.render_enable.Address();
    if (mode == Regs::RenderEnable::Mode::Conditional) {
        // Always true: the value is resolved on the GPU from the buffer holding it.
        HeapUploads* heap_uploads = nullptr;
        if (const auto device_addr = r.gpu_memory->GpuToCpuAddress(address)) {
            const std::pair<DAddr, u32> range{*device_addr, 8};
            heap_uploads = TakeHeapUploads(std::span(&range, 1));
        }
        Command(MT::Kind::ConditionalRendering, HostConditional{this, heap_uploads});
        return true;
    }
    // IfEqual / IfNotEqual: false (CPU comparison on the engine) when neither word is a query the
    // query cache holds or GPU-written buffer data; otherwise the answer is the committer's.
    if (stream.ConsumerPosition(consumer) < last_query_position) {
        return serial();
    }
    for (const GPUVAddr word : {address, address + 16}) {
        const auto device_addr = r.gpu_memory->GpuToCpuAddress(word);
        if (!device_addr) {
            continue;
        }
        if (Tegra::Nx2AnyPage(Tegra::g_nx2_query_pages, *device_addr, 8)) {
            return serial();
        }
        if (MTDetail::SeqGpuModified(r.buffer_cache, *device_addr, 8)) {
            return serial();
        }
    }
    Command(MT::Kind::ConditionalRendering, HostConditional{this, nullptr});
    return false;
}

// ---- Indirect draws ----

inline void Nx2DrawPipeline::SeqDrawIndirect() {
    RasterizerVulkan& r = rasterizer;
    if (!VideoCommon::MT::IndirectPackets()) {
        Serial(MT::Kind::DrawIndirect, [&] { r.DrawIndirect(); });
        return;
    }
    const IndirectParams params = r.maxwell3d.Real()->draw_manager.indirect_state;
    SeqDraw(params.is_indexed, 0, &params);
}

// ---- DrawTexture ----

inline void Nx2DrawPipeline::SeqDrawTexture() {
    RasterizerVulkan& r = rasterizer;
    if (!VideoCommon::MT::DrawTextureCommands()) {
        Serial(MT::Kind::DrawTexture, [&] { r.DrawTexture(); });
        return;
    }
    using Regs = Maxwell3D::Regs;
    Maxwell3D& real = *r.maxwell3d.Real();
    const Regs& regs = real.regs;
    struct DrawTextureCommand {
        Nx2DrawPipeline* self;
        Maxwell3D::DrawManager::DrawTextureState state;
        std::array<VideoCommon::MT::OverlayEntry, 2> overlay;
        u32 num_overlay;
        std::array<u8, 64> bytes;
        void operator()() const {
            Maxwell3D* const mirror = self->Mirror3D();
            if (mirror == nullptr) {
                return;
            }
            mirror->draw_manager.draw_texture_state = state;
            VideoCommon::MT::CaptureView view{
                .overlay = std::span(overlay.data(), num_overlay),
                .ubos = {},
                .blob = bytes.data(),
                .verify = false,
            };
            VideoCommon::MT::t_capture = &view;
            SCOPE_EXIT {
                VideoCommon::MT::t_capture = nullptr;
            };
            self->rasterizer.DrawTexture();
        }
    };
    DrawTextureCommand command{};
    command.self = this;
    command.state = real.draw_manager.draw_texture_state;
    // The TIC and TSC entries GetImageView / GetSampler read (SynchronizeDescriptors' limits).
    const auto capture = [&](GPUVAddr gpu_addr) {
        for (u32 i = 0; i < command.num_overlay; ++i) {
            if (command.overlay[i].gpu_addr == gpu_addr) {
                return;
            }
        }
        const u32 offset = command.num_overlay * 32;
        r.gpu_memory->ReadBlockUnsafe(gpu_addr, command.bytes.data() + offset, 32);
        command.overlay[command.num_overlay++] = {gpu_addr, 32, offset};
    };
    const u32 tic_limit = regs.tex_header.limit;
    const u32 tic_index = Tegra::Texture::TextureHeaderIndex(command.state.src_texture, tic_limit,
                                                             r.gpu_memory->UsesNativeTiling());
    if (tic_index <= tic_limit) {
        capture(regs.tex_header.Address() + u64{tic_index} * 32);
    }
    const bool linked_tsc = regs.sampler_binding == Regs::SamplerBinding::ViaHeaderBinding;
    const u32 tsc_limit = linked_tsc ? tic_limit : regs.tex_sampler.limit;
    if (command.state.src_sampler <= tsc_limit) {
        capture(regs.tex_sampler.Address() + u64{command.state.src_sampler} * 32);
    }
    if (command.num_overlay == 2 && command.overlay[1].gpu_addr < command.overlay[0].gpu_addr) {
        std::swap(command.overlay[0], command.overlay[1]);
    }
    if (command.num_overlay == 2 &&
        command.overlay[0].gpu_addr + 32 > command.overlay[1].gpu_addr) {
        // Overlapping entries (tables sharing memory): the overlay can't hold both.
        Serial(MT::Kind::DrawTexture, [&] { r.DrawTexture(); });
        return;
    }
    Command(MT::Kind::DrawTexture, command);
}

// ---- DMA ----

inline bool Nx2DrawPipeline::SeqDmaCopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount) {
    RasterizerVulkan& r = rasterizer;
    const auto serial = [&] {
        return Serial(MT::Kind::Dma,
                      [&] { return r.accelerate_dma.BufferCopy(src_address, dest_address, amount); });
    };
    if (!VideoCommon::MT::DmaPackets()) {
        return serial();
    }
    const std::optional<DAddr> src = r.gpu_memory->GpuToCpuAddress(src_address);
    const std::optional<DAddr> dst = r.gpu_memory->GpuToCpuAddress(dest_address);
    if (!src || !dst) {
        return false;
    }
    if (amount == 0 || amount > 0x10000000) {
        return serial();
    }
    BufferCache& buffer_cache = r.buffer_cache;
    bool held = false;
    bool dst_gpu_data = false;
    {
        MTDetail::SeqTrackerLock lock{buffer_cache};
        // GPU data at the destination may still come back to guest memory from a download taken
        // before this copy, after the copy below wrote it: only the old path orders those.
        dst_gpu_data = buffer_cache.IsRegionGpuModified(*dst, amount);
        // DMACopy only acts on ranges the buffer cache holds (or GPU writes still in flight
        // will create); otherwise MaxwellDMA copies guest memory, as when it returns false.
        held = buffer_cache.MtIsRegionRegistered(*src, amount) ||
               buffer_cache.MtIsRegionRegistered(*dst, amount) ||
               buffer_cache.IsRegionGpuModified(*src, amount);
    }
    if (dst_gpu_data) {
        counters.dma_serial.fetch_add(1, std::memory_order_relaxed);
        return serial();
    }
    if (!held) {
        return false;
    }
    struct DmaCopyCommand {
        Nx2DrawPipeline* self;
        DAddr src;
        DAddr dst;
        u64 amount;
        HeapUploads* heap_uploads;
        std::vector<std::pair<DAddr, u64>>* intervals;
        void operator()() const {
            RasterizerVulkan& rz = self->rasterizer;
            {
                std::scoped_lock lock{rz.buffer_cache.mutex};
                if (heap_uploads != nullptr) {
                    rz.buffer_cache.MtApplyUploads(heap_uploads->uploads,
                                                   heap_uploads->blob.data());
                }
                rz.buffer_cache.Nx2CollectListCopies(true);
                rz.buffer_cache.MtCommitDmaCopy(src, dst, amount, *intervals);
                if (rz.buffer_cache.Nx2CopyListPages(rz.scheduler.CurrentTick())) {
                    rz.scheduler.Flush();
                }
                rz.buffer_cache.Nx2CollectListCopies(false);
            }
            delete heap_uploads;
            delete intervals;
        }
    };
    const u32 size = static_cast<u32>(amount);
    const std::array<std::pair<DAddr, u32>, 2> ranges{{{*src, size}, {*dst, size}}};
    HeapUploads* const heap_uploads = TakeHeapUploads(ranges);
    auto* const intervals = new std::vector<std::pair<DAddr, u64>>();
    {
        MTDetail::SeqTrackerLock lock{buffer_cache};
        buffer_cache.MtCaptureDmaCopy(*src, *dst, amount, *intervals);
    }
    // DMACopy's guest memory copy, here in stream order (later captures read it).
    BeforeWrite(*dst, amount);
    VideoCommon::MT::BumpWriteGeneration(*dst, amount);
    dma_scratch.resize(amount);
    r.device_memory.ReadBlockUnsafe(*src, dma_scratch.data(), amount);
    r.device_memory.WriteBlockUnsafe(*dst, dma_scratch.data(), amount);
    Command(MT::Kind::Dma, DmaCopyCommand{this, *src, *dst, amount, heap_uploads, intervals});
    return true;
}

inline bool Nx2DrawPipeline::SeqDmaClear(GPUVAddr dst_address, u64 amount, u32 value) {
    RasterizerVulkan& r = rasterizer;
    const auto serial = [&] {
        return Serial(MT::Kind::Dma,
                      [&] { return r.accelerate_dma.BufferClear(dst_address, amount, value); });
    };
    if (!VideoCommon::MT::DmaPackets()) {
        return serial();
    }
    const std::optional<DAddr> dst = r.gpu_memory->GpuToCpuAddress(dst_address);
    if (!dst) {
        return false;
    }
    if (MTDetail::SeqGpuModified(r.buffer_cache, *dst, amount * sizeof(u32))) {
        counters.dma_serial.fetch_add(1, std::memory_order_relaxed);
        return serial();
    }
    // DMAClear reads no guest memory; MaxwellDMA ignores its result and fills guest memory next.
    struct DmaClearCommand {
        RasterizerVulkan* r;
        GPUVAddr dst_address;
        u64 amount;
        u32 value;
        void operator()() const {
            std::scoped_lock lock{r->buffer_cache.mutex};
            (void)r->buffer_cache.DMAClear(dst_address, amount, value);
        }
    };
    Command(MT::Kind::Dma, DmaClearCommand{&r, dst_address, amount, value});
    return true;
}

// ---- Flushes and invalidations (EDEN_NX2_MT_FLUSHCMD) ----

inline void Nx2DrawPipeline::SeqFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which,
                                            bool may_queue) {
    using VideoCommon::CacheType;
    RasterizerVulkan& r = rasterizer;
    if (!VideoCommon::MT::FlushCommands()) {
        Serial(MT::Kind::FlushRegion, [&] { r.FlushRegion(addr, size, which); });
        return;
    }
    // FlushRegion only downloads GPU-written data: the query cache's pending results, the
    // buffer cache's GPU-modified ranges and GPU-modified images. Each part is needed when such
    // data may lie in the range now, or may appear there from work the committer has not run
    // yet (positions read once, first: a later read only sees more consumed).
    const u64 consumed = stream.ConsumerPosition(consumer);
    bool needed = false;
    if (True(which & CacheType::QueryCache)) {
        // As SeqInnerInvalidation: pages that ever held a query, or reports still in flight.
        needed = consumed < last_query_position ||
                 Tegra::Nx2AnyPage(Tegra::g_nx2_query_pages, addr, size);
    }
    if (!needed && True(which & CacheType::BufferCache)) {
        needed = consumed < seq_async_buffer_position ||
                 MTDetail::SeqMayHoldGpuBufferData(r.buffer_cache, addr, size);
    }
    if (!needed && True(which & CacheType::TextureCache)) {
        if (consumed < seq_async_image_position) {
            // A draw/compute packet or Command in flight may still register or render an image
            // over the range (its page bits are set at registration, on the committer).
            needed = true;
        } else if (Tegra::Nx2MayHoldTexture(addr, size)) {
            // Every image the range can hold is registered: ask the cache (DownloadMemory only
            // takes GPU-modified images).
            std::scoped_lock lock{r.texture_cache.mutex};
            needed = r.texture_cache.IsRegionGpuModified(addr, size);
        }
    }
    if (!needed) {
        counters.flush_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (may_queue && VideoCommon::MT::t_flush_async) {
        // GpuSplit::RunFlush: the front waits for the committer to pass the drain's mark behind
        // this Command before it reads, and queues nothing before that.
        struct FlushCommand {
            RasterizerVulkan* r;
            DAddr addr;
            u64 size;
            CacheType which;
            void operator()() const {
                r->FlushRegion(addr, size, which);
            }
        };
        counters.flush_commands.fetch_add(1, std::memory_order_relaxed);
        Command(MT::Kind::FlushRegion, FlushCommand{&r, addr, size, which});
        return;
    }
    // The caller reads the memory next.
    counters.flush_serials.fetch_add(1, std::memory_order_relaxed);
    Serial(MT::Kind::FlushRegion, [&] { r.FlushRegion(addr, size, which); });
}

inline void Nx2DrawPipeline::SeqInvalidateRegion(DAddr addr, u64 size,
                                                 VideoCommon::CacheType which) {
    using VideoCommon::CacheType;
    RasterizerVulkan& r = rasterizer;
    if (!VideoCommon::MT::FlushCommands()) {
        Serial(MT::Kind::InvalidateRegion, [&] { r.InvalidateRegion(addr, size, which); });
        return;
    }
    counters.inval_sequencer.fetch_add(1, std::memory_order_relaxed);
    const u64 consumed = stream.ConsumerPosition(consumer);
    // Images belong to the committer and nothing on the sequencer reads their state: marking
    // them CPU-modified in stream order is what the Serial did, after everything before. Skipped
    // only when no image can lie on the range, now or from work still in flight.
    if (True(which & CacheType::TextureCache) &&
        (consumed < seq_async_image_position || Tegra::Nx2MayHoldTexture(addr, size))) {
        struct TextureInvalidation {
            RasterizerVulkan* r;
            DAddr addr;
            u64 size;
            void operator()() const {
                std::scoped_lock lock{r->texture_cache.mutex};
                r->texture_cache.WriteMemory(addr, size);
            }
        };
        counters.inval_tex_commands.fetch_add(1, std::memory_order_relaxed);
        Command(MT::Kind::InvalidateRegion, TextureInvalidation{&r, addr, size});
    }
    // The buffer tracker here: the next capture must take the written bytes (phase 6). Pages
    // holding GPU-written data still go to the committer as a Serial.
    if (True(which & CacheType::BufferCache)) {
        const std::pair<DAddr, std::size_t> region{addr, static_cast<std::size_t>(size)};
        SeqInvalidateBuffers(std::span(&region, 1));
    }
    // Queries: as SeqInnerInvalidation.
    if (True(which & CacheType::QueryCache) &&
        (consumed < last_query_position ||
         Tegra::Nx2AnyPage(Tegra::g_nx2_query_pages, addr, size))) {
        Command(MT::Kind::Query,
                [rp = &r, addr, size] { rp->query_cache.InvalidateRegion(addr, size); });
    }
    // Programs: the sequencer looks pipelines up (SeqDraw), as SeqInnerInvalidation.
    if (True(which & CacheType::ShaderCache)) {
        r.pipeline_cache.InvalidateRegion(addr, size);
    }
}

// ---- Cycle 11 W2: channel binds (mt/c11_serial.h, S1/S2) ----

inline void Nx2DrawPipeline::SeqBindChannel(Tegra::Control::ChannelState& channel) {
    using VideoCommon::MT::C11ChanRefuse;
    VideoCommon::MT::C11SerialLogStart();
    RasterizerVulkan& r = rasterizer;
    auto& stats = VideoCommon::MT::g_c11_serial;
    Maxwell3D* const new3d = channel.payload ? &channel.payload->maxwell_3d : nullptr;
    // The rasterizer's stored pointers are the sequencer's view: the sequencer switches them
    // itself (S2), or a Serial did while it waited.
    Maxwell3D* const bound3d = r.maxwell3d.Real();
    // S1: the channel already bound on this thread (and, in stream order, on the committer).
    if (VideoCommon::MT::C11SameChan() && new3d != nullptr && bound3d == new3d &&
        r.gpu_memory == channel.memory_manager) {
        stats.bind_same.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    C11ChanRefuse refuse = C11ChanRefuse::Count;
    if (!VideoCommon::MT::C11ChanCmd()) {
        refuse = C11ChanRefuse::Off;
    } else if (resolver_count != 0) {
        refuse = C11ChanRefuse::Resolvers;
    } else if (new3d == nullptr || bound3d == nullptr || r.gpu_memory == nullptr ||
               channel.memory_manager == nullptr) {
        refuse = C11ChanRefuse::NoChannel;
    } else if (r.gpu_memory != channel.memory_manager) {
        refuse = C11ChanRefuse::AddressSpace;
    } else if (r.program_id != channel.program_id) {
        refuse = C11ChanRefuse::Program;
    }
    if (refuse != C11ChanRefuse::Count) {
        stats.bind_serial[static_cast<size_t>(refuse)].fetch_add(1, std::memory_order_relaxed);
        // The pre-cycle-11 path: RasterizerVulkan::BindChannel on the committer, then RunSerial
        // rebinds the committer's mirrors (BindCurrentChannel).
        Serial(MT::Kind::Channel, [&] { r.BindChannel(channel); });
        return;
    }
    // S2. The bound channel's register writes go first, to its mirror.
    CloseBatch();
    EmitDeltas();
    const s32 channel_id = channel.bind_id;
    // The sequencer's own channel context, at once: captures, engine reads and pipeline lookups
    // from here on use the new channel (same MemoryManager: gpu_memory does not change).
    r.BindToChannel(channel_id);
    r.pipeline_cache.BindToChannel(channel_id);
    // The committer's mirrors get the new channel's engine state as of now; what the sequencer
    // writes later reaches them as deltas after the Command (BindCurrentChannel's real-engine
    // half, done here by the engines' owner).
    KeplerCompute* const new_compute = &channel.payload->kepler_compute;
    auto snapshot = std::make_unique<VideoCommon::MT::C11ChannelSnapshot>();
    snapshot->real3d = new3d;
    snapshot->real_compute = new_compute;
    snapshot->regs = new3d->regs;
    snapshot->state = new3d->state;
    snapshot->root_tables = new3d->root_tables;
    snapshot->draw_state = new3d->draw_manager.draw_state;
    snapshot->native_turing = new3d->GetNativeMacroEngine().native_turing;
    snapshot->compute_regs = new_compute->regs;
    snapshot->launch_description = new_compute->launch_description;
    // The Serial's state_tracker.ChangeChannel + InvalidateState set every dirty flag of the new
    // channel's real engine: the sequencer's pipeline cache (RefreshStages' Shaders flag) must
    // look the new channel's programs up again.
    new3d->dirty.flags.set();
    new3d->mt_record = true;
    new3d->mt_written.Clear();
    new3d->mt_cb_binds.clear();
    new3d->mt_root_ops.clear();
    new_compute->mt_written.Clear();
    struct ChannelCommand {
        Nx2DrawPipeline* self;
        s32 channel_id;
        VideoCommon::MT::C11ChannelSnapshot* snapshot;
        void operator()() const {
            self->C11CommitChannel(channel_id, snapshot);
        }
    };
    Command(MT::Kind::Channel, ChannelCommand{this, channel_id, snapshot.release()});
    stats.bind_cmd.fetch_add(1, std::memory_order_relaxed);
}

inline void Nx2DrawPipeline::C11CommitChannel(s32 channel_id,
                                              VideoCommon::MT::C11ChannelSnapshot* raw_snapshot) {
    const std::unique_ptr<VideoCommon::MT::C11ChannelSnapshot> snapshot{raw_snapshot};
    RasterizerVulkan& r = rasterizer;
    auto& stats = VideoCommon::MT::g_c11_serial;
    {
        // The cache binds as the Serial ran them: against the real engines (the query runtime
        // keeps the engine pointer it is given).
        auto& overrides3d = VideoCommon::MT::g_engine_overrides<Maxwell3D>;
        auto& overrides_compute = VideoCommon::MT::g_engine_overrides<KeplerCompute>;
        const size_t saved3d = overrides3d.count;
        const size_t saved_compute = overrides_compute.count;
        overrides3d.count = 0;
        overrides_compute.count = 0;
        SCOPE_EXIT {
            overrides3d.count = saved3d;
            overrides_compute.count = saved_compute;
        };
        {
            std::scoped_lock lock{r.buffer_cache.mutex, r.texture_cache.mutex};
            r.texture_cache.BindToChannel(channel_id);
            r.buffer_cache.BindToChannel(channel_id);
        }
        r.query_cache.BindToChannel(channel_id);
    }
    r.bound_channel_id = channel_id;
    // BindCurrentChannel from the snapshot (the real engines belong to the sequencer, which may
    // already be writing the new channel's registers).
    Maxwell3D* const real3d = snapshot->real3d;
    KeplerCompute* const real_compute = snapshot->real_compute;
    ChannelMirrors* pair = FindMirrors(real3d);
    if (pair == nullptr) {
        auto created = std::make_unique<ChannelMirrors>();
        created->real3d = real3d;
        created->real_compute = real_compute;
        created->mirror3d = std::make_unique<Maxwell3D>(real3d->GetMemoryManager());
        Maxwell3D& m = *created->mirror3d;
        m.regs = snapshot->regs;
        m.state = snapshot->state;
        m.root_tables = snapshot->root_tables;
        m.dirty.tables = real3d->dirty.tables; // set up at InitializeChannel, read-only since
        m.draw_manager.draw_state = snapshot->draw_state;
        m.dirty.flags.set();
        auto& overrides3d = VideoCommon::MT::g_engine_overrides<Maxwell3D>;
        ASSERT(overrides3d.count < overrides3d.pairs.size());
        overrides3d.pairs[overrides3d.count++] = {real3d, created->mirror3d.get()};
        if (real_compute != nullptr) {
            created->mirror_compute =
                std::make_unique<KeplerCompute>(real_compute->GetMemoryManager());
            created->mirror_compute->regs = snapshot->compute_regs;
            created->mirror_compute->launch_description = snapshot->launch_description;
            auto& overrides = VideoCommon::MT::g_engine_overrides<KeplerCompute>;
            ASSERT(overrides.count < overrides.pairs.size());
            overrides.pairs[overrides.count++] = {real_compute, created->mirror_compute.get()};
        }
        pair = created.get();
        mirrors.push_back(std::move(created));
        stats.mirror_created.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO(Render_Vulkan, "MT: mirrors for 3D engine {} / compute {} (C11 channel command)",
                 fmt::ptr(real3d), fmt::ptr(real_compute));
    } else {
        // Registers that changed while the channel was not bound (none expected, counted):
        // applied as deltas so their dirty flags are set.
        Maxwell3D& m = *pair->mirror3d;
        u64 differing = 0;
        for (u32 i = 0; i < Maxwell3D::Regs::NUM_REGS; ++i) {
            if (m.regs.reg_array[i] != snapshot->regs.reg_array[i]) {
                VideoCommon::MT::MirrorApplier<Maxwell3D>::Apply(m, i, snapshot->regs.reg_array[i]);
                ++differing;
            }
        }
        if (differing != 0) {
            stats.mirror_diff_regs.fetch_add(differing, std::memory_order_relaxed);
        }
        m.state = snapshot->state;
        m.root_tables = snapshot->root_tables;
        m.dirty.flags.set();
        if (pair->mirror_compute && real_compute != nullptr) {
            pair->mirror_compute->regs = snapshot->compute_regs;
        }
    }
    pair->mirror3d->GetNativeMacroEngine().native_turing = snapshot->native_turing;
    current = pair;
    VideoCommon::MT::PinEngineView<Maxwell3D>(pair->mirror3d.get());
    VideoCommon::MT::PinEngineView<KeplerCompute>(pair->mirror_compute.get());
    channel_gen.fetch_add(1, std::memory_order_release);
    // state_tracker.ChangeChannel + InvalidateState, on the mirror's flags (ChangeChannel would
    // point them at the real engine's, which the sequencer writes).
    r.state_tracker.SetFlags(&pair->mirror3d->dirty.flags);
    r.state_tracker.InvalidateState();
}

// ---- Capture repeat (cycle 12 W3, EDEN_NX2_C12_CAPTURE_REPEAT, mt/c12_capture.h) ----

inline std::array<u32, 5> Nx2DrawPipeline::C12CbufMasks(const GraphicsPipeline* pipeline) {
    std::array<u32, 5> masks{};
    const auto& infos = pipeline->StageInfos();
    for (size_t stage = 0; stage < infos.size() && stage < masks.size(); ++stage) {
        const Shader::Info& info = infos[stage];
        u32 mask = 0;
        for (const auto& desc : info.storage_buffers_descriptors) {
            mask |= 1U << (desc.cbuf_index & 31);
        }
        const auto add = [&mask](const auto& desc) {
            if (desc.handle_program != ~0U) {
                mask = ~0U; // a handle program may read any bound constant buffer
                return;
            }
            mask |= 1U << (desc.cbuf_index & 31);
            if constexpr (requires { desc.has_secondary; }) {
                if (desc.has_secondary) {
                    mask |= 1U << (desc.secondary_cbuf_index & 31);
                }
            }
        };
        for (const auto& desc : info.texture_descriptors) {
            add(desc);
        }
        for (const auto& desc : info.image_descriptors) {
            add(desc);
        }
        masks[stage] = mask;
    }
    return masks;
}

inline void Nx2DrawPipeline::C12BuildKey(const std::array<u32, 5>& masks,
                                         std::vector<u64>& out) {
    // Everything pass 1 reads besides guest memory: the tables, limits and sampler binding, the
    // channel's memory manager and the constant buffers its reads start from.
    Maxwell3D& real = *rasterizer.maxwell3d.Real();
    const auto& regs = real.regs;
    const Tegra::MemoryManager& memory = *rasterizer.gpu_memory;
    out.clear();
    out.push_back(regs.tex_header.Address());
    out.push_back(regs.tex_sampler.Address());
    out.push_back(static_cast<u64>(regs.tex_header.limit) |
                  static_cast<u64>(regs.tex_sampler.limit) << 32);
    out.push_back(static_cast<u64>(regs.sampler_binding));
    out.push_back(memory.UsesNativeTiling() ? 1 : 0);
    out.push_back(reinterpret_cast<std::uintptr_t>(&memory));
    // sync-c12: native_turing picks the storage-buffer sizing (upstream 405e5f7ac8: bare pointers
    // sized from the mapping, no size word read), so it decides which reads pass 1 makes.
    out.push_back(real.GetNativeMacroEngine().native_turing ? 1 : 0);
    for (size_t stage = 0; stage < masks.size(); ++stage) {
        if (masks[stage] == 0) {
            continue;
        }
        const auto& cbufs = real.state.shader_stages[stage].const_buffers;
        out.push_back(u64{0xC12} << 32 | stage);
        for (u32 bits = masks[stage]; bits != 0; bits &= bits - 1) {
            const size_t index = static_cast<size_t>(std::countr_zero(bits));
            if (index >= cbufs.size()) {
                break;
            }
            out.push_back(static_cast<u64>(cbufs[index].address) << 1 |
                          (cbufs[index].enabled ? 1 : 0));
        }
    }
}

inline void Nx2DrawPipeline::C12Begin(const GraphicsPipeline* pipeline) {
    namespace M12 = VideoCommon::MT;
    c12_memo = nullptr;
    c12_hit = false;
    c12_gens = false;
    c12_repeat = 0;
    c12_flags = 0;
    c12_ref_op = 0;
    if (c12_repeat_mode == '0') {
        return;
    }
    const bool active = !verify && !lockstep && resolver_count == 0;
    M12::C12CaptureLogStart(active);
    if (!active || !M12::Lite7Detail::On(c12_repeat_mode)) {
        return;
    }
    if (!c12_memos) {
        // EDEN_NX2_C13_MEMO_WAYS: the slot's ways are contiguous (1 way: the cycle 12 table).
        c12_memos = std::make_unique<C12Memo[]>(C12MemoSlots * c13_ways_alloc);
    }
    const u64 bits = reinterpret_cast<std::uintptr_t>(pipeline) >> 4;
    const size_t slot = static_cast<size_t>((bits ^ (bits >> 10) ^ (bits >> 20)) &
                                            (C12MemoSlots - 1));
    C12Memo* const set = &c12_memos[slot * c13_ways_alloc];
    c12_ref_slot = static_cast<u16>(slot);
    c12_layout_begin = rasterizer.gpu_memory->LayoutGeneration();
    M12::C12CaptureCounters& c12c = M12::g_c12_capture;
    const bool stats = M12::C12CaptureStats();
    if (stats) {
        c12c.captures.fetch_add(1, std::memory_order_relaxed);
    }
    if (M12::Lite7Detail::On(c12_gens_mode)) {
        // The generations count only while the tracking is on (Start turns it on for UBO_GEN,
        // Stop off): found off, it starts now, and generations armed before mean nothing.
        if (!M12::g_ubo_gen_on.load(std::memory_order_acquire)) {
            M12::g_ubo_gen_on.store(true, std::memory_order_seq_cst);
            ++c12_gen_epoch;
            c12_gen_on_captures = 0;
        } else if (c12_gen_on_captures < 1024) {
            // A writer that read the flag off just before it went on may still land unbumped:
            // nothing is armed until this many captures later.
            ++c12_gen_on_captures;
        }
        c12_gens = true;
    }
    // EDEN_NX2_C13_MEMO_WAYS (cycle 13 W4): more than one way for this capture ('a': per
    // capture; its 1-way half uses way 0 only, as cycle 12).
    if (c13_ways_alloc > 1) {
        const u32 ways = std::min(M12::C13MemoWaysNow(), c13_ways_alloc);
        if (ways > 1) {
            C13BeginWays(pipeline, set, ways);
            return;
        }
    }
    C12Memo& memo = set[0];
    c12_memo = &memo;
    if (memo.pipeline != pipeline) {
        memo.pipeline = pipeline;
        memo.valid = false;
        memo.ref_stored = false;
        memo.miss_streak = 0;
        memo.throttle = 0;
        memo.seq = 0;
        memo.geometry.clear();
        memo.ubo_binds.clear();
        if (stats) {
            c12c.miss_empty.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (memo.miss_streak >= M12::C12MissStreak && (++memo.throttle % M12::C12RecordEvery) != 0) {
        // A pipeline whose reads change draw after draw: neither validated nor recorded.
        if (stats) {
            c12c.miss_throttled.fetch_add(1, std::memory_order_relaxed);
        }
        c12_memo = nullptr;
        return;
    }
    if (!memo.valid) {
        if (stats) {
            c12c.miss_empty.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (memo.memory != rasterizer.gpu_memory || memo.layout_gen != c12_layout_begin) {
        if (stats) {
            c12c.miss_key.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    C12BuildKey(memo.cbuf_masks, c12_key);
    if (c12_key != memo.key) {
        if (stats) {
            c12c.miss_key.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    const auto begin = stats ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
    const bool same = C12Validate(memo);
    if (stats) {
        c12c.validate_ns.fetch_add(
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - begin)
                                 .count()),
            std::memory_order_relaxed);
    }
    if (!same) {
        if (stats) {
            c12c.miss_bytes.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    c12_hit = true;
    if (stats) {
        c12c.hits.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---- Two-way capture memo (cycle 13 W4 M1, EDEN_NX2_C13_MEMO_WAYS, mt/c13_memo.h) ----

inline u64 Nx2DrawPipeline::C13ViewTag(const GraphicsPipeline* pipeline) {
    namespace M13 = VideoCommon::MT;
    const char mode = M13::C13MemoTagMode();
    if (mode == '0') {
        return 0;
    }
    // The first stage with uniform buffers (the vertex stage in practice): its bindings carry
    // the view (camera) buffer. '1': its first binding; 'h': all of them.
    const auto& infos = pipeline->StageInfos();
    const Maxwell3D& real = *rasterizer.maxwell3d.Real();
    const auto& stages = real.state.shader_stages;
    for (size_t stage = 0; stage < infos.size() && stage < stages.size(); ++stage) {
        const u32 mask = infos[stage].constant_buffer_mask;
        if (mask == 0) {
            continue;
        }
        const auto& cbufs = stages[stage].const_buffers;
        u64 hash = 0xcbf29ce484222325ULL ^ static_cast<u64>(stage);
        for (u32 bits = mask; bits != 0; bits &= bits - 1) {
            const size_t index = static_cast<size_t>(std::countr_zero(bits));
            if (index >= cbufs.size()) {
                break;
            }
            const u64 word = static_cast<u64>(cbufs[index].address) << 1 |
                             (cbufs[index].enabled ? 1 : 0);
            hash = (hash ^ word ^ (static_cast<u64>(index) << 56)) * 0x100000001b3ULL;
            if (mode != 'h') {
                break;
            }
        }
        return hash | 1; // never 0 (0: a way with no tag)
    }
    return 0;
}

inline void Nx2DrawPipeline::C13BeginWays(const GraphicsPipeline* pipeline, C12Memo* set,
                                          u32 ways) {
    namespace M12 = VideoCommon::MT;
    M12::C12CaptureCounters& c12c = M12::g_c12_capture;
    M12::C13MemoCounters& c13c = M12::g_c13_memo;
    const bool stats = M12::C12CaptureStats();
    const bool stats13 = M12::C13Stats();
    const u64 tag = C13ViewTag(pipeline);
    // This pipeline's ways: same view tag first, then most recently used first.
    std::array<u32, M12::kC13MaxWays> order{};
    u32 count = 0;
    u32 mru = ways;
    u32 tag_ways = 0;
    for (u32 way = 0; way < ways; ++way) {
        if (set[way].pipeline != pipeline) {
            continue;
        }
        order[count++] = way;
        if (mru == ways || set[way].c13_last_use > set[mru].c13_last_use) {
            mru = way;
        }
        if (tag != 0 && set[way].c13_tag == tag) {
            ++tag_ways;
        }
    }
    std::sort(order.begin(), order.begin() + count, [set, tag](u32 a, u32 b) {
        const bool tag_a = tag != 0 && set[a].c13_tag == tag;
        const bool tag_b = tag != 0 && set[b].c13_tag == tag;
        if (tag_a != tag_b) {
            return tag_a;
        }
        return set[a].c13_last_use > set[b].c13_last_use;
    });
    if (stats13) {
        c13c.cap_lookups.fetch_add(1, std::memory_order_relaxed);
        c13c.cap_ways_used.fetch_add(count, std::memory_order_relaxed);
    }
    // The cycle 12 throttle, per way: only a pipeline whose every way is in a miss streak
    // skips (except every C12RecordEvery captures, counted on its most recent way).
    if (count != 0) {
        bool all_streak = true;
        for (u32 i = 0; i < count; ++i) {
            all_streak &= set[order[i]].miss_streak >= M12::C12MissStreak;
        }
        if (all_streak && (++set[mru].throttle % M12::C12RecordEvery) != 0) {
            if (stats) {
                c12c.miss_throttled.fetch_add(1, std::memory_order_relaxed);
            }
            if (stats13) {
                c13c.cap_throttled.fetch_add(1, std::memory_order_relaxed);
            }
            c12_memo = nullptr;
            return;
        }
    }
    // At most two validations per capture (the cycle 12 cost is one).
    enum : u32 { MissEmpty = 0, MissKey = 1, MissBytes = 2 };
    u32 miss = MissEmpty;
    bool key_built = false;
    u32 validations = 0;
    for (u32 i = 0; i < count; ++i) {
        C12Memo& candidate = set[order[i]];
        if (!candidate.valid) {
            continue;
        }
        if (candidate.memory != rasterizer.gpu_memory ||
            candidate.layout_gen != c12_layout_begin) {
            miss = std::max<u32>(miss, MissKey);
            continue;
        }
        if (!key_built) {
            // The masks are the pipeline's (C12CbufMasks): the same for all its ways.
            C12BuildKey(candidate.cbuf_masks, c12_key);
            key_built = true;
        }
        if (c12_key != candidate.key) {
            miss = std::max<u32>(miss, MissKey);
            continue;
        }
        if (validations == 2) {
            break;
        }
        ++validations;
        const auto begin = stats ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
        const bool same = C12Validate(candidate);
        if (stats) {
            c12c.validate_ns.fetch_add(
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count()),
                std::memory_order_relaxed);
        }
        if (!same) {
            miss = MissBytes;
            continue;
        }
        c12_memo = &candidate;
        c12_hit = true;
        candidate.c13_last_use = ++c13_use_clock;
        if (stats) {
            c12c.hits.fetch_add(1, std::memory_order_relaxed);
        }
        if (stats13) {
            const bool is_mru = order[i] == mru;
            (is_mru ? c13c.cap_hit_mru : c13c.cap_hit_other)
                .fetch_add(1, std::memory_order_relaxed);
            if (tag != 0 && candidate.c13_tag == tag) {
                c13c.cap_hit_tag.fetch_add(1, std::memory_order_relaxed);
            }
            if (validations > 1) {
                c13c.cap_second.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return;
    }
    if (stats) {
        std::atomic<u64>& reason = miss == MissBytes ? c12c.miss_bytes
                                   : miss == MissKey ? c12c.miss_key
                                                     : c12c.miss_empty;
        reason.fetch_add(1, std::memory_order_relaxed);
    }
    // The way C12AfterPass1 records into: this pipeline's same-view way when the tag tells
    // its ways apart (one way has it, another does not), else the least recently used way of
    // the slot (a free one first, whichever pipeline held it).
    C12Memo* victim = nullptr;
    if (tag_ways == 1 && count > 1) {
        victim = &set[order[0]];
        if (stats13) {
            c13c.cap_replace_tag.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        u32 lru = 0;
        for (u32 way = 1; way < ways; ++way) {
            const bool free_way = set[way].pipeline == nullptr;
            const bool free_lru = set[lru].pipeline == nullptr;
            if (free_way != free_lru ? free_way
                                     : set[way].c13_last_use < set[lru].c13_last_use) {
                lru = way;
            }
        }
        victim = &set[lru];
        if (stats13) {
            if (count == 0) {
                c13c.cap_new.fetch_add(1, std::memory_order_relaxed);
            }
            if (victim->pipeline != nullptr && victim->pipeline != pipeline) {
                c13c.cap_evict_other.fetch_add(1, std::memory_order_relaxed);
            } else if (victim->pipeline == pipeline) {
                c13c.cap_replace_lru.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (victim->pipeline != pipeline) {
        // As cycle 12 on a pipeline change; the new way starts with the pipeline's streak (its
        // most recent way's), so a pipeline that never repeats still throttles.
        victim->pipeline = pipeline;
        victim->valid = false;
        victim->ref_stored = false;
        victim->miss_streak = mru != ways ? set[mru].miss_streak : 0;
        victim->throttle = 0;
        victim->seq = 0;
        victim->geometry.clear();
        victim->ubo_binds.clear();
    }
    victim->c13_tag = tag;
    victim->c13_last_use = ++c13_use_clock;
    c12_memo = victim;
}

inline bool Nx2DrawPipeline::C12Validate(C12Memo& memo) {
    namespace M12 = VideoCommon::MT;
    const size_t num_pages = memo.pages.size();
    if (num_pages > M12::C12MaxPages) {
        return false;
    }
    std::array<const u8*, M12::C12MaxPages> hosts{};
    std::array<u64, M12::C12MaxPages> now_gen{};
    std::array<u8, M12::C12MaxPages> skip{};
    M12::C12CaptureCounters& c12c = M12::g_c12_capture;
    const bool stats = M12::C12CaptureStats();
    const bool armable = c12_gens && c12_gen_on_captures >= 1024;
    const bool probe = (++c12_probe % M12::C12ProbeEvery) == 0;
    // Protection and generations under the tracker lock (as SeqStageUniformGen), each
    // generation read before its page's bytes are compared: a write after the read bumps it
    // (the next validation compares again), whatever the compare saw.
    std::optional<MTDetail::SeqTrackerLock> lock;
    for (size_t p = 0; p < num_pages; ++p) {
        M12::C12MemoPage& page = memo.pages[p];
        hosts[p] = rasterizer.device_memory.GetPointer<u8>(page.page);
        if (hosts[p] == nullptr) {
            return false;
        }
        if (!c12_gens || (page.unprotected >= M12::C12UnprotectedStreak && !probe)) {
            continue;
        }
        if (!lock) {
            lock.emplace(rasterizer.buffer_cache);
        }
        const bool protected_page =
            !rasterizer.buffer_cache.IsRegionCpuModified(page.page, M12::C12PageSize) &&
            !Tegra::Nx2AnyPage(Tegra::g_nx2_query_pages, page.page, M12::C12PageSize);
        if (!protected_page) {
            if (page.unprotected < 255) {
                ++page.unprotected;
            }
            page.gen = 0;
            if (stats) {
                c12c.pages_unprot.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        page.unprotected = 0;
        now_gen[p] = M12::C12PageGeneration(page.page);
        if (armable && page.gen != 0 && page.gen == now_gen[p] && page.epoch == c12_gen_epoch) {
            skip[p] = 1;
        }
    }
    for (const M12::C12MemoCheck& check : memo.checks) {
        if (skip[check.page] != 0 && !c12_check) {
            continue;
        }
        if (std::memcmp(hosts[check.page] + check.offset, memo.bytes.data() + check.bytes_offset,
                        check.size) != 0) {
            if (skip[check.page] != 0) {
                // CHECK: a page the generations called unchanged was written (a write that
                // bypasses the tracker).
                c12c.gen_stale.fetch_add(1, std::memory_order_relaxed);
                M12::C12CaptureReportMismatch("generation-skipped page",
                                             memo.pages[check.page].page);
            }
            return false;
        }
    }
    u64 skipped = 0;
    u64 armed = 0;
    for (size_t p = 0; p < num_pages; ++p) {
        if (skip[p] != 0) {
            ++skipped;
            continue;
        }
        M12::C12MemoPage& page = memo.pages[p];
        if (armable && now_gen[p] != 0 && (page.gen != now_gen[p] || page.epoch != c12_gen_epoch)) {
            page.gen = now_gen[p];
            page.epoch = c12_gen_epoch;
            ++armed;
        }
    }
    if (stats) {
        c12c.pages_gen.fetch_add(skipped, std::memory_order_relaxed);
        c12c.pages_cmp.fetch_add(num_pages - skipped, std::memory_order_relaxed);
        c12c.pages_armed.fetch_add(armed, std::memory_order_relaxed);
    }
    return true;
}

inline void Nx2DrawPipeline::C12AfterPass1(const GraphicsPipeline* pipeline, C12SsboList& ssbos) {
    namespace M12 = VideoCommon::MT;
    if (c12_memo == nullptr) {
        return;
    }
    C12Memo& memo = *c12_memo;
    if (c12_hit && !c12_check) {
        // Pass 1 was skipped: its results are the memo's.
        raw_reads.assign(memo.reads.begin(), memo.reads.end());
        raw_bytes.assign(memo.bytes.begin(), memo.bytes.end());
        ssbo_entries.assign(memo.ssbo_entries.begin(), memo.ssbo_entries.end());
        ssbos.assign(memo.ssbos.begin(), memo.ssbos.end());
        tic_valid.assign(memo.tic_valid.begin(), memo.tic_valid.end());
        tic_valid_count = memo.tic_valid_count;
        if (c11_repeat_mode != '0' && resolver_count == 0) {
            c11_repeat_key.assign(memo.c11_key.begin(), memo.c11_key.end());
        }
        if (M12::C12CaptureStats()) {
            M12::g_c12_capture.reads_restored.fetch_add(memo.reads.size(),
                                                       std::memory_order_relaxed);
        }
    } else if (c12_hit) {
        // EDEN_NX2_C12_CAPTURE_REPEAT_CHECK: pass 1 ran; it must equal the memo.
        if (!C12Compare(memo, ssbos)) {
            c12_hit = false;
            if (memo.miss_streak < 0xffff) {
                ++memo.miss_streak;
            }
            C12Record(memo, pipeline, ssbos);
            return;
        }
    } else {
        if (memo.miss_streak < 0xffff) {
            ++memo.miss_streak;
        }
        // Cycle 13 W3 L2 (mt/c13_front.h): a draw before a pending kick: no record work (the
        // memo keeps its old entry; c12_memo cleared so C12Finish does not touch it).
        if (VideoCommon::GpuSplit::Get().C13SeqBeforeKick()) [[unlikely]] {
            VideoCommon::MT::g_c13_seq_skips.fetch_add(1, std::memory_order_relaxed);
            c12_memo = nullptr;
            return;
        }
        C12Record(memo, pipeline, ssbos);
        return;
    }
    memo.miss_streak = 0;
    // EmitDraw gives this capture the seq next_seq (no return after pass 1).
    const u64 delta = next_seq - memo.seq;
    if (memo.seq != 0 && delta != 0 && delta <= 0xffffffffULL) {
        c12_repeat = static_cast<u32>(delta);
        c12_flags = M12::C12SameReads | M12::C12SameSsbo | M12::C12SameTicValid;
    }
    memo.seq = next_seq;
}

inline void Nx2DrawPipeline::C12Record(C12Memo& memo, const GraphicsPipeline* pipeline,
                                       const C12SsboList& ssbos) {
    namespace M12 = VideoCommon::MT;
    memo.valid = false;
    memo.ref_stored = false;
    const bool stats = M12::C12CaptureStats();
    const auto refuse = [&] {
        if (stats) {
            M12::g_c12_capture.record_refused.fetch_add(1, std::memory_order_relaxed);
        }
    };
    // Translations of pass 1 under one stable page table layout only.
    const u64 layout = rasterizer.gpu_memory->LayoutGeneration();
    if ((layout & 1) == 0 || layout != c12_layout_begin || raw_reads.size() > M12::C12MaxReads) {
        refuse();
        return;
    }
    memo.pages.clear();
    memo.checks.clear();
    for (const RawRead& read : raw_reads) {
        const std::optional<DAddr> device_addr = SeqTranslate(read.gpu_addr);
        if (!device_addr) {
            refuse();
            return;
        }
        const u64 page = *device_addr & ~(M12::C12PageSize - 1);
        const u64 offset = *device_addr & (M12::C12PageSize - 1);
        if (offset + read.size > M12::C12PageSize) {
            refuse();
            return;
        }
        size_t index = 0;
        while (index < memo.pages.size() && memo.pages[index].page != page) {
            ++index;
        }
        if (index == memo.pages.size()) {
            if (index >= M12::C12MaxPages) {
                refuse();
                return;
            }
            M12::C12MemoPage entry{};
            entry.page = page;
            memo.pages.push_back(entry);
        }
        M12::C12MemoCheck check{};
        check.page = static_cast<u16>(index);
        check.offset = static_cast<u16>(offset);
        check.size = read.size;
        check.bytes_offset = read.offset;
        memo.checks.push_back(check);
    }
    // The bytes as the validation will see them (through the host pointers): a read the memory
    // manager answered otherwise (the syncpoint shim) or one written since gives no memo.
    std::array<const u8*, M12::C12MaxPages> hosts{};
    for (size_t p = 0; p < memo.pages.size(); ++p) {
        hosts[p] = rasterizer.device_memory.GetPointer<u8>(memo.pages[p].page);
        if (hosts[p] == nullptr) {
            refuse();
            return;
        }
    }
    for (const M12::C12MemoCheck& check : memo.checks) {
        if (std::memcmp(hosts[check.page] + check.offset, raw_bytes.data() + check.bytes_offset,
                        check.size) != 0) {
            refuse();
            return;
        }
    }
    memo.layout_gen = layout;
    memo.memory = rasterizer.gpu_memory;
    memo.cbuf_masks = C12CbufMasks(pipeline);
    C12BuildKey(memo.cbuf_masks, memo.key);
    memo.reads.assign(raw_reads.begin(), raw_reads.end());
    memo.bytes.assign(raw_bytes.begin(), raw_bytes.end());
    memo.ssbo_entries.assign(ssbo_entries.begin(), ssbo_entries.end());
    memo.ssbos.assign(ssbos.begin(), ssbos.end());
    memo.tic_valid.assign(tic_valid.begin(), tic_valid.end());
    memo.tic_valid_count = tic_valid_count;
    if (c11_repeat_mode != '0' && resolver_count == 0) {
        memo.c11_key.assign(c11_repeat_key.begin(), c11_repeat_key.end());
    } else {
        memo.c11_key.clear();
    }
    memo.geometry.clear();
    memo.ubo_binds.clear();
    memo.seq = next_seq;
    memo.valid = true;
    if (stats) {
        M12::g_c12_capture.records.fetch_add(1, std::memory_order_relaxed);
    }
}

inline bool Nx2DrawPipeline::C12Compare(const C12Memo& memo, const C12SsboList& ssbos) {
    namespace M12 = VideoCommon::MT;
    M12::g_c12_capture.checks.fetch_add(1, std::memory_order_relaxed);
    if (raw_reads.size() != memo.reads.size()) {
        M12::C12CaptureReportMismatch("read count", raw_reads.size());
        return false;
    }
    for (size_t i = 0; i < raw_reads.size(); ++i) {
        const RawRead& now = raw_reads[i];
        const RawRead& kept = memo.reads[i];
        if (now.gpu_addr != kept.gpu_addr || now.size != kept.size ||
            now.descriptor != kept.descriptor) {
            M12::C12CaptureReportMismatch("read address", now.gpu_addr);
            return false;
        }
        if (std::memcmp(raw_bytes.data() + now.offset, memo.bytes.data() + kept.offset,
                        now.size) != 0) {
            M12::C12CaptureReportMismatch("read bytes", now.gpu_addr);
            return false;
        }
    }
    if (ssbo_entries.size() != memo.ssbo_entries.size() ||
        (!ssbo_entries.empty() &&
         std::memcmp(ssbo_entries.data(), memo.ssbo_entries.data(),
                     ssbo_entries.size() * sizeof(ssbo_entries[0])) != 0)) {
        M12::C12CaptureReportMismatch("storage buffer entries", ssbo_entries.size());
        return false;
    }
    if (ssbos.size() != memo.ssbos.size()) {
        M12::C12CaptureReportMismatch("storage bindings", ssbos.size());
        return false;
    }
    for (size_t i = 0; i < ssbos.size(); ++i) {
        const M12::C12SsboCapture& a = ssbos[i];
        const M12::C12SsboCapture& b = memo.ssbos[i];
        if (a.device_addr != b.device_addr || a.size != b.size || a.is_written != b.is_written ||
            a.stage != b.stage) {
            M12::C12CaptureReportMismatch("storage binding", a.device_addr);
            return false;
        }
    }
    if (tic_valid_count != memo.tic_valid_count || tic_valid != memo.tic_valid) {
        M12::C12CaptureReportMismatch("TIC validity", tic_valid_count);
        return false;
    }
    if (c11_repeat_mode != '0' && resolver_count == 0 && c11_repeat_key != memo.c11_key) {
        M12::C12CaptureReportMismatch("C11 repeat key", c11_repeat_key.size());
        return false;
    }
    return true;
}

inline bool Nx2DrawPipeline::C12Finish() {
    namespace M12 = VideoCommon::MT;
    M12::C12CaptureMaybeLog();
    if (c12_memo == nullptr || !c12_memo->valid) {
        return false;
    }
    C12Memo& memo = *c12_memo;
    const bool stats = M12::C12CaptureStats();
    M12::C12CaptureCounters& c12c = M12::g_c12_capture;
    // The rest of the binding state the committer's whole-draw repeat may compare cheaply.
    c12_ubo_binds.clear();
    for (const M12::UboEntry& entry : ubos) {
        c12_ubo_binds.push_back(u64{entry.stage} | u64{entry.index} << 8 |
                                u64{entry.staged != 0 ? 1U : 0U} << 16 | u64{entry.size} << 32);
        c12_ubo_binds.push_back(entry.device_addr);
    }
    if (c12_repeat != 0) {
        if (geometry_entries.size() == memo.geometry.size() &&
            (geometry_entries.empty() ||
             std::memcmp(geometry_entries.data(), memo.geometry.data(),
                         geometry_entries.size() * sizeof(geometry_entries[0])) == 0)) {
            c12_flags |= M12::C12SameGeometry;
        }
        if (c12_ubo_binds == memo.ubo_binds) {
            c12_flags |= M12::C12SameUboBinds;
        }
        if (uploads.empty()) {
            c12_flags |= M12::C12NoUploads;
        }
        if (stats) {
            c12c.flagged.fetch_add(1, std::memory_order_relaxed);
            c12c.flag_ssbo.fetch_add(ssbo_entries.empty() ? 0 : 1, std::memory_order_relaxed);
            c12c.flag_geometry.fetch_add((c12_flags & M12::C12SameGeometry) != 0 ? 1 : 0,
                                         std::memory_order_relaxed);
            c12c.flag_ubo.fetch_add((c12_flags & M12::C12SameUboBinds) != 0 ? 1 : 0,
                                    std::memory_order_relaxed);
            c12c.flag_no_uploads.fetch_add((c12_flags & M12::C12NoUploads) != 0 ? 1 : 0,
                                           std::memory_order_relaxed);
        }
    }
    // Now of the packet memo.seq (this one).
    memo.geometry.assign(geometry_entries.begin(), geometry_entries.end());
    memo.ubo_binds.swap(c12_ubo_binds);
    // EDEN_NX2_C12_CAPTURE_REF: compact packets only (the old layout has no operation). Not
    // with EDEN_NX2_C13_MEMO_WAYS > 1: the readers keep one overlay per memo slot, not per way.
    if (!c12_ref || !compact_packet || Batching() || c13_ways_alloc > 1) {
        return false;
    }
    if (c12_hit && memo.ref_stored) {
        c12_ref_op = static_cast<u8>(M12::C12RefOp::Use);
        if (stats) {
            c12c.ref_use.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }
    c12_ref_op = static_cast<u8>(M12::C12RefOp::Store);
    memo.ref_stored = true;
    if (stats) {
        c12c.ref_store.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

} // namespace Vulkan
