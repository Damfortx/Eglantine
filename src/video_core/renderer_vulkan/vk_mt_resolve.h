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
#include "video_core/mt/uniform_ring.h"
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

    /// The resolver's stream cursor (watchdog).
    [[nodiscard]] u32 Consumer() const noexcept {
        return consumer;
    }
    [[nodiscard]] bool Running() const noexcept {
        return has_consumer;
    }

    VideoCommon::MT::BatchCursor cursor;
    VideoCommon::MT::RingArena<u64> results{size_t{1} << 20}; // 8 MiB
    ResolverCounters counters;

    /// EDEN_NX2_MT_RBUF: the resolver's streamed uniform ring (host-visible, created by Start on
    /// the thread that starts the pipeline: the memory allocator is not thread-safe).
    struct UniformRing {
        vk::Buffer buffer;
        VkBuffer handle{};
        VkDeviceAddress address{};
        std::span<u8> mapped;
        VideoCommon::MT::UniformRingCore core;
        VideoCommon::MT::UniformReuseMemo memo;
        std::vector<u8> scratch;
    };
    std::unique_ptr<UniformRing> ring;
    /// Committer: the batch's draws are recorded into command buffers up to `tick`.
    void StampBatch(u64 batch, u64 tick) noexcept {
        if (ring) {
            ring->core.Stamp(batch / count, tick);
        }
    }
    static constexpr size_t RingBytes = size_t{32} << 20;

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
    /// EDEN_NX2_MT_COMPACT: this resolver's TIC/TSC replica (every draw packet updates it, the
    /// ones of other batches too) and the decoded overlay of the packet it resolves.
    VideoCommon::MT::PacketDecoder decoder;
    std::vector<const ImageView*> view_objects;
    std::vector<u8> view_rescaled;
    std::vector<Sampler*> sampler_objects;
    std::vector<const Buffer*> buffer_objects;
    void CreateRing();
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

inline void Resolver::CreateRing() {
    RasterizerVulkan& r = pipeline.rasterizer;
    if (!VideoCommon::MT::ResolverBufferHalf() || ring) {
        return;
    }
    try {
        auto created = std::make_unique<UniformRing>();
        VkBufferCreateInfo ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = RingBytes,
            .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        if (r.device.IsBufferDeviceAddressSupported()) {
            ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        }
        // Host memory (MemoryUsage::Upload): VRAM is the constraint at 2x.
        created->buffer = r.memory_allocator.CreateBuffer(ci, MemoryUsage::Upload);
        created->handle = *created->buffer;
        created->mapped = created->buffer.Mapped();
        if (created->mapped.size() < RingBytes) {
            LOG_WARNING(Render_Vulkan, "MT resolver {}: uniform ring is not host visible", index);
            return;
        }
        if (r.device.IsBufferDeviceAddressSupported()) {
            created->address = r.device.GetLogical().GetBufferDeviceAddress(created->handle);
        }
        created->core.Reset(RingBytes, r.buffer_cache.MtUniformBufferAlignment());
        ring = std::move(created);
    } catch (const std::exception& e) {
        LOG_WARNING(Render_Vulkan, "MT resolver {}: no uniform ring ({})", index, e.what());
    }
}

inline void Resolver::Start() {
    CreateRing();
    if (ring && (VideoCommon::MT::Compact() || VideoCommon::MT::UboGen())) {
        // Streamed uniforms may come as the committer's kept bytes (staged 2/3): keep a replica.
        decoder.EnableUniforms();
    }
    ready.store(false, std::memory_order_relaxed);
    thread = std::jthread([this](std::stop_token stop) {
        VideoCommon::MT::RunLoudThread("GPUResolve", stop, [&] { Loop(stop); });
    });
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
        const u64 batch = Nx2DrawPipeline::PacketBatch(entry);
        if (batch % count == index) {
            ResolveDraw(entry);
        } else {
            Nx2DrawPipeline::SkipPacket(entry, decoder);
        }
        break;
    }
    case StreamEntry::BatchEnd: {
        const u64 batch = entry[1];
        if (batch % count == index) {
            LeaveBatch();
            if (ring) {
                ring->core.EndBatch(batch / count);
            }
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
        VideoCommon::MT::WaitWatch::Scope watch_scope{
            pipeline.watch,
            static_cast<VideoCommon::MT::WaitSite>(
                static_cast<u32>(VideoCommon::MT::WaitSite::ResolverBarrier0) + (index & 7)),
            reinterpret_cast<u64>(done)};
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
    const Nx2DrawPipeline::PacketView packet = Nx2DrawPipeline::ParsePacket(entry, decoder);
    // Before anything reads the packet's streamed uniforms (staged 2/3 read the kept bytes).
    decoder.ApplyUniforms(packet.ubos, packet.blob);
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
    out.buffer_fail = BufOff;
    if (ring) {
        ring->core.BeginBatch(packet.header.batch / count);
    }
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
    if (ok) {
        counters.buffer_fail[out.buffer_fail].fetch_add(1, std::memory_order_relaxed);
    }
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
    const auto& stage_infos = *info.stage_infos;

    // The payload layout, per stage: uniform buffers, storage buffers, then images (texture
    // buffers never reach here). The image entries keep this layout without EDEN_NX2_MT_RBUF.
    bool rbuf = VideoCommon::MT::ResolverBufferHalf();
    u8 buffer_fail = rbuf ? BufNone : BufOff;
    const auto fail_buffers = [&](u8 why) {
        if (buffer_fail == BufNone) {
            buffer_fail = why;
        }
    };
    std::array<u32, Maxwell::MaxShaderStage> stage_base{};
    std::array<u32, Maxwell::MaxShaderStage> stage_buffers{};
    u32 payload_entries = 0;
    for (size_t stage = 0; stage < stage_infos.size(); ++stage) {
        if (!info.spec.enabled_stages[stage]) {
            continue;
        }
        const Shader::Info& stage_info = stage_infos[stage];
        const u32 ubos = static_cast<u32>(std::popcount(info.enabled_uniform_buffer_masks[stage]));
        const u32 ssbos = static_cast<u32>(stage_info.storage_buffers_descriptors.size());
        u32 images = Shader::NumDescriptors(stage_info.texture_descriptors);
        if (info.spec.has_images) {
            images += Shader::NumDescriptors(stage_info.image_descriptors);
        }
        stage_base[stage] = payload_entries;
        stage_buffers[stage] = rbuf ? ubos + ssbos : 0;
        payload_entries += stage_buffers[stage] + images;
    }
    if (rbuf) {
        if (header.indirect != 0) {
            fail_buffers(BufIndirect);
        } else if (header.inline_index_size != 0) {
            fail_buffers(BufInlineIndex);
        } else if (Settings::IsGPUFenceBehaviorAccurate()) {
            fail_buffers(BufAccurateFences);
        } else if (r.buffer_cache_runtime.ShouldLimitDynamicStorageBuffers()) {
            fail_buffers(BufStorageLimit);
        } else if (payload_entries != info.num_descriptor_entries) {
            fail_buffers(BufLayout);
        } else if (!ring) {
            fail_buffers(BufNoRing);
        }
    }
    // Results: views, samplers, then buffers (UBOs, SSBOs, geometry), in u64 words.
    u32 max_buffers = static_cast<u32>(packet.ssbos.size() + packet.geometry.size() +
                                       packet.ubos.size() + Maxwell::MaxShaderStage);
    const size_t view_words = size_t{info.num_image_elements} * sizeof(ViewRef) / 8;
    const size_t sampler_words = size_t{info.num_textures} * sizeof(SamplerRef) / 8;
    const size_t buffer_words = size_t{max_buffers} * sizeof(BufferRef) / 8;
    const size_t entry_words = size_t{payload_entries} * sizeof(DescriptorUpdateEntry) / 8;
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
    auto* const payload = reinterpret_cast<DescriptorUpdateEntry*>(
        words.data() + view_words + sampler_words + buffer_words);
    view_objects.assign(info.num_image_elements, nullptr);
    view_rescaled.assign(info.num_image_elements, 0);
    sampler_objects.assign(info.num_textures, nullptr);
    buffer_objects.assign(max_buffers, nullptr);
    out.image_entries = payload;
    out.payload = payload;
    out.num_payload = payload_entries;
    out.views = views;
    out.samplers = samplers;
    out.buffers = buffers;

    const VideoCommon::MT::CaptureView capture{
        .overlay = packet.overlay,
        .ubos = packet.ubos,
        .blob = packet.blob,
        .overlay_blob = packet.overlay_blob,
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
    const bool allow_refresh = VideoCommon::MT::ResolverRefresh();
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
        switch (texture_cache.TryResolveViewCaptured(channel, entry, valid, blacklist, resolved,
                                                     allow_refresh)) {
        case TextureCache::MtViewResolve::Ok:
            view_objects[bit] = resolved.object;
            view_rescaled[bit] = texture_cache.IsRescalingResolved(resolved) ? 1 : 0;
            ref.view = resolved.view.index;
            ref.view_gen = resolved.view_gen;
            ref.image = resolved.image.index;
            ref.image_gen = resolved.image_gen;
            ref.image_version = resolved.image_version;
            ref.null = 0;
            ref.refresh = resolved.refresh ? 1 : 0;
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
        const u32 ref_index = buffer_cursor++;
        BufferRef& ref = buffers[ref_index];
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
            buffer_objects[ref_index] = range.buffer;
            return true;
        case BufferCache::MtRangeResolve::Miss:
            out.reason = BufferPageMiss;
            return false;
        }
        return false;
    };

    // EDEN_NX2_MT_RBUF: buffer descriptor entries, as UpdateDescriptorQueue::AddBuffer writes them.
    struct PayloadSink {
        DescriptorUpdateEntry* cursor;
        bool use_descriptor_buffer;
        void AddBuffer(VkBuffer buffer, VkDeviceAddress base_address, VkDeviceSize offset,
                       VkDeviceSize size) {
            if (!use_descriptor_buffer) {
                *cursor++ = VkDescriptorBufferInfo{
                    .buffer = buffer,
                    .offset = offset,
                    .range = size,
                };
                return;
            }
            *cursor++ = DescriptorAddress{
                .address = base_address == 0 ? 0 : base_address + offset,
                .range = base_address == 0 ? VK_WHOLE_SIZE : size,
                .format = VK_FORMAT_UNDEFINED,
            };
        }
    };
    const bool use_descriptor_buffer =
        r.guest_descriptor_queue.SupportsDescriptorBuffer() && info.uses_descriptor_buffer;
    const u64 ring_local = header.batch / count;
    // A streamed uniform buffer's bytes into the ring (or the slot's previous allocation).
    const auto stream_uniform = [&](PayloadSink& sink, u32 stage, u32 index, const u8* data,
                                    u32 size, u64 source) -> bool {
        UniformRing& rg = *ring;
        std::optional<u64> offset = rg.memo.Find(stage, index, ring_local, source, data, size);
        if (offset) {
            counters.ring_reuses.fetch_add(1, std::memory_order_relaxed);
        } else {
            rg.core.BeginBatch(ring_local);
            offset = rg.core.Allocate(size, [&](u64 tick) { return r.scheduler.IsFree(tick); });
            if (!offset) {
                return false;
            }
            std::memcpy(rg.mapped.data() + *offset, data, size);
            rg.memo.Store(stage, index, ring_local, source, *offset, data, size);
            counters.ring_bytes.fetch_add(size, std::memory_order_relaxed);
        }
        counters.ring_streams.fetch_add(1, std::memory_order_relaxed);
        sink.AddBuffer(rg.handle, rg.address, *offset, size);
        BufferRef& ref = buffers[buffer_cursor++];
        ref = BufferRef{};
        ref.kind = KindUboStream;
        ref.stage = static_cast<u8>(stage);
        ref.slot = static_cast<u8>(index);
        ref.buffer = VideoCommon::NULL_BUFFER_ID.index;
        ref.offset = static_cast<u32>(*offset);
        ref.size = size;
        return true;
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
    const auto& uniform_sizes = *info.uniform_buffer_sizes;
    size_t ssbo_cursor = 0;
    size_t ubo_cursor = 0;
    for (size_t stage = 0; stage < stage_infos.size(); ++stage) {
        if (!info.spec.enabled_stages[stage]) {
            continue;
        }
        const Shader::Info& stage_info = stage_infos[stage];
        const auto& cbufs = mirror.state.shader_stages[stage].const_buffers;
        PayloadSink sink{payload + stage_base[stage], use_descriptor_buffer};
        // Uniform buffers in BindHostGraphicsUniformBuffers' order: cached ones from the cache,
        // streamed ones (RBUF) into the ring, bank 17 from the root table when unbound.
        u32 mask = info.enabled_uniform_buffer_masks[stage];
        while (mask != 0) {
            const u32 index = static_cast<u32>(std::countr_zero(mask));
            mask &= mask - 1;
            const VideoCommon::MT::UboEntry* ubo = nullptr;
            if (ubo_cursor < packet.ubos.size() && packet.ubos[ubo_cursor].stage == stage &&
                packet.ubos[ubo_cursor].index == index) {
                ubo = &packet.ubos[ubo_cursor++];
            }
            if (ubo == nullptr) {
                if (buffer_fail != BufNone) {
                    continue;
                }
                if (index != 17 || !mirror.root_tables.loaded) {
                    fail_buffers(BufMissingUniform);
                    continue;
                }
                // Root table 0 streamed as bank 17 (BindHostGraphicsUniformBuffer).
                const auto& table = mirror.root_tables.tables[0];
                const u32 size = uniform_sizes[stage][index];
                const u32 copied = (std::min)(size, static_cast<u32>(sizeof(table)));
                ring->scratch.assign(size, 0);
                std::memcpy(ring->scratch.data(), table.data(), copied);
                if (!stream_uniform(sink, static_cast<u32>(stage), index, ring->scratch.data(), size,
                                    0)) {
                    fail_buffers(BufRingFull);
                }
                continue;
            }
            if (ubo->staged == 0) {
                // FindBuffer looks up the whole bound size (UpdateUniformBuffers).
                const u32 bound_size = cbufs[ubo->index].size;
                const u32 ref_index = buffer_cursor;
                if (!resolve_buffer(KindUbo, static_cast<u8>(stage), ubo->index, 0,
                                    ubo->device_addr, ubo->size,
                                    (std::max)(bound_size, ubo->size))) {
                    return false;
                }
                if (buffer_fail == BufNone) {
                    BufferCacheRuntime::MakeBufferDescriptor(sink, *buffer_objects[ref_index],
                                                             buffers[ref_index].offset, ubo->size);
                }
                continue;
            }
            if (buffer_fail != BufNone) {
                continue;
            }
            u64 source = 0;
            const u8* const bytes = decoder.UniformBytes(*ubo, packet.blob, source);
            if (bytes == nullptr) {
                fail_buffers(BufUniformMemo); // the committer's per-slot copy, no replica here
                continue;
            }
            if (!stream_uniform(sink, static_cast<u32>(stage), index, bytes, ubo->size,
                                source)) {
                fail_buffers(BufRingFull);
            }
        }
        u32 stage_ssbos = 0;
        for (; ssbo_cursor < packet.ssbos.size() && packet.ssbos[ssbo_cursor].stage == stage;
             ++ssbo_cursor) {
            const auto& ssbo = packet.ssbos[ssbo_cursor];
            const DAddr device_addr = ssbo.skipped != 0 ? 0 : ssbo.device_addr;
            const u32 ref_index = buffer_cursor;
            if (!resolve_buffer(KindSsbo, static_cast<u8>(stage), ssbo.index, ssbo.is_written,
                                device_addr, ssbo.size, ssbo.size)) {
                return false;
            }
            ++stage_ssbos;
            if (ssbo.skipped != 0 && ssbo.device_addr != 0) {
                // FindBuffer(device_addr, 0) may find a buffer there: the committer's.
                fail_buffers(BufNullStorage);
            }
            if (buffer_fail == BufNone) {
                BufferCacheRuntime::MakeBufferDescriptor(sink, *buffer_objects[ref_index],
                                                         buffers[ref_index].offset, ssbo.size);
            }
        }
        if (buffer_fail == BufNone &&
            sink.cursor != payload + stage_base[stage] + stage_buffers[stage]) {
            fail_buffers(BufLayout);
        }
        (void)stage_ssbos;
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
    u32 view_index = 0;
    u32 sampler_index = 0;
    for (size_t stage = 0; stage < stage_infos.size(); ++stage) {
        if (!info.spec.enabled_stages[stage]) {
            continue;
        }
        const Shader::Info& stage_info = stage_infos[stage];
        u32 entry_cursor = stage_base[stage] + stage_buffers[stage];
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
                payload[entry_cursor++] = VkDescriptorImageInfo{
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
                    payload[entry_cursor++] = VkDescriptorImageInfo{
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
    out.buffer_fail = buffer_fail;
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
        if (adaptive_batches) {
            wanted_batch.store(header.batch + 1, std::memory_order_relaxed);
        }
        const bool timed = VideoCommon::MT::Stats();
        const auto begin = std::chrono::steady_clock::now();
        if (steal_us != 0) {
            // Record this draw ourselves rather than wait longer for the resolver.
            const auto deadline = begin + std::chrono::microseconds{steal_us};
            for (u32 spins = 0; !resolver.cursor.Done(header.batch); ++spins) {
                if ((spins & 63) == 63 && std::chrono::steady_clock::now() >= deadline) {
                    counters.resolved_stolen.fetch_add(1, std::memory_order_relaxed);
                    if (timed) {
                        counters.commit_wait_ns.fetch_add(
                            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now() - begin)
                                                 .count()),
                            std::memory_order_relaxed);
                    }
                    return nullptr;
                }
                VideoCommon::MT::CpuPause();
            }
        }
        bool warned = false;
        VideoCommon::MT::WaitWatch::Scope watch_scope{watch, VideoCommon::MT::WaitSite::CommitResolved,
                                                      header.batch};
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
            texture_cache.MtApplyViewIntent(resolved, ref.written != 0, ref.blacklist != 0,
                                            ref.refresh != 0);
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

inline void Nx2DrawPipeline::ApplyResolvedBuffers(const MT::ResolvedDraw& res, bool is_indexed) {
    const u32 mismatches = rasterizer.buffer_cache.MtCommitResolvedBuffers(
        std::span<const MT::BufferRef>(res.buffers, res.num_buffers),
        res.pipeline->GetResolveInfo().spec.enabled_stages, is_indexed);
    if (mismatches != 0) [[unlikely]] {
        counters.uniform_id_mismatches.fetch_add(mismatches, std::memory_order_relaxed);
        if (VideoCommon::MT::ResolverChecks() != 0) {
            VerifyReport(0x5300, fmt::format("MT rcheck: draw {}: {} uniform binding(s) held "
                                             "another buffer than the resolver's",
                                             res.seq, mismatches));
        }
    }
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
    if (res.buffer_fail == MT::BufNone && sink.filled) {
        // EDEN_NX2_MT_RBUF: the whole payload. Streamed uniforms differ in where the bytes are
        // (the resolver's ring, the committer's staging): only their sizes are compared.
        if (sink.payload.size() != res.num_payload) {
            mismatch(12, fmt::format("payload entries: resolver {} committer {}", res.num_payload,
                                     sink.payload.size()));
            return;
        }
        const MT::Resolver::UniformRing* const ring =
            resolvers[res.resolver]->ring ? resolvers[res.resolver]->ring.get() : nullptr;
        for (u32 i = 0; i < res.num_payload; ++i) {
            const DescriptorUpdateEntry& a = res.payload[i];
            const DescriptorUpdateEntry& b = sink.payload[i];
            if (ring != nullptr) {
                const bool db = rasterizer.guest_descriptor_queue.SupportsDescriptorBuffer() &&
                                res.pipeline->GetResolveInfo().uses_descriptor_buffer;
                const bool in_ring =
                    db ? (ring->address != 0 && a.address.address >= ring->address &&
                          a.address.address < ring->address + MT::Resolver::RingBytes)
                       : a.buffer.buffer == ring->handle;
                if (in_ring) {
                    if ((db ? a.address.range != b.address.range
                            : a.buffer.range != b.buffer.range)) {
                        mismatch(13, fmt::format("payload entry {}: streamed uniform size", i));
                        return;
                    }
                    continue;
                }
            }
            if (std::memcmp(&a, &b, sizeof(a)) != 0) {
                mismatch(12, fmt::format("payload entry {} differs", i));
                return;
            }
        }
    }
    for (u32 i = 0; i < res.num_buffers; ++i) {
        const MT::BufferRef& ref = res.buffers[i];
        if (ref.kind == MT::KindUboStream) {
            continue;
        }
        if (ref.kind == MT::KindIndex && header.is_indexed == 0) {
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
    {
        // EDEN_NX2_MT_RBUF: why results left the buffer half to the committer, ring use.
        std::array<u64, MT::BufFailCount> buffer_fail{};
        u64 ring_bytes = 0;
        u64 ring_reuses = 0;
        u64 ring_streams = 0;
        for (auto& resolver : resolvers) {
            MT::ResolverCounters& c = resolver->counters;
            for (size_t i = 0; i < buffer_fail.size(); ++i) {
                buffer_fail[i] += take(c.buffer_fail[i]);
            }
            ring_bytes += take(c.ring_bytes);
            ring_reuses += take(c.ring_reuses);
            ring_streams += take(c.ring_streams);
        }
        std::string fail_text;
        for (size_t i = 1; i < buffer_fail.size(); ++i) {
            if (buffer_fail[i] != 0) {
                fail_text += fmt::format(" {} {:.0f}", MT::BufferFailNames[i], rate(buffer_fail[i]));
            }
        }
        LOG_INFO(Render_Vulkan,
                 "MT resolved buffers/s: committed {:.0f}, resolved {:.0f}, left to the "
                 "committer:{} | ring {:.0f} streams ({:.0f} reused), {:.2f} MB | uniform id "
                 "mismatches {}",
                 rate(take(counters.resolved_buffers)), rate(buffer_fail[MT::BufNone]),
                 fail_text.empty() ? std::string{" none"} : fail_text, rate(ring_streams),
                 rate(ring_reuses), static_cast<double>(ring_bytes) / 1e6 / seconds,
                 counters.uniform_id_mismatches.load(std::memory_order_relaxed));
    }
    LOG_INFO(Render_Vulkan,
             "MT resolvers/s:{} | ok {:.0f} (committed {:.0f}), slow {:.0f}:{}, missing {:.0f}, stolen {:.0f} | validation "
             "failures:{} | batches {:.0f}, commit wait {:.1f} ms, ring waits {:.0f}, barriers "
             "{:.0f} | shadow {:.0f} checks, {} mismatches, {:.0f} unvalidated | EBR pending {}",
             per_resolver, rate(take(counters.resolved_ok)), rate(take(counters.resolved_committed)),
             rate(take(counters.resolved_slow)),
             slow_text, rate(take(counters.resolved_missing)),
             rate(take(counters.resolved_stolen)), fail_text,
             rate(take(counters.batches)), static_cast<double>(take(counters.commit_wait_ns)) / 1e6 / seconds,
             rate(take(counters.ring_waits)), rate(take(counters.barriers)),
             rate(take(counters.shadow_checks)),
             counters.shadow_mismatches.load(std::memory_order_relaxed),
             rate(take(counters.shadow_unvalidated)), epochs.PendingCount());
}

inline void Nx2DrawPipeline::KickWaiters() {
    stream.WakeAll();
    commit_word.fetch_add(1, std::memory_order_seq_cst);
    commit_word.notify_all();
    serial_word.fetch_add(1, std::memory_order_seq_cst);
    serial_word.notify_all();
    for (const auto& resolver : resolvers) {
        resolver->cursor.Wake();
    }
}

inline void Nx2DrawPipeline::WatchdogReport(const std::string& why) {
    auto& split = VideoCommon::GpuSplit::Get();
    std::string resolver_text;
    for (const auto& resolver : resolvers) {
        if (!resolver->Running()) {
            continue;
        }
        resolver_text += fmt::format(" [cursor {} published batches {}]",
                                     stream.ConsumerPosition(resolver->Consumer()),
                                     resolver->cursor.PublishedCount());
    }
    LOG_WARNING(Render_Vulkan,
                "MT watchdog: {} | stream published {} committer cursor {} (quiet {}, hook {}, "
                "commit_waiting {}, commit_word {}, serial_word {}) | draws emitted {} committed {} "
                "open batch {} | resolvers{} | split consumed {} settled {} | packets {} serials "
                "{} marks {} batches {}",
                why, stream.PublishedPosition(), stream.ConsumerPosition(consumer),
                committer_quiet.load(std::memory_order_relaxed),
                hook_requested.load(std::memory_order_relaxed),
                commit_waiting.load(std::memory_order_relaxed),
                commit_word.load(std::memory_order_relaxed),
                serial_word.load(std::memory_order_relaxed),
                emitted_seq.load(std::memory_order_relaxed),
                committed_seq.load(std::memory_order_relaxed),
                open_batch.load(std::memory_order_relaxed),
                resolver_text.empty() ? std::string{" none"} : resolver_text,
                split.ConsumedPosition(), split.SettledPosition(),
                counters.packets.load(std::memory_order_relaxed),
                [&] {
                    u64 total = 0;
                    for (const auto& value : counters.serials) {
                        total += value.load(std::memory_order_relaxed);
                    }
                    return total;
                }(),
                counters.marks.load(std::memory_order_relaxed),
                counters.batches.load(std::memory_order_relaxed));
}

inline void Nx2DrawPipeline::WatchdogLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("GPUWatchdog");
    using VideoCommon::MT::WaitWatch;
    constexpr u64 Limit = 2'000'000'000ULL;
    struct Progress {
        u64 position = 0;
        u64 since = 0;
        bool reported = false;
    };
    // Committer, then resolvers.
    std::array<Progress, 1 + 8> progress{};
    u64 reported_space = 0;
    u64 recovered_seen = 0;
    const auto check_cursor = [&](Progress& p, u64 position, u64 published, u64 now,
                                  const std::string& who) {
        if (position >= published || position != p.position) {
            if (p.reported && position != p.position) {
                LOG_WARNING(Render_Vulkan, "MT watchdog: {} advanced again ({} -> {})", who,
                            p.position, position);
            }
            p = Progress{position, now, false};
            return;
        }
        if (!p.reported && now - p.since > Limit) {
            p.reported = true;
            WatchdogReport(fmt::format("{} has not advanced in {} ms with {} words pending", who,
                                       (now - p.since) / 1'000'000, published - position));
            KickWaiters();
        }
    };
    while (!stop.stop_requested()) {
        for (u32 i = 0; i < 10 && !stop.stop_requested(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        const u64 now = WaitWatch::NowNs();
        for (u32 site = 0; site < watch.slots.size(); ++site) {
            auto& slot = watch.slots[site];
            const u64 since = slot.since.load(std::memory_order_acquire);
            if (since == 0 || since > now || now - since <= Limit ||
                slot.reported_since.load(std::memory_order_relaxed) == since) {
                continue;
            }
            slot.reported_since.store(since, std::memory_order_relaxed);
            WatchdogReport(fmt::format("{} waiting {} ms (target {})",
                                       VideoCommon::MT::WaitSiteName(site),
                                       (now - since) / 1'000'000,
                                       slot.target.load(std::memory_order_relaxed)));
            KickWaiters();
        }
        if (const u64 since = stream.space_wait_since.load(std::memory_order_relaxed);
            since != 0 && since < now && now - since > Limit && since != reported_space) {
            reported_space = since;
            WatchdogReport(fmt::format("sequencer waiting {} ms for stream space",
                                       (now - since) / 1'000'000));
            KickWaiters();
        }
        const u64 published = stream.PublishedPosition();
        check_cursor(progress[0], stream.ConsumerPosition(consumer), published, now, "committer");
        for (size_t i = 0; i < resolvers.size() && i < 8; ++i) {
            if (resolvers[i]->Running()) {
                check_cursor(progress[1 + i], stream.ConsumerPosition(resolvers[i]->Consumer()),
                             published, now, fmt::format("resolver {}", i));
            }
        }
        if (const u64 recovered = watch.recovered.load(std::memory_order_relaxed);
            recovered != recovered_seen) {
            LOG_WARNING(Render_Vulkan, "MT watchdog: {} reported wait(s) ended after the report",
                        recovered - recovered_seen);
            recovered_seen = recovered;
        }
    }
}

} // namespace Vulkan
