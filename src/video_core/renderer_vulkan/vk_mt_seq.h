// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Phase 7 of docs/mt-plan.md ("sequencer diet"): items phase 4 ran as Serial (the sequencer
// waiting for the committer) become Commands or draw packets carrying what the sequencer read.
// Included by vk_rasterizer.cpp after vk_mt_pipeline.h.

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "video_core/mt/seq_diet.h"
#include "video_core/renderer_vulkan/vk_mt_pipeline.h"

namespace Vulkan {

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
    using View = VideoCommon::MT::CaptureView;
    const DAddr device_addr = entry.device_addr;
    const u32 size = entry.size;
    if (VideoCommon::MT::UboGen() && size != 0 && size <= View::UboMemoBytes &&
        entry.stage < View::UboMemoStages && entry.index < View::UboMemoSlots) {
        UniformMemo& memo = seq_ubo_memo[entry.stage][entry.index];
        // Guest CPU writes reach the generations only through write traps: the pages must be
        // protected (not CPU-modified in the buffer tracker) now and when the memo was taken
        // (dropping the protection bumps them). Query reports write guest memory directly.
        const bool protected_pages =
            !rasterizer.buffer_cache.IsRegionCpuModified(device_addr, size) &&
            !Tegra::Nx2AnyPage(Tegra::g_nx2_query_pages, device_addr, size);
        // Read before the bytes: a write in between only makes the next capture miss.
        const u64 generation =
            protected_pages ? VideoCommon::MT::UboWriteGenerations().Read(device_addr, size) : 0;
        if (generation != 0 && memo.generation == generation && memo.device_addr == device_addr &&
            memo.size == size) {
            entry.staged = 2;
            counters.ubo_gen_hits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
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
}

inline void Nx2DrawPipeline::KeepUniformMemos(std::span<const VideoCommon::MT::UboEntry> entries,
                                              const u8* blob_data) {
    using View = VideoCommon::MT::CaptureView;
    if (!ubo_memo_bytes) {
        return;
    }
    for (const VideoCommon::MT::UboEntry& entry : entries) {
        if (entry.staged == 1 && entry.memo != 0) {
            std::memcpy(ubo_memo_bytes.get() +
                            (size_t{entry.stage} * View::UboMemoSlots + entry.index) *
                                View::UboMemoBytes,
                        blob_data + entry.offset, entry.size);
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
        MTDetail::SeqTrackerLock lock{r.buffer_cache};
        if (r.buffer_cache.IsRegionGpuModified(*device_addr, 8)) {
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
    {
        MTDetail::SeqTrackerLock lock{buffer_cache};
        // GPU data at the destination may still come back to guest memory from a download taken
        // before this copy, after the copy below wrote it: only the old path orders those.
        if (buffer_cache.IsRegionGpuModified(*dst, amount)) {
            counters.dma_serial.fetch_add(1, std::memory_order_relaxed);
            return serial();
        }
        // DMACopy only acts on ranges the buffer cache holds (or GPU writes still in flight
        // will create); otherwise MaxwellDMA copies guest memory, as when it returns false.
        held = buffer_cache.MtIsRegionRegistered(*src, amount) ||
               buffer_cache.MtIsRegionRegistered(*dst, amount) ||
               buffer_cache.IsRegionGpuModified(*src, amount);
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
    {
        MTDetail::SeqTrackerLock lock{r.buffer_cache};
        if (r.buffer_cache.IsRegionGpuModified(*dst, amount * sizeof(u32))) {
            counters.dma_serial.fetch_add(1, std::memory_order_relaxed);
            return serial();
        }
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

} // namespace Vulkan
