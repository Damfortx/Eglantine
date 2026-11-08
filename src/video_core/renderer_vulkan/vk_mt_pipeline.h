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
#include <vector>

#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu_split.h"
#include "video_core/mt/capture.h"
#include "video_core/mt/draw_packet.h"
#include "video_core/mt/engine_ptr.h"
#include "video_core/mt/maxwell3d_reg_names.h"
#include "video_core/mt/reg_delta.h"
#include "video_core/mt/role.h"
#include "video_core/mt/spmc_stream.h"
#include "video_core/dirty_flags.h"
#include "video_core/nx2.h"
#include "video_core/shader_handle_program.h"
#include "video_core/textures/texture.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"

namespace Vulkan {

class RasterizerVulkan;

namespace MT {

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
    std::array<std::atomic<u64>, 16> forced_serial{};
};

inline constexpr std::array<const char*, 16> ForcedSerialNames{
    "debug-serial", "transform-feedback", "texture-buffers", "handle-program",
    "multi-range-ssbo", "draw-capture", "too-big", "plan", "mesh", "no-index-range",
    "vertex-range", "ssbo-count", "other", "", "", ""};

} // namespace MT

/// The committer pipeline (one per rasterizer, created when EDEN_NX2_MT is on).
class Nx2DrawPipeline final : public VideoCommon::MtBackInterface {
    using Maxwell3D = Tegra::Engines::Maxwell3D;
    using KeplerCompute = Tegra::Engines::KeplerCompute;
    using SpmcStream = VideoCommon::MT::SpmcStream;

public:
    explicit Nx2DrawPipeline(RasterizerVulkan& rasterizer_) : rasterizer{rasterizer_} {}

    ~Nx2DrawPipeline() override {
        Stop();
    }

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
        EmitDeltas();
        u64* const out = stream.Reserve(functor_words + 3);
        out[0] = SpmcStream::MakeHeader(static_cast<u8>(MT::StreamEntry::Command), functor_words + 3,
                                        static_cast<u32>(kind));
        out[1] = reinterpret_cast<u64>(&CommandThunk<Func>);
        out[2] = 0;
        std::memcpy(out + 3, &func, sizeof(Func));
        stream.Commit();
        Count(counters.commands, kind);
        if (lockstep) {
            WaitCommitted();
        }
    }

    /// Before the sequencer writes guest memory over pages images or command lists may use.
    void BeforeWrite(u64 device_addr, u64 size);

    /// Waits until the committer consumed everything emitted so far.
    void WaitCommitted();

    /// Sends the registers written since the last call (and the binds kept outside them).
    void EmitDeltas();

    /// RasterizerVulkan::Draw on the sequencer: capture and emit a DrawPacket (or a Serial).
    void SeqDraw(bool is_indexed, u32 instance_count);
    /// RasterizerVulkan::InnerInvalidation on the sequencer (FlushCaching).
    void SeqInnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences);

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
        u32 instance_count;
        u8 is_indexed;
        u8 native_turing;
        u8 pad[2];
        u32 inline_index_offset;
        u32 inline_index_size;
        u32 num_overlay;
        u32 num_ubo;
        u32 num_upload;
        u32 blob_size;
    };
    static constexpr u32 DrawHeaderWords = static_cast<u32>((sizeof(DrawHeader) + 7) / 8);

    /// A guest read the committer will repeat (its bytes go into the overlay).
    struct RawRead {
        u64 gpu_addr;
        u32 size;
        u32 offset;
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
    /// Copies the CPU-modified parts of a range the draw binds into the packet.
    void SeqTakeUploads(DAddr device_addr, u32 size);
    /// -1, or why this draw goes as a Serial (MT::ForcedSerialNames).
    int CaptureDraw(GraphicsPipeline* pipeline, bool is_indexed);
    void EmitDraw(GraphicsPipeline* pipeline, bool is_indexed, u32 instance_count);
    void CommitDraw(const u64* entry);
    void VerifyDraw(const DrawHeader& header, std::span<const VideoCommon::MT::OverlayEntry> overlay,
                    std::span<const VideoCommon::MT::UboEntry> ubos,
                    std::span<const VideoCommon::MT::UploadEntry> uploads, const u8* blob);
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

    void CommitterLoop(std::stop_token stop);
    void RunEntry(const u64* entry);
    void RunSerial(const u64* entry);
    void Idle(std::stop_token& stop);

    /// Committer: the mirrors of the rasterizer's bound channel (created from the real engines
    /// while the sequencer waits), installed as this thread's engine overrides.
    void BindCurrentChannel();
    ChannelMirrors* FindMirrors(const Maxwell3D* real3d);
    void VerifyMirror(const char* where);
    void LogStats();

    RasterizerVulkan& rasterizer;
    SpmcStream stream{size_t{1} << 21}; // 16 MiB
    u32 consumer{};
    std::jthread committer;
    bool running = false;
    const bool lockstep = VideoCommon::MT::Lockstep() || VideoCommon::MT::Verify();
    const bool verify = VideoCommon::MT::Verify();

    // Sequencer-owned.
    std::vector<u64> delta_scratch;
    u64 next_seq = 1;
    std::vector<RawRead> raw_reads;
    std::vector<u8> raw_bytes;
    std::vector<VideoCommon::MT::OverlayEntry> overlay;
    std::vector<VideoCommon::MT::UboEntry> ubos;
    std::vector<VideoCommon::MT::UploadEntry> uploads;
    std::vector<u8> blob;
    std::vector<std::pair<DAddr, u32>> taken_ranges;
    std::chrono::steady_clock::time_point last_stats{std::chrono::steady_clock::now()};

    // Committer-owned.
    std::vector<std::unique_ptr<ChannelMirrors>> mirrors;
    ChannelMirrors* current{};
    u64 last_mark{};

    // Shared.
    alignas(64) std::atomic<bool> hook_requested{};
    std::atomic<bool> committer_quiet{true};
    alignas(64) std::atomic<u32> serial_word{};
    alignas(64) std::atomic<u32> commit_word{};
    std::atomic<bool> commit_waiting{};
    std::atomic<bool> start_ready{};
};

} // namespace Vulkan

// ---------------------------------------------------------------------------------------------
// Implementation (needs RasterizerVulkan complete).

namespace Vulkan {

namespace MTDetail {
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
    start_ready.store(false, std::memory_order_relaxed);
    committer = std::jthread([this](std::stop_token stop) { CommitterLoop(stop); });
    while (!start_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    running = true;
    VideoCommon::MT::g_write_hook_on.store(true, std::memory_order_seq_cst);
    VideoCommon::MT::g_active.store(true, std::memory_order_seq_cst);
    LOG_INFO(Render_Vulkan, "MT: committer started (lockstep {}, verify {}, compute {})",
             lockstep, verify, VideoCommon::MT::ComputePackets() ? "packet" : "serial");
}

inline void Nx2DrawPipeline::Stop() {
    if (!running) {
        return;
    }
    committer.request_stop();
    stream.WakeAll();
    committer.join();
    VideoCommon::MT::g_active.store(false, std::memory_order_seq_cst);
    VideoCommon::MT::g_write_hook_on.store(false, std::memory_order_seq_cst);
    VideoCommon::MT::g_hooks = {};
    stream.RemoveConsumer(consumer);
    running = false;
    LOG_INFO(Render_Vulkan, "MT: committer stopped");
}

inline void Nx2DrawPipeline::SeqMark(u64 position) {
    if (VideoCommon::MT::Stats()) [[unlikely]] {
        LogStats();
    }
    auto& split = VideoCommon::GpuSplit::Get();
    // Nothing of ours in flight: the position is settled already.
    if (stream.ConsumerPosition(consumer) == stream.WritePosition()) {
        split.StoreSettled(position);
        counters.settled_shortcuts.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 payload = position;
    stream.Push(static_cast<u8>(MT::StreamEntry::Mark), 0, &payload, 1);
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

inline void Nx2DrawPipeline::SerialRaw(MT::Kind kind, void (*fn)(void*), void* arg) {
    EmitDeltas();
    std::atomic<u32> done{0};
    u64* const out = stream.Reserve(5);
    out[0] = SpmcStream::MakeHeader(static_cast<u8>(MT::StreamEntry::Serial), 5, static_cast<u32>(kind));
    out[1] = reinterpret_cast<u64>(fn);
    out[2] = reinterpret_cast<u64>(arg);
    out[3] = reinterpret_cast<u64>(&done);
    out[4] = 0;
    stream.Commit();
    Count(counters.serials, kind);
    const bool timed = VideoCommon::MT::Stats();
    const auto begin = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (u32 spins = 0; spins < 8192; ++spins) {
        if (done.load(std::memory_order_acquire) != 0) {
            break;
        }
        MTDetail::Pause();
    }
    while (done.load(std::memory_order_acquire) == 0) {
        const u32 seen = serial_word.load(std::memory_order_seq_cst);
        if (done.load(std::memory_order_seq_cst) != 0) {
            break;
        }
        serial_word.wait(seen, std::memory_order_seq_cst);
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
    const u64 target = stream.WritePosition();
    if (stream.ConsumerPosition(consumer) >= target) {
        return;
    }
    counters.wait_committed.fetch_add(1, std::memory_order_relaxed);
    for (u32 spins = 0; spins < 4096; ++spins) {
        if (stream.ConsumerPosition(consumer) >= target) {
            return;
        }
        MTDetail::Pause();
    }
    commit_waiting.store(true, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
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
        committer_quiet.store(false, std::memory_order_seq_cst);
        RunEntry(entry);
        stream.Advance(consumer, entry);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (commit_waiting.load(std::memory_order_relaxed)) {
            commit_word.fetch_add(1, std::memory_order_seq_cst);
            commit_word.notify_all();
        }
        if (hook_requested.load(std::memory_order_relaxed)) [[unlikely]] {
            hook_requested.store(false, std::memory_order_seq_cst);
            rasterizer.scheduler.FlushDeferred();
        }
    }
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
    for (u32 spins = 0; spins < 4096; ++spins) {
        if (stream.Peek(consumer) != nullptr || stop.stop_requested()) {
            return;
        }
        MTDetail::Pause();
    }
    if (!committer_quiet.load(std::memory_order_relaxed) ||
        hook_requested.load(std::memory_order_relaxed)) {
        hook_requested.store(false, std::memory_order_seq_cst);
        rasterizer.scheduler.FlushDeferred();
        committer_quiet.store(true, std::memory_order_seq_cst);
    }
    stream.WaitForData(consumer, [&] {
        return stop.stop_requested() || hook_requested.load(std::memory_order_seq_cst);
    });
}

inline void Nx2DrawPipeline::RunEntry(const u64* entry) {
    const u64 head = entry[0];
    const u32 aux = SpmcStream::HeaderAux(head);
    switch (static_cast<MT::StreamEntry>(SpmcStream::HeaderKind(head))) {
    case MT::StreamEntry::Deltas:
        if (current != nullptr) {
            VideoCommon::MT::MirrorApplier<Maxwell3D>::ApplyPairs(*current->mirror3d, entry + 1, aux);
        }
        break;
    case MT::StreamEntry::ComputeDeltas:
        if (current != nullptr && current->mirror_compute) {
            VideoCommon::MT::MirrorApplier<KeplerCompute>::ApplyPairs(*current->mirror_compute,
                                                                      entry + 1, aux);
        }
        break;
    case MT::StreamEntry::CbBind:
        if (current != nullptr) {
            for (u32 i = 0; i < aux; ++i) {
                VideoCommon::MT::CbBindDelta bind;
                std::memcpy(&bind, entry + 1 + i * 2, sizeof(bind));
                Maxwell3D::ApplyCbBind(current->mirror3d->state, bind);
            }
        }
        break;
    case MT::StreamEntry::RootTable:
        if (current != nullptr) {
            for (u32 i = 0; i < aux; ++i) {
                const u64 op = entry[1 + i];
                Maxwell3D::ApplyRootTableMethod(current->mirror3d->root_tables,
                                                static_cast<u32>(op), static_cast<u32>(op >> 32));
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
    case MT::StreamEntry::Mark:
        last_mark = entry[1];
        VideoCommon::GpuSplit::Get().StoreSettled(entry[1]);
        break;
    default:
        break;
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
    const u64 words = take(counters.packet_words);
    LOG_INFO(Render_Vulkan,
             "MT stats/s: packets {:.0f} ({:.0f} B avg) | serial {:.0f} ({:.1f} ms wait):{} | "
             "commands:{} | forced serial:{} | marks {:.0f}, shortcuts {:.0f}, wait-committed "
             "{:.0f}, write barriers {:.0f}, delta words {:.0f} | verify {:.0f} checks, {} "
             "mismatches, {} concurrent CPU writes",
             static_cast<double>(packets) / seconds,
             packets != 0 ? static_cast<double>(words * 8) / static_cast<double>(packets) : 0.0,
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
}

} // namespace Vulkan

// ---- Draw packets ----

namespace Vulkan {

template <typename T>
T Nx2DrawPipeline::SeqRead(u64 gpu_addr) {
    const T value = rasterizer.gpu_memory->template Read<T>(gpu_addr);
    const u32 offset = static_cast<u32>(raw_bytes.size());
    raw_bytes.resize(raw_bytes.size() + sizeof(T));
    std::memcpy(raw_bytes.data() + offset, &value, sizeof(T));
    raw_reads.push_back({gpu_addr, static_cast<u32>(sizeof(T)), offset});
    return value;
}

inline void Nx2DrawPipeline::SeqReadDescriptor(u64 gpu_addr) {
    // DescriptorTable::Read's read of a TIC or TSC entry.
    constexpr u32 Size = 32;
    const u32 offset = static_cast<u32>(raw_bytes.size());
    raw_bytes.resize(raw_bytes.size() + Size);
    u8* const out = raw_bytes.data() + offset;
    const bool one_page =
        (gpu_addr & Core::Memory::YUZU_PAGEMASK) + Size <= Core::Memory::YUZU_PAGESIZE;
    if (const u8* const host = one_page ? rasterizer.gpu_memory->GetPointer(gpu_addr) : nullptr) {
        std::memcpy(out, host, Size);
    } else {
        rasterizer.gpu_memory->ReadBlockUnsafe(gpu_addr, out, Size);
    }
    raw_reads.push_back({gpu_addr, Size, offset});
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
        }
        uploads.push_back(entry);
    });
}

inline int Nx2DrawPipeline::CaptureDraw(GraphicsPipeline* pipeline, bool is_indexed) {
    using Maxwell = Maxwell3D::Regs;
    Maxwell3D& real = *rasterizer.maxwell3d.Real();
    Tegra::MemoryManager& memory = *rasterizer.gpu_memory;
    const auto& regs = real.regs;
    const auto& infos = pipeline->StageInfos();
    raw_reads.clear();
    raw_bytes.clear();
    overlay.clear();
    ubos.clear();
    uploads.clear();
    blob.clear();
    taken_ranges.clear();

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
    const bool prefers_sparse = [&] {
        std::scoped_lock lock{rasterizer.buffer_cache.mutex};
        return rasterizer.buffer_cache.MtPrefersSparseSources();
    }();
    for (size_t stage = 0; stage < infos.size(); ++stage) {
        const Shader::Info& info = infos[stage];
        const auto& cbufs = real.state.shader_stages[stage].const_buffers;
        for (const auto& desc : info.storage_buffers_descriptors) {
            const GPUVAddr ssbo_addr = cbufs[desc.cbuf_index].address + desc.cbuf_offset;
            const GPUVAddr gpu_addr = SeqRead<u64>(ssbo_addr);
            if (gpu_addr == 0) {
                continue;
            }
            const u64 next_qword = SeqRead<u64>(ssbo_addr + 8);
            const VideoCommon::Binding binding = rasterizer.buffer_cache.MtStorageBinding(
                gpu_addr, next_qword, desc.cbuf_index, desc.is_written);
            if (binding.size == 0 || binding.device_addr == 0) {
                continue;
            }
            if ((!desc.is_written || prefers_sparse) &&
                !memory.IsContinuousRange(binding.gpu_addr, binding.size)) {
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
            if (index <= tic_limit) {
                SeqReadDescriptor(regs.tex_header.Address() + u64{index} * 32);
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
    std::scoped_lock lock{rasterizer.buffer_cache.mutex};
    const auto& draw_state = real.draw_manager.draw_state;
    if (is_indexed && draw_state.inline_index_draw_indexes.empty()) {
        const auto& index_buffer = draw_state.index_buffer;
        const GPUVAddr begin = index_buffer.StartAddress();
        const u32 address_size = static_cast<u32>(index_buffer.EndAddress() - begin);
        const u32 draw_size = (index_buffer.count + index_buffer.first) *
                              static_cast<u32>(index_buffer.FormatSizeInBytes());
        const u32 size = (std::min)(address_size, draw_size);
        if (const auto device_addr = memory.GpuToCpuAddress(begin); device_addr && size != 0) {
            SeqTakeUploads(*device_addr, size);
        }
    }
    for (u32 index = 0; index < Maxwell::NumVertexArrays; ++index) {
        const auto& array = regs.vertex_streams[index];
        if (array.enable == 0) {
            continue;
        }
        const GPUVAddr begin = array.Address();
        const GPUVAddr end = native ? begin + regs.NativeVertexStreamSize(index)
                                    : regs.vertex_stream_limits[index].Address() + 1;
        const auto device_addr = memory.GpuToCpuAddress(begin);
        u32 size = static_cast<u32>(end - begin);
        if (size == 0 || !device_addr) {
            continue;
        }
        if (!memory.IsWithinGPUAddressRange(end) || size >= 64 * 1024 * 1024) {
            size = static_cast<u32>(memory.MaxContinuousRange(begin, size));
        }
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
                if (const auto translated = memory.GpuToCpuAddress(cbuf.address)) {
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
                .pad = 0,
                .size = size,
                .device_addr = device_addr,
                .offset = 0,
                .pad2 = 0,
            };
            if (fast) {
                entry.offset = static_cast<u32>(blob.size());
                blob.resize(blob.size() + ((size + 7) & ~7U));
                rasterizer.device_memory.ReadBlockUnsafe(device_addr, blob.data() + entry.offset, size);
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
    return -1;
}

inline void Nx2DrawPipeline::EmitDraw(GraphicsPipeline* pipeline, bool is_indexed,
                                      u32 instance_count) {
    Maxwell3D& real = *rasterizer.maxwell3d.Real();
    const auto& state = real.draw_manager.draw_state;
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
    const u32 words = 1 + DrawHeaderWords + overlay_words + ubo_words + upload_words + blob_words;
    u64* const out = stream.Reserve(words);
    out[0] = SpmcStream::MakeHeader(static_cast<u8>(MT::StreamEntry::DrawPacket), words, 0);
    DrawHeader header{};
    header.seq = next_seq++;
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
    header.native_turing = real.GetNativeMacroEngine().native_turing ? 1 : 0;
    header.inline_index_offset = inline_offset;
    header.inline_index_size = inline_size;
    header.num_overlay = static_cast<u32>(overlay.size());
    header.num_ubo = static_cast<u32>(ubos.size());
    header.num_upload = static_cast<u32>(uploads.size());
    header.blob_size = static_cast<u32>(blob.size());
    std::memset(out + 1, 0, DrawHeaderWords * 8);
    std::memcpy(out + 1, &header, sizeof(header));
    u64* cursor = out + 1 + DrawHeaderWords;
    std::memcpy(cursor, overlay.data(), overlay_words * 8);
    cursor += overlay_words;
    std::memcpy(cursor, ubos.data(), ubo_words * 8);
    cursor += ubo_words;
    std::memcpy(cursor, uploads.data(), upload_words * 8);
    cursor += upload_words;
    std::memcpy(cursor, blob.data(), blob.size());
    stream.Commit();
    counters.packets.fetch_add(1, std::memory_order_relaxed);
    counters.packet_words.fetch_add(words, std::memory_order_relaxed);
    if (lockstep) {
        WaitCommitted();
    }
}

inline void Nx2DrawPipeline::SeqDraw(bool is_indexed, u32 instance_count) {
    RasterizerVulkan& r = rasterizer;
    // The draw's sync requests (CPU reads, composites) run on the committer after it.
    SCOPE_EXIT {
        r.gpu.TickWork();
    };
    const auto serial_draw = [&] {
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
    const int reason = CaptureDraw(pipeline, is_indexed);
    if (reason >= 0) {
        ForceSerialReason(static_cast<u32>(reason));
        serial_draw();
        return;
    }
    EmitDraw(pipeline, is_indexed, instance_count);
}

inline void Nx2DrawPipeline::CommitDraw(const u64* entry) {
    DrawHeader header;
    std::memcpy(&header, entry + 1, sizeof(header));
    const u64* cursor = entry + 1 + DrawHeaderWords;
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
        VerifyDraw(header, overlay_span, ubo_span, upload_span, blob_data);
    }
    r.FlushWork();
    VideoCommon::MT::CaptureView view{
        .overlay = overlay_span,
        .ubos = ubo_span,
        .blob = blob_data,
        .verify = verify,
    };
    {
        std::scoped_lock lock{r.buffer_cache.mutex, r.texture_cache.mutex};
        r.buffer_cache.MtApplyUploads(upload_span, blob_data);
        VideoCommon::MT::t_capture = &view;
        r.mt_commit_pipeline = header.pipeline;
        SCOPE_EXIT {
            VideoCommon::MT::t_capture = nullptr;
            r.mt_commit_pipeline = nullptr;
        };
        r.mesh_draw = header.pipeline->IsMeshPipeline();
        const bool is_indexed = header.is_indexed != 0;
        try {
            if (r.ConfigureDrawPipeline(header.pipeline, is_indexed)) {
                r.RecordDraw(is_indexed, header.instance_count);
            }
        } catch (const std::exception& e) {
            LOG_ERROR(Render_Vulkan, "MT: draw packet threw: {}", e.what());
        }
    }
    if (verify) {
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
                                        const u8* blob_data) {
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
    std::vector<u8> bytes;
    // Bytes that differ from the capture are a capture bug unless a guest CPU thread wrote them
    // since (the sequencer waits for this packet; the game's threads don't): its write trap
    // marked them CPU-modified in the buffer tracker.
    const auto written_by_cpu = [&](DAddr device_addr, u64 size) {
        std::scoped_lock lock{rasterizer.buffer_cache.mutex};
        return rasterizer.buffer_cache.IsRegionCpuModified(device_addr, size);
    };
    for (const auto& entry : overlay_span) {
        bytes.resize(entry.size);
        rasterizer.gpu_memory->ReadBlockUnsafe(entry.gpu_addr, bytes.data(), entry.size);
        if (std::memcmp(bytes.data(), blob_data + entry.offset, entry.size) != 0) {
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
        if (std::memcmp(bytes.data(), blob_data + entry.offset, entry.size) != 0) {
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
            VerifyReport(0x3002, fmt::format("draw {}: upload at {:#x} (+{}) changed",
                                             header.seq, entry.device_addr, entry.size));
        }
    }
}

inline void Nx2DrawPipeline::SeqInnerInvalidation(
    std::span<const std::pair<DAddr, std::size_t>> sequences) {
    RasterizerVulkan& r = rasterizer;
    if (Settings::values.skip_cpu_inner_invalidation.GetValue()) {
        return;
    }
    const bool fast = Tegra::Nx2InvalFast();
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
    // Buffers: GPU data on the written pages is downloaded first, which only the scheduler's
    // owner may do: those ranges go to the committer.
    boost::container::small_vector<std::pair<DAddr, std::size_t>, 8> owner_ranges;
    {
        std::scoped_lock lock{r.buffer_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            if (r.buffer_cache.MtPageHoldsGpuData(addr, size)) {
                owner_ranges.emplace_back(addr, size);
            } else {
                r.buffer_cache.WriteMemory(addr, size);
            }
        }
    }
    boost::container::small_vector<std::pair<DAddr, std::size_t>, 8> query_ranges;
    for (const auto& [addr, size] : sequences) {
        if (!fast || Tegra::Nx2AnyPage(Tegra::g_nx2_query_pages, addr, size)) {
            query_ranges.emplace_back(addr, size);
        }
    }
    if (!owner_ranges.empty() || !query_ranges.empty()) {
        Serial(MT::Kind::InvalidateRegion, [&] {
            if (!owner_ranges.empty()) {
                std::scoped_lock lock{r.buffer_cache.mutex};
                for (const auto& [addr, size] : owner_ranges) {
                    r.buffer_cache.WriteMemory(addr, size);
                }
            }
            for (const auto& [addr, size] : query_ranges) {
                r.query_cache.InvalidateRegion(addr, size);
            }
        });
    }
    r.pipeline_cache.InvalidateRegions(sequences);
}

} // namespace Vulkan
