// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Multithreaded GPU back, phase 5 (docs/mt-phase5-spec.md): K resolver threads read every
// stream entry, keep their own Maxwell3D mirror, and resolve the draw packets of the batches
// they own (batch % K) against the caches without locks. The committer validates each result
// (generations, image versions, buffer epoch) at commit. Included at the end of
// vk_mt_pipeline.h only.

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "video_core/mt/arena.h"
#include "video_core/mt/capture.h"
#include "video_core/mt/engine_ptr.h"
#include "video_core/mt/epoch.h"
#include "video_core/mt/resolve_packet.h"
#include "video_core/mt/resolve_sync.h"
#include "video_core/mt/role.h"
#include "video_core/shader_handle_program.h"
#include "video_core/textures/texture.h"
#include "video_core/renderer_vulkan/vk_mt_resolve_types.h"

namespace Vulkan::MT {

/// Guest memory reads of a resolver: only the packet's overlay, never guest memory.
struct OverlayReader {
    const VideoCommon::MT::CaptureView* view;
    mutable bool miss = false;
    template <typename T>
    T Read(u64 gpu_addr) const {
        T value{};
        if (!view->Read(gpu_addr, &value, sizeof(T))) {
            miss = true;
        }
        return value;
    }
};

class Resolver {
    using Maxwell3D = Tegra::Engines::Maxwell3D;
    using KeplerCompute = Tegra::Engines::KeplerCompute;

public:
    Resolver(Nx2DrawPipeline& pipeline_, u32 index_, u32 count_)
        : pipeline{pipeline_}, index{index_}, count{count_} {}

    ~Resolver() {
        Stop();
    }

    Resolver(const Resolver&) = delete;
    Resolver& operator=(const Resolver&) = delete;

    /// Before the sequencer runs (it starts at the stream's write position 0).
    void AddConsumer();
    void Start();
    void Stop();

    VideoCommon::MT::BatchCursor cursor;
    VideoCommon::MT::RingArena<u64> results{size_t{1} << 20}; // 8 MiB
    ResolverCounters counters;

private:
    struct Mirrors {
        Maxwell3D* real3d{};
        KeplerCompute* real_compute{};
        std::unique_ptr<Maxwell3D> mirror3d;
        std::unique_ptr<KeplerCompute> mirror_compute;
        VideoCommon::TextureCacheChannelInfo* tex_channel{};
    };

    void Loop(std::stop_token stop);
    void OnEntry(const u64* entry, std::stop_token& stop);
    void BindChannel();
    void ResolveDraw(const u64* entry);
    bool ResolveOne(const Nx2DrawPipeline::PacketView& packet, ResolvedDraw& out);
    void EnterBatch();
    void LeaveBatch();

    Nx2DrawPipeline& pipeline;
    const u32 index;
    const u32 count;
    u32 consumer{};
    bool has_consumer = false;
    int epoch_reader = -1;
    bool in_batch = false;
    u64 current_batch = 0;
    std::vector<std::unique_ptr<Mirrors>> mirrors;
    Mirrors* current{};
    u32 channel_gen = 0;
    u64 inject_counter = 0;
    std::vector<const ImageView*> view_objects;
    std::vector<u8> view_rescaled;
    std::vector<Sampler*> sampler_objects;
    std::jthread thread;
    std::atomic<bool> ready{false};
};

} // namespace Vulkan::MT

// ---------------------------------------------------------------------------------------------

namespace Vulkan::MT {

inline void Resolver::AddConsumer() {
    consumer = pipeline.stream.AddConsumer();
    has_consumer = true;
}

inline void Resolver::Start() {
    ready.store(false, std::memory_order_relaxed);
    thread = std::jthread([this](std::stop_token stop) { Loop(stop); });
    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

inline void Resolver::Stop() {
    if (thread.joinable()) {
        thread.request_stop();
        pipeline.stream.WakeAll();
        thread.join();
    }
    if (has_consumer) {
        pipeline.stream.RemoveConsumer(consumer);
        has_consumer = false;
    }
}

inline void Resolver::Loop(std::stop_token stop) {
    Common::SetCurrentThreadName(fmt::format("GPUResolve{}", index).c_str());
    Tegra::Nx2ApplyTimerSlack();
    VideoCommon::MT::g_role = VideoCommon::MT::Role::Resolver;
    Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    Common::SetCurrentThreadToPerformanceCores();
    epoch_reader = pipeline.epochs.RegisterReader();
    ASSERT(epoch_reader >= 0);
    BindChannel();
    ready.store(true, std::memory_order_release);
    const bool timed = VideoCommon::MT::Stats();
    while (true) {
        const u64* const entry = pipeline.stream.Peek(consumer);
        if (entry == nullptr) {
            if (stop.stop_requested()) {
                break;
            }
            // Hold no guard while idle: the committer's Collect must be able to advance.
            LeaveBatch();
            const auto begin = timed ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
            pipeline.stream.WaitForData(consumer, [&] { return stop.stop_requested(); });
            if (timed) {
                counters.wait_ns.fetch_add(
                    static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - begin)
                                         .count()),
                    std::memory_order_relaxed);
            }
            continue;
        }
        OnEntry(entry, stop);
        pipeline.stream.Advance(consumer, entry);
    }
    LeaveBatch();
    VideoCommon::MT::g_engine_overrides<Maxwell3D>.count = 0;
    VideoCommon::MT::g_engine_overrides<KeplerCompute>.count = 0;
    current = nullptr;
    mirrors.clear();
    pipeline.epochs.UnregisterReader(epoch_reader);
    epoch_reader = -1;
    VideoCommon::MT::g_role = VideoCommon::MT::Role::None;
}

inline void Resolver::EnterBatch() {
    if (!in_batch) {
        pipeline.epochs.Enter(epoch_reader);
        in_batch = true;
    }
}

inline void Resolver::LeaveBatch() {
    if (in_batch) {
        pipeline.epochs.Exit(epoch_reader);
        in_batch = false;
    }
}

inline void Resolver::BindChannel() {
    // Only while the sequencer is quiescent (Start, or a channel barrier it waits on).
    RasterizerVulkan& r = pipeline.rasterizer;
    Maxwell3D* const real3d = r.maxwell3d.Real();
    KeplerCompute* const real_compute = r.kepler_compute.Real();
    channel_gen = pipeline.channel_gen.load(std::memory_order_acquire);
    if (real3d == nullptr) {
        current = nullptr;
        return;
    }
    Mirrors* pair = nullptr;
    for (const auto& entry : mirrors) {
        if (entry->real3d == real3d) {
            pair = entry.get();
        }
    }
    if (pair == nullptr) {
        auto created = std::make_unique<Mirrors>();
        created->real3d = real3d;
        created->real_compute = real_compute;
        created->mirror3d = std::make_unique<Maxwell3D>(real3d->GetMemoryManager());
        created->mirror3d->dirty.tables = real3d->dirty.tables;
        if (real_compute != nullptr) {
            created->mirror_compute =
                std::make_unique<KeplerCompute>(real_compute->GetMemoryManager());
        }
        auto& overrides3d = VideoCommon::MT::g_engine_overrides<Maxwell3D>;
        ASSERT(overrides3d.count < overrides3d.pairs.size());
        overrides3d.pairs[overrides3d.count++] = {real3d, created->mirror3d.get()};
        if (real_compute != nullptr) {
            auto& overrides = VideoCommon::MT::g_engine_overrides<KeplerCompute>;
            ASSERT(overrides.count < overrides.pairs.size());
            overrides.pairs[overrides.count++] = {real_compute, created->mirror_compute.get()};
        }
        pair = created.get();
        mirrors.push_back(std::move(created));
    }
    Maxwell3D& m = *pair->mirror3d;
    m.regs = real3d->regs;
    m.state = real3d->state;
    m.root_tables = real3d->root_tables;
    m.draw_manager.draw_state = real3d->draw_manager.draw_state;
    m.GetNativeMacroEngine().native_turing = real3d->GetNativeMacroEngine().native_turing;
    m.dirty.flags.set();
    if (pair->mirror_compute && real_compute != nullptr) {
        pair->mirror_compute->regs = real_compute->regs;
        pair->mirror_compute->launch_description = real_compute->launch_description;
    }
    pair->tex_channel = r.texture_cache.MtChannelState();
    current = pair;
}

inline void Resolver::OnEntry(const u64* entry, std::stop_token& stop) {
    using SpmcStream = VideoCommon::MT::SpmcStream;
    const u64 head = entry[0];
    switch (static_cast<StreamEntry>(SpmcStream::HeaderKind(head))) {
    case StreamEntry::Deltas:
    case StreamEntry::ComputeDeltas:
    case StreamEntry::CbBind:
    case StreamEntry::RootTable:
        if (current != nullptr) {
            Nx2DrawPipeline::ApplyMirrorEntry(current->mirror3d.get(), current->mirror_compute.get(),
                                              entry);
        }
        break;
    case StreamEntry::DrawPacket: {
        u64 batch;
        std::memcpy(&batch, reinterpret_cast<const u8*>(entry + 1) +
                                offsetof(Nx2DrawPipeline::DrawHeader, batch),
                    sizeof(batch));
        if (batch % count == index) {
            ResolveDraw(entry);
        }
        break;
    }
    case StreamEntry::BatchEnd: {
        const u64 batch = entry[1];
        if (batch % count == index) {
            LeaveBatch();
            counters.batches.fetch_add(1, std::memory_order_relaxed);
            cursor.Publish(batch);
        }
        break;
    }
    case StreamEntry::Serial: {
        auto* const acks = reinterpret_cast<VideoCommon::MT::BarrierAcks*>(entry[4]);
        if (acks == nullptr) {
            break;
        }
        // A channel barrier: once the committer ran it (the sequencer still waits for our
        // ack), copy the newly bound channel's state.
        LeaveBatch();
        const auto* const done = reinterpret_cast<const std::atomic<u32>*>(entry[3]);
        for (u32 spins = 0; done->load(std::memory_order_acquire) == 0; ++spins) {
            if (stop.stop_requested()) {
                return;
            }
            if (spins < 4096) {
                VideoCommon::MT::CpuPause();
            } else {
                std::this_thread::yield();
            }
        }
        BindChannel();
        acks->Ack();
        break;
    }
    default:
        break;
    }
}

inline void Resolver::ResolveDraw(const u64* entry) {
    const bool timed = VideoCommon::MT::Stats();
    const auto begin =
        timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const Nx2DrawPipeline::PacketView packet = Nx2DrawPipeline::ParsePacket(entry);
    EnterBatch();
    current_batch = packet.header.batch;
    ResolvedDraw& out = pipeline.resolved.Slot(packet.header.seq);
    out = ResolvedDraw{};
    out.seq = packet.header.seq;
    out.batch = packet.header.batch;
    out.status = ResolveStatus::Slow;
    out.reason = Other;
    out.resolver = static_cast<u8>(index);
    out.channel_gen = channel_gen;
    out.pipeline = packet.header.pipeline;
    out.buffer_epoch = pipeline.rasterizer.buffer_cache.MtBufferEpoch();
    bool ok = false;
    try {
        ok = ResolveOne(packet, out);
    } catch (const std::exception& e) {
        static std::atomic<bool> logged{};
        if (!logged.exchange(true)) {
            LOG_ERROR(Render_Vulkan, "MT resolver {}: draw {} threw: {}", index, out.seq, e.what());
        }
        out.reason = Exception;
    }
    if (ok && VideoCommon::MT::ResolverChecks() >= 2 && (++inject_counter % 100) == 0) {
        ok = false;
        out.reason = Injected;
    }
    out.status = ok ? ResolveStatus::Ok : ResolveStatus::Slow;
    if (ok) {
        out.reason = SlowNone;
    }
    counters.draws.fetch_add(1, std::memory_order_relaxed);
    counters.slow[out.reason].fetch_add(1, std::memory_order_relaxed);
    if (timed) {
        counters.busy_ns.fetch_add(
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - begin)
                                 .count()),
            std::memory_order_relaxed);
    }
}

inline bool Resolver::ResolveOne(const Nx2DrawPipeline::PacketView& packet, ResolvedDraw& out) {
    using Maxwell = Maxwell3D::Regs;
    RasterizerVulkan& r = pipeline.rasterizer;
    const auto& header = packet.header;
    if (current == nullptr || current->tex_channel == nullptr) {
        out.reason = NoChannel;
        return false;
    }
    GraphicsPipeline* const graphics = header.pipeline;
    const GraphicsPipeline::ResolveInfo& info = graphics->GetResolveInfo();
    if (info.has_texture_buffers) {
        out.reason = TextureBuffers;
        return false;
    }
    Maxwell3D& mirror = *current->mirror3d;
    const auto& regs = mirror.regs;
    // Results: views, samplers, then buffers (UBOs, SSBOs, geometry), in u64 words.
    u32 max_buffers = static_cast<u32>(packet.ssbos.size() + packet.geometry.size());
    for (const auto& ubo : packet.ubos) {
        max_buffers += ubo.staged == 0 ? 1 : 0;
    }
    const size_t view_words = size_t{info.num_image_elements} * sizeof(ViewRef) / 8;
    const size_t sampler_words = size_t{info.num_textures} * sizeof(SamplerRef) / 8;
    const size_t buffer_words = size_t{max_buffers} * sizeof(BufferRef) / 8;
    const size_t entry_words = size_t{info.num_image_elements} * sizeof(DescriptorUpdateEntry) / 8;
    const size_t total_words = view_words + sampler_words + buffer_words + entry_words;
    u64 end_position = 0;
    std::span<u64> words;
    if (total_words != 0) {
        words = results.TryAllocate(total_words, end_position);
        if (words.empty()) {
            out.reason = ArenaFull;
            return false;
        }
        out.result_end = end_position;
    }
    auto* const views = reinterpret_cast<ViewRef*>(words.data());
    auto* const samplers = reinterpret_cast<SamplerRef*>(words.data() + view_words);
    auto* const buffers = reinterpret_cast<BufferRef*>(words.data() + view_words + sampler_words);
    auto* const image_entries = reinterpret_cast<DescriptorUpdateEntry*>(
        words.data() + view_words + sampler_words + buffer_words);
    view_objects.assign(info.num_image_elements, nullptr);
    view_rescaled.assign(info.num_image_elements, 0);
    sampler_objects.assign(info.num_textures, nullptr);
    out.image_entries = image_entries;
    out.views = views;
    out.samplers = samplers;
    out.buffers = buffers;

    const VideoCommon::MT::CaptureView capture{
        .overlay = packet.overlay,
        .ubos = packet.ubos,
        .blob = packet.blob,
    };
    VideoCommon::MT::t_capture = &capture;
    SCOPE_EXIT {
        VideoCommon::MT::t_capture = nullptr;
    };
    const OverlayReader reader{&capture};
    TextureCache& texture_cache = r.texture_cache;
    BufferCache& buffer_cache = r.buffer_cache;
    VideoCommon::TextureCacheChannelInfo& channel = *current->tex_channel;
    const bool via_header_index = regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding;
    const u32 tic_limit = regs.tex_header.limit;
    const u32 tsc_limit = via_header_index ? tic_limit : regs.tex_sampler.limit;
    const bool native_tiling = r.gpu_memory->UsesNativeTiling();
    u32 view_cursor = 0;
    u32 sampler_cursor = 0;
    u32 buffer_cursor = 0;

    const auto resolve_view = [&](u32 handle, bool blacklist) -> bool {
        ViewRef& ref = views[view_cursor];
        const u32 bit = view_cursor++;
        ref = ViewRef{};
        ref.blacklist = blacklist ? 1 : 0;
        ref.null = 1;
        const u32 tic_index = Tegra::Texture::TextureHeaderIndex(handle, tic_limit, native_tiling);
        if (tic_index > tic_limit) {
            return true;
        }
        Tegra::Texture::TICEntry entry;
        if (!capture.Read(regs.tex_header.Address() + u64{tic_index} * 32, &entry, sizeof(entry))) {
            out.reason = OverlayMiss;
            return false;
        }
        const bool valid = bit / 64 < packet.tic_valid.size() &&
                           ((packet.tic_valid[bit / 64] >> (bit % 64)) & 1) != 0;
        TextureCache::MtResolvedView resolved;
        switch (texture_cache.TryResolveViewCaptured(channel, entry, valid, blacklist, resolved)) {
        case TextureCache::MtViewResolve::Ok:
            view_objects[bit] = resolved.object;
            view_rescaled[bit] = texture_cache.IsRescalingResolved(resolved) ? 1 : 0;
            ref.view = resolved.view.index;
            ref.view_gen = resolved.view_gen;
            ref.image = resolved.image.index;
            ref.image_gen = resolved.image_gen;
            ref.image_version = resolved.image_version;
            ref.null = 0;
            return true;
        case TextureCache::MtViewResolve::Null:
            return true;
        case TextureCache::MtViewResolve::Miss:
            out.reason = ViewMapMiss;
            return false;
        case TextureCache::MtViewResolve::NotClean:
            out.reason = ViewNotClean;
            return false;
        }
        return false;
    };
    const auto resolve_sampler = [&](u32 tsc_index) -> bool {
        const u32 slot = sampler_cursor++;
        SamplerRef& ref = samplers[slot];
        ref = SamplerRef{VideoCommon::NULL_SAMPLER_ID.index, 0};
        sampler_objects[slot] = texture_cache.MtSamplerObject(VideoCommon::NULL_SAMPLER_ID);
        if (tsc_index > tsc_limit) {
            return true;
        }
        Tegra::Texture::TSCEntry entry;
        if (!capture.Read(regs.tex_sampler.Address() + u64{tsc_index} * 32, &entry, sizeof(entry))) {
            out.reason = OverlayMiss;
            return false;
        }
        VideoCommon::SamplerId id{};
        u32 gen = 0;
        switch (texture_cache.TryResolveSamplerCaptured(channel, entry, id, gen)) {
        case TextureCache::MtSamplerResolve::Ok:
            break;
        case TextureCache::MtSamplerResolve::Null:
            return true;
        case TextureCache::MtSamplerResolve::Miss:
            // Samplers are never erased: create it here rather than fall back.
            id = texture_cache.CreateSamplerFromResolver(channel, entry);
            ++out.sampler_creations;
            counters.sampler_creations.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        ref = SamplerRef{id.index, gen};
        sampler_objects[slot] = texture_cache.MtSamplerObject(id);
        return true;
    };
    const auto resolve_buffer = [&](u8 kind, u8 stage, u8 slot, u8 written, DAddr device_addr,
                                    u32 size, u32 lookup_size) -> bool {
        BufferRef& ref = buffers[buffer_cursor++];
        ref = BufferRef{};
        ref.kind = kind;
        ref.stage = stage;
        ref.slot = slot;
        ref.written = written;
        ref.size = size;
        ref.device_addr = device_addr;
        BufferCache::MtResolvedRange range;
        switch (buffer_cache.TryResolveRange(device_addr, lookup_size, false, range)) {
        case BufferCache::MtRangeResolve::Ok:
        case BufferCache::MtRangeResolve::Null:
            ref.buffer = range.id.index;
            ref.gen = range.gen;
            ref.offset = range.offset;
            return true;
        case BufferCache::MtRangeResolve::Miss:
            out.reason = BufferPageMiss;
            return false;
        }
        return false;
    };

    // Geometry first, as UpdateGraphicsBuffers does.
    for (const auto& geometry : packet.geometry) {
        if (geometry.kind == VideoCommon::MT::GeometryEntry::InlineIndex) {
            continue;
        }
        const u8 kind = geometry.kind == VideoCommon::MT::GeometryEntry::Index ? KindIndex : KindVertex;
        if (!resolve_buffer(kind, 0, geometry.stream, 0, geometry.device_addr, geometry.size,
                            geometry.size)) {
            return false;
        }
    }
    const auto& stage_infos = *info.stage_infos;
    size_t ssbo_cursor = 0;
    size_t ubo_cursor = 0;
    for (size_t stage = 0; stage < stage_infos.size(); ++stage) {
        if (!info.spec.enabled_stages[stage]) {
            continue;
        }
        const Shader::Info& stage_info = stage_infos[stage];
        const auto& cbufs = mirror.state.shader_stages[stage].const_buffers;
        // Cached uniform buffers (streamed ones are bound from the packet's bytes).
        for (; ubo_cursor < packet.ubos.size() && packet.ubos[ubo_cursor].stage == stage;
             ++ubo_cursor) {
            const auto& ubo = packet.ubos[ubo_cursor];
            if (ubo.staged != 0) {
                continue;
            }
            // FindBuffer looks up the whole bound size (UpdateUniformBuffers).
            const u32 bound_size = cbufs[ubo.index].size;
            if (!resolve_buffer(KindUbo, static_cast<u8>(stage), ubo.index, 0, ubo.device_addr,
                                ubo.size, (std::max)(bound_size, ubo.size))) {
                return false;
            }
        }
        for (; ssbo_cursor < packet.ssbos.size() && packet.ssbos[ssbo_cursor].stage == stage;
             ++ssbo_cursor) {
            const auto& ssbo = packet.ssbos[ssbo_cursor];
            const DAddr device_addr = ssbo.skipped != 0 ? 0 : ssbo.device_addr;
            if (!resolve_buffer(KindSsbo, static_cast<u8>(stage), ssbo.index, ssbo.is_written,
                                device_addr, ssbo.size, ssbo.size)) {
                return false;
            }
        }
        const auto read_handle = [&](const auto& desc, u32 array_index) -> std::pair<u32, u32> {
            if (desc.handle_program != ~0U) {
                return Tegra::Texture::TexturePair(
                    VideoCommon::EvaluateTextureHandle(stage_info, desc.handle_program, reader,
                                                       [&](u32 cbuf) -> GPUVAddr {
                                                           return cbuf < cbufs.size() &&
                                                                          cbufs[cbuf].enabled
                                                                      ? cbufs[cbuf].address
                                                                      : GPUVAddr{};
                                                       }),
                    via_header_index);
            }
            const u32 index_offset{array_index << desc.size_shift};
            const u32 offset{desc.cbuf_offset + index_offset};
            const GPUVAddr addr{cbufs[desc.cbuf_index].address + offset};
            if constexpr (requires { desc.has_secondary; }) {
                if (desc.has_secondary) {
                    const u32 second_offset{desc.secondary_cbuf_offset + index_offset};
                    const GPUVAddr separate_addr{cbufs[desc.secondary_cbuf_index].address +
                                                 second_offset};
                    const u32 lhs_raw{reader.Read<u32>(addr) << desc.shift_left};
                    const u32 rhs_raw{reader.Read<u32>(separate_addr) << desc.secondary_shift_left};
                    return Tegra::Texture::TexturePair(lhs_raw | rhs_raw, via_header_index);
                }
            }
            return Tegra::Texture::TexturePair(reader.Read<u32>(addr), via_header_index);
        };
        for (const auto& desc : stage_info.texture_descriptors) {
            for (u32 i = 0; i < desc.count; ++i) {
                const auto handle = read_handle(desc, i);
                if (reader.miss || capture.overlay_misses != 0) {
                    out.reason = OverlayMiss;
                    return false;
                }
                if (!resolve_view(handle.first, false) || !resolve_sampler(handle.second)) {
                    return false;
                }
            }
        }
        if (info.spec.has_images) {
            for (const auto& desc : stage_info.image_descriptors) {
                for (u32 i = 0; i < desc.count; ++i) {
                    const auto handle = read_handle(desc, i);
                    if (reader.miss || capture.overlay_misses != 0) {
                        out.reason = OverlayMiss;
                        return false;
                    }
                    const u32 slot = view_cursor;
                    if (!resolve_view(handle.first, desc.is_written)) {
                        return false;
                    }
                    views[slot].written = desc.is_written ? 1 : 0;
                }
            }
        }
    }
    out.num_views = view_cursor;
    out.num_samplers = sampler_cursor;
    out.num_buffers = buffer_cursor;

    // PushImageDescriptors: the same entries in the same order, from the resolved objects.
    const ImageView& null_view = texture_cache.NullImageViewObject();
    RescalingPushConstant rescaling;
    u32 entry_cursor = 0;
    u32 view_index = 0;
    u32 sampler_index = 0;
    for (size_t stage = 0; stage < stage_infos.size(); ++stage) {
        if (!info.spec.enabled_stages[stage]) {
            continue;
        }
        const Shader::Info& stage_info = stage_infos[stage];
        out.stage_begin[stage] = entry_cursor;
        for (const auto& desc : stage_info.texture_descriptors) {
            bool is_rescaled = false;
            for (u32 i = 0; i < desc.count; ++i, ++view_index, ++sampler_index) {
                const ImageView& view =
                    view_objects[view_index] != nullptr ? *view_objects[view_index] : null_view;
                VkImageView vk_view = view.Handle(desc.type);
                if (vk_view == VK_NULL_HANDLE) {
                    if (const VkImageView null_handle = null_view.Handle(desc.type);
                        null_handle != VK_NULL_HANDLE) {
                        vk_view = null_handle;
                    }
                }
                Sampler& sampler = *sampler_objects[sampler_index];
                VkSampler vk_sampler = sampler.TryHandleFor(view, desc.is_depth);
                if (vk_sampler == VK_NULL_HANDLE) {
                    vk_sampler = sampler.HandleFor(view, desc.is_depth);
                }
                image_entries[entry_cursor++] = VkDescriptorImageInfo{
                    .sampler = vk_sampler,
                    .imageView = vk_view,
                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                };
                is_rescaled |= view_rescaled[view_index] != 0;
            }
            rescaling.PushTexture(is_rescaled);
        }
        if (info.spec.has_images) {
            for (const auto& desc : stage_info.image_descriptors) {
                bool is_rescaled = false;
                for (u32 i = 0; i < desc.count; ++i, ++view_index) {
                    const ImageView& view =
                        view_objects[view_index] != nullptr ? *view_objects[view_index] : null_view;
                    const VkImageView vk_view = view.TryStorageView(desc.type, desc.format);
                    if (vk_view == VK_NULL_HANDLE && view.ImageHandle() != VK_NULL_HANDLE) {
                        // Created on first use: the committer's path makes it.
                        out.reason = ViewLazyHandle;
                        return false;
                    }
                    image_entries[entry_cursor++] = VkDescriptorImageInfo{
                        .sampler = VK_NULL_HANDLE,
                        .imageView = vk_view,
                        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                    };
                    is_rescaled |= view_rescaled[view_index] != 0;
                }
                rescaling.PushImage(is_rescaled);
            }
        }
        out.stage_count[stage] = entry_cursor - out.stage_begin[stage];
    }
    out.rescaling = rescaling.Data();
    return true;
}

} // namespace Vulkan::MT

// ---- Nx2DrawPipeline's resolver side (needs Resolver complete) ----

namespace Vulkan {

inline Nx2DrawPipeline::~Nx2DrawPipeline() {
    Stop();
}

inline void Nx2DrawPipeline::StartResolvers() {
    const u32 count = (std::min)(VideoCommon::MT::Resolvers(), 6u);
    if (count == 0) {
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        resolvers.push_back(std::make_unique<MT::Resolver>(*this, i, count));
        // The sequencer has not emitted anything yet: the resolvers start at position 0 too.
        resolvers.back()->AddConsumer();
    }
    for (auto& resolver : resolvers) {
        resolver->Start();
    }
    resolver_count = count;
}

inline void Nx2DrawPipeline::StopResolvers() {
    for (auto& resolver : resolvers) {
        resolver->Stop();
    }
    resolvers.clear();
    resolver_count = 0;
}

inline MT::ResolvedDraw* Nx2DrawPipeline::WaitResolved(const DrawHeader& header) {
    if (resolver_count == 0) {
        return nullptr;
    }
    MT::Resolver& resolver = *resolvers[header.batch % resolver_count];
    if (!resolver.cursor.Done(header.batch)) {
        const bool timed = VideoCommon::MT::Stats();
        const auto begin = std::chrono::steady_clock::now();
        bool warned = false;
        resolver.cursor.Wait(
            header.batch,
            [&] {
                if (!warned && std::chrono::steady_clock::now() - begin > std::chrono::seconds{2}) {
                    warned = true;
                    LOG_WARNING(Render_Vulkan,
                                "MT: committer waited 2 s for batch {} (draw {}) of resolver {}",
                                header.batch, header.seq, header.batch % resolver_count);
                }
            },
            [] { return false; });
        if (timed) {
            counters.commit_wait_ns.fetch_add(
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count()),
                std::memory_order_relaxed);
        }
    }
    MT::ResolvedDraw& slot = resolved.Slot(header.seq);
    if (slot.seq != header.seq) {
        counters.resolved_missing.fetch_add(1, std::memory_order_relaxed);
        if (VideoCommon::MT::ResolverChecks() != 0) {
            VerifyReport(0x5200, fmt::format("MT rcheck: resolved slot of draw {} holds draw {}",
                                             header.seq, slot.seq));
        }
        return nullptr;
    }
    return &slot;
}

inline MT::ValidateFail Nx2DrawPipeline::Validate(const MT::ResolvedDraw& res,
                                                  const DrawHeader& header) const {
    RasterizerVulkan& r = rasterizer;
    if (res.channel_gen != channel_gen.load(std::memory_order_relaxed)) {
        return MT::FailChannel;
    }
    if (res.pipeline != header.pipeline) {
        return MT::FailPipeline;
    }
    for (u32 i = 0; i < res.num_views; ++i) {
        const MT::ViewRef& ref = res.views[i];
        if (ref.null != 0) {
            continue;
        }
        const TextureCache::MtResolvedView view{
            .view = VideoCommon::ImageViewId{ref.view},
            .view_gen = ref.view_gen,
            .image = VideoCommon::ImageId{ref.image},
            .image_gen = ref.image_gen,
            .image_version = ref.image_version,
            .object = &r.texture_cache.NullImageViewObject(),
        };
        if (!r.texture_cache.MtValidateView(view)) {
            return MT::FailView;
        }
    }
    if (r.buffer_cache.MtBufferEpoch() != res.buffer_epoch) {
        for (u32 i = 0; i < res.num_buffers; ++i) {
            const MT::BufferRef& ref = res.buffers[i];
            if (!r.buffer_cache.MtValidateBuffer(VideoCommon::BufferId{ref.buffer}, ref.gen)) {
                return MT::FailBuffer;
            }
        }
    }
    return MT::FailNone;
}

inline void Nx2DrawPipeline::PrepareResolvedTextures(const MT::ResolvedDraw& res,
                                                     GraphicsPipeline::MtResolvedTextures& out) {
    TextureCache& texture_cache = rasterizer.texture_cache;
    commit_views.clear();
    for (u32 i = 0; i < res.num_views; ++i) {
        const MT::ViewRef& ref = res.views[i];
        VideoCommon::ImageViewInOut view{};
        view.blacklist = ref.blacklist != 0;
        view.id = ref.null != 0 ? VideoCommon::NULL_IMAGE_VIEW_ID : VideoCommon::ImageViewId{ref.view};
        commit_views.push_back(view);
        if (ref.null == 0) {
            const TextureCache::MtResolvedView resolved{
                .view = view.id,
                .view_gen = ref.view_gen,
                .image = VideoCommon::ImageId{ref.image},
                .image_gen = ref.image_gen,
                .image_version = ref.image_version,
                .object = &texture_cache.NullImageViewObject(),
            };
            texture_cache.MtApplyViewIntent(resolved, ref.written != 0, ref.blacklist != 0);
        }
    }
    commit_samplers.clear();
    for (u32 i = 0; i < res.num_samplers; ++i) {
        commit_samplers.push_back(VideoCommon::SamplerId{res.samplers[i].sampler});
    }
    out.views = std::span<const VideoCommon::ImageViewInOut>(commit_views.data(), commit_views.size());
    out.samplers = std::span<const VideoCommon::SamplerId>(commit_samplers.data(), commit_samplers.size());
    out.entries = res.image_entries;
    out.stage_begin = res.stage_begin;
    out.stage_count = res.stage_count;
    out.rescaling = res.rescaling;
}

inline void Nx2DrawPipeline::ShadowCompare(const MT::ResolvedDraw& res, const DrawHeader& header,
                                           const GraphicsPipeline::MtShadowSink& sink) {
    counters.shadow_checks.fetch_add(1, std::memory_order_relaxed);
    RasterizerVulkan& r = rasterizer;
    const auto mismatch = [&](u32 field, const std::string& what) {
        counters.shadow_mismatches.fetch_add(1, std::memory_order_relaxed);
        VerifyReport(0x5000 | field, fmt::format("MT shadow: draw {} {}", header.seq, what));
    };
    if (sink.views.size() != res.num_views) {
        mismatch(1, fmt::format("views: resolver {} committer {}", res.num_views, sink.views.size()));
        return;
    }
    for (u32 i = 0; i < res.num_views; ++i) {
        const u32 resolved_id = res.views[i].null != 0 ? VideoCommon::NULL_IMAGE_VIEW_ID.index
                                                        : res.views[i].view;
        if (sink.views[i].index != resolved_id) {
            mismatch(2, fmt::format("view {}: resolver {} committer {}", i, resolved_id,
                                    sink.views[i].index));
            return;
        }
    }
    if (sink.samplers.size() != res.num_samplers) {
        mismatch(3, fmt::format("samplers: resolver {} committer {}", res.num_samplers,
                                sink.samplers.size()));
        return;
    }
    for (u32 i = 0; i < res.num_samplers; ++i) {
        if (sink.samplers[i].index != res.samplers[i].sampler) {
            mismatch(4, fmt::format("sampler {}: resolver {} committer {}", i,
                                    res.samplers[i].sampler, sink.samplers[i].index));
            return;
        }
    }
    if (sink.rescaling != res.rescaling) {
        mismatch(9, "rescaling words");
        return;
    }
    if (sink.images_recorded) {
        for (size_t stage = 0; stage < res.stage_count.size(); ++stage) {
            if (sink.stage_image_count[stage] != res.stage_count[stage]) {
                mismatch(10, fmt::format("stage {} image entries: resolver {} committer {}", stage,
                                         res.stage_count[stage], sink.stage_image_count[stage]));
                return;
            }
            for (u32 i = 0; i < res.stage_count[stage]; ++i) {
                const VkDescriptorImageInfo& a = res.image_entries[res.stage_begin[stage] + i].image;
                const VkDescriptorImageInfo& b = sink.payload[sink.stage_image_begin[stage] + i].image;
                if (a.sampler != b.sampler || a.imageView != b.imageView ||
                    a.imageLayout != b.imageLayout) {
                    mismatch(11, fmt::format("stage {} image entry {}: resolver {}/{} committer {}/{}",
                                             stage, i, fmt::ptr(a.imageView), fmt::ptr(a.sampler),
                                             fmt::ptr(b.imageView), fmt::ptr(b.sampler)));
                    return;
                }
            }
        }
    }
    for (u32 i = 0; i < res.num_buffers; ++i) {
        const MT::BufferRef& ref = res.buffers[i];
        if (ref.kind == MT::KindIndex && header.draw_state.draw_indexed == false) {
            continue;
        }
        const VideoCommon::BufferId bound =
            r.buffer_cache.MtBindingBufferId(ref.kind, ref.stage, ref.slot);
        if (bound.index != ref.buffer) {
            mismatch(5 + ref.kind,
                     fmt::format("buffer kind {} stage {} slot {} addr {:#x}: resolver {} "
                                 "committer {}",
                                 ref.kind, ref.stage, ref.slot, ref.device_addr, ref.buffer,
                                 bound.index));
            return;
        }
    }
}

} // namespace Vulkan

namespace Vulkan {

inline void Nx2DrawPipeline::LogResolverStats(double seconds) {
    const auto take = [](std::atomic<u64>& value) {
        return value.exchange(0, std::memory_order_relaxed);
    };
    const auto rate = [seconds](u64 value) { return static_cast<double>(value) / seconds; };
    if (resolver_count == 0) {
        return;
    }
    std::string per_resolver;
    std::array<u64, MT::SlowReasonCount> slow{};
    for (auto& resolver : resolvers) {
        MT::ResolverCounters& c = resolver->counters;
        const u64 draws = take(c.draws);
        const u64 batches = take(c.batches);
        per_resolver += fmt::format(" [{:.0f} draws/s, {:.1f} per batch, busy {:.0f}%, wait {:.0f}%, "
                                    "{} sampler creations]",
                                    rate(draws),
                                    batches != 0 ? static_cast<double>(draws) / batches : 0.0,
                                    static_cast<double>(take(c.busy_ns)) / 1e7 / seconds,
                                    static_cast<double>(take(c.wait_ns)) / 1e7 / seconds,
                                    take(c.sampler_creations));
        for (size_t i = 0; i < slow.size(); ++i) {
            slow[i] += take(c.slow[i]);
        }
    }
    std::string slow_text;
    for (size_t i = 1; i < slow.size(); ++i) {
        if (slow[i] != 0) {
            slow_text += fmt::format(" {} {:.0f}", MT::SlowReasonNames[i], rate(slow[i]));
        }
    }
    std::string fail_text;
    for (size_t i = 1; i < counters.validate_fail.size(); ++i) {
        if (const u64 value = take(counters.validate_fail[i]); value != 0) {
            fail_text += fmt::format(" {} {:.0f}", MT::ValidateFailNames[i], rate(value));
        }
    }
    take(counters.validate_fail[0]);
    LOG_INFO(Render_Vulkan,
             "MT resolvers/s:{} | ok {:.0f} (committed {:.0f}), slow {:.0f}:{}, missing {:.0f} | validation "
             "failures:{} | batches {:.0f}, commit wait {:.1f} ms, ring waits {:.0f}, barriers "
             "{:.0f} | shadow {:.0f} checks, {} mismatches, {:.0f} unvalidated | EBR pending {}",
             per_resolver, rate(take(counters.resolved_ok)), rate(take(counters.resolved_committed)),
             rate(take(counters.resolved_slow)),
             slow_text, rate(take(counters.resolved_missing)), fail_text,
             rate(take(counters.batches)), static_cast<double>(take(counters.commit_wait_ns)) / 1e6 / seconds,
             rate(take(counters.ring_waits)), rate(take(counters.barriers)),
             rate(take(counters.shadow_checks)),
             counters.shadow_mismatches.load(std::memory_order_relaxed),
             rate(take(counters.shadow_unvalidated)), epochs.PendingCount());
}

} // namespace Vulkan
