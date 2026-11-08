// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Multithreaded GPU back, phase 4 (docs/mt-plan.md, docs/mt-phase4-spec.md): the split's back
// thread becomes the sequencer (engines, guest memory, captures) and a committer thread owns the
// Vulkan scheduler and records. Included by vk_rasterizer.cpp only.

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu_split.h"
#include "video_core/mt/draw_packet.h"
#include "video_core/mt/engine_ptr.h"
#include "video_core/mt/maxwell3d_reg_names.h"
#include "video_core/mt/reg_delta.h"
#include "video_core/mt/role.h"
#include "video_core/mt/spmc_stream.h"
#include "video_core/dirty_flags.h"
#include "video_core/nx2.h"
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
             "mismatches",
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
             counters.verify_mismatches.load(std::memory_order_relaxed));
}

} // namespace Vulkan
