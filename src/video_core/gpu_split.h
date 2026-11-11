// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <deque>
#include <unordered_map>
#include <optional>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>

#include "common/common_types.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"
#include "video_core/mt/lite_switches.h"
#include "video_core/mt/lite3_front_switches.h"
#include "video_core/mt/lite4_switches.h"
#include "video_core/mt/lite5_list_switches.h"
#include "video_core/mt/lite6_drain_switches.h"
#include "video_core/mt/c10_front.h"
#include "video_core/mt/c11_front.h"
#include "video_core/mt/c12_front.h"
#include "video_core/mt/c13_front.h"
#include "video_core/mt/seq_diet.h"

namespace Core {
class System;
}

namespace Tegra::Engines {
class EngineInterface;
}

namespace VideoCommon {

// Splits the GPU thread in two (NX2_GPU_THREAD_SPLIT.md). The GPU thread (the front) takes the
// CPU's commands, parses command lists and runs the puller; engine method calls and everything
// behind them (register state, macros, draws, the rasterizer and its caches) run in order on a
// second thread (the back), fed through a single-producer single-consumer ring. Work the front
// would do on back-owned state is deferred into the same ring; the front waits for the back to
// catch up (Drain) only where it needs a result from that state, e.g. a semaphore it acquires.
// On for Switch 2 titles with the 3D and compute engines' macros on the front, against copies of
// the registers they read (Maxwell3D and KeplerCompute Front*), which sends the writes they make.
// EDEN_NX2_SPLIT=1 keeps the macros on the back; EDEN_NX2_SPLIT=0 runs everything on one thread.
/// A thread pinned to a core of its own that runs one job at a time for the split's back while
/// the back does other work, then joins (title 1's draws are ~10 us each: a pinned handoff costs
/// ~0.1 us, an unpinned one 2-3 us). It spins while jobs come, and sleeps after ~1 ms without one.
/// EDEN_NX2_BACK_HELPER=1 turns it on (=a alternates; off by default); EDEN_NX2_HELPER_CPUS=<back>,<helper>
/// picks the cores (default 2,3; -1 leaves a thread unpinned).
class BackHelper {
public:
    static BackHelper& Get();
    /// Whether jobs may be posted now (on, and on the split's back).
    [[nodiscard]] bool Enabled();
    /// Starts `job(context)` on the helper. One job at a time: Join before the next.
    void Post(void (*job)(void*), void* context);
    /// Waits for the posted job.
    void Join();

private:
    BackHelper();
    void Loop(std::stop_token stop);

    alignas(64) std::atomic<u64> posted{0};
    alignas(64) std::atomic<u64> done{0};
    alignas(64) std::atomic<u32> sleeping{0};
    void (*job_function)(void*) = nullptr;
    void* job_context = nullptr;
    bool back_pinned = false;
    std::jthread thread;
};

/// The multithreaded back's pipeline as the split sees it (renderer_vulkan/vk_mt_pipeline.h,
/// EDEN_NX2_MT): the back becomes the sequencer and a committer thread records.
class MtBackInterface {
public:
    virtual ~MtBackInterface() = default;
    /// Front, before the back thread starts: starts the committer.
    virtual void Start() = 0;
    /// After the back thread stopped: drains and joins the committer.
    virtual void Stop() = 0;
    /// Back: everything before ring position `position` was executed.
    virtual void SeqMark(u64 position) = 0;
    /// Any thread: run the committer's idle hook soon.
    virtual void RequestHook() = 0;
    /// Whether the committer ran its idle hook and has nothing queued.
    [[nodiscard]] virtual bool Quiet() const = 0;
};

class GpuSplit {
public:
    static GpuSplit& Get() {
        return instance;
    }

    /// Registers the calling thread with the emulated system (a thread that may run guest
    /// callbacks, like the back).
    void RegisterHostThread();
    /// Set by the rasterizer when EDEN_NX2_MT is on (before the split starts).
    void SetMtBack(MtBackInterface* back) {
        mt_back = back;
    }
    /// Whether the back runs as the sequencer of the multithreaded back (any thread).
    [[nodiscard]] bool MtActive() const {
        return mt_active.load(std::memory_order_acquire);
    }
    /// Ring position whose effects are committed (== consumed without the multithreaded back).
    [[nodiscard]] u64 SettledPosition() const {
        return MtActive() ? settled.load(std::memory_order_acquire)
                          : consumed.load(std::memory_order_acquire);
    }
    /// Committer: everything before `position` is committed.
    void StoreSettled(u64 position);
    /// Sequencer (EDEN_NX2_MT_DMA_CONSUMED): the entry it runs queued a Command that writes guest
    /// memory on the committer (fence/semaphore releases, query reports, downloads, DMA...).
    /// `bulk`: not a semaphore/report word nor a fence (a DMA copy or clear, a download, other
    /// work): what a command list or an MME DMA read the front noted no write for may still wait
    /// for (EDEN_NX2_MT_LITE_WRITERS).
    void NoteCommitterWrite(bool bulk = true) {
        const u64 position = back_exec_position + 1;
        exec_wrote_committer = true;
        if (bulk && committer_bulk_write_position.load(std::memory_order_relaxed) < position) {
            committer_bulk_write_position.store(position, std::memory_order_release);
        }
        if (committer_write_position.load(std::memory_order_relaxed) < position) {
            committer_write_position.store(position, std::memory_order_release);
        }
    }

    /// Committer thread start/stop (the scheduler owner under the multithreaded back).
    static void MarkCommitterThread(bool on) {
        on_committer_thread = on;
    }
    [[nodiscard]] static bool OnCommitterThread() {
        return on_committer_thread;
    }

    /// Whether the front sends work to the back (front thread only).
    [[nodiscard]] bool Active() const {
        return active;
    }
    /// Whether the front runs the engines' macros (EDEN_NX2_SPLIT=2).
    [[nodiscard]] bool FrontMacros() const {
        return front_macros;
    }

    /// Whether state writes are sent as blocks of final values (EDEN_NX2_SPLIT_STATE, front only).
    [[nodiscard]] bool StateBlocks() const {
        return state_blocks;
    }

    /// Whether the calling thread is the back.
    [[nodiscard]] static bool OnBackThread() {
        return on_back_thread;
    }
    /// Marks the calling thread as the GPU thread (the front, or the only one without the split).
    static void MarkGpuThread() {
        on_gpu_thread = true;
    }
    /// Whether the calling thread owns the Vulkan scheduler: the back while it runs, else the
    /// GPU thread. Other threads (CPU cores, services) must not record or submit work.
    [[nodiscard]] static bool OwnsScheduler() {
        if (Get().MtActive()) {
            return on_committer_thread;
        }
        return Get().BackRunning() ? on_back_thread : on_gpu_thread;
    }
    /// Whether the calling thread runs the idle hook (submits deferred flushes): the back, or the
    /// committer under the multithreaded back.
    [[nodiscard]] static bool OnIdleHookThread() {
        return Get().MtActive() ? on_committer_thread : on_back_thread;
    }
    /// Whether the thread that runs the idle hook is running.
    [[nodiscard]] bool IdleHookThreadRunning() const {
        return MtActive() ? committer_running.load(std::memory_order_acquire) : BackRunning();
    }
    /// Called on the back before it sleeps waiting for work (the rasterizer submits deferred work).
    void SetIdleHook(void (*hook)(void*), void* argument) {
        idle_hook = hook;
        idle_argument = argument;
    }
    /// Any thread: whether the back thread runs (false before it starts and once it stopped).
    [[nodiscard]] bool BackRunning() const {
        return back_running.load(std::memory_order_acquire);
    }
    /// Any thread: has the back call the idle hook soon, between entries or at once if it sleeps.
    void RequestIdleHook() {
        if (MtActive()) {
            mt_back->RequestHook();
            // EDEN_NX2_MT_LITE_QUIET: the sequencer's idle spin also ends on the request (it
            // stores back_quiet after it; it runs no hook of its own under the MT back). A
            // sleeping sequencer is quiet already: no wake.
            if (VideoCommon::MT::LiteQuiet()) {
                hook_requested.store(true, std::memory_order_seq_cst);
            }
            return;
        }
        hook_requested.store(true, std::memory_order_seq_cst);
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }

    /// Starts the back thread once the split is wanted (EDEN_NX2_SPLIT=1, a Switch 2 title,
    /// commands read without cache flushes). Called by the front between commands.
    void MaybeStart(Core::System& system);
    void Stop();

    /// An engine method write, executed as DmaPusher would (method sink, ConsumeSink, call).
    /// `segment`: the engine's current_dma_segment for the call, when the engine reads it.
    void Method(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument, bool is_last,
                const GPUVAddr* segment) {
        ReleaseFrontHold(HoldMethod | (method & 0xffff)); // W1 GPUPARAM v2 front hold
        FlushState();
        SelectEngine(engine);
        const size_t words = segment != nullptr ? 2 : 1;
        u64* const entry = Reserve(words);
        entry[0] = static_cast<u64>(Kind::Method) | (is_last ? IsLast : 0) |
                   (segment != nullptr ? HasSegment : 0) | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        if (segment != nullptr) {
            entry[1] = *segment;
        }
        Commit(words);
    }
    void MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                     u32 amount, u32 methods_pending, const GPUVAddr* segment);
    /// A write made by a macro: CallMethod without the method sink, as the engines' macros do.
    /// A register write a macro makes (run even if not executable). An engine's writes are
    /// gathered and go to the back as one entry (Kind::List) before any entry of another kind:
    /// title 1's macros make ~2.5 million writes a second, each an entry of its own before.
    /// `mergeable`: a constant buffer data word (3D) or inline upload word (compute), whose
    /// consecutive runs the back runs as one multi-method (one write, not one a word).
    void MacroWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument,
                    bool mergeable = false) {
        ReleaseFrontHold(HoldMacroWrite | (method & 0xffff)); // W1 GPUPARAM v2 front hold
        // With the back idle half the time (title 1's presents waiting 3 ms for their own frame)
        // the lists showed no gain; with that gone the back is the limit again and they gave
        // 0.8-3.2 more frames a second, 0.64 us less back time a draw (medians, two A/B runs).
        // EDEN_NX2_MACRO_LIST=0 sends each write as an entry (=a alternates).
        static const char list_mode = Tegra::Nx2EnvMode("EDEN_NX2_MACRO_LIST");
        if (!Tegra::Nx2Switch(list_mode, true)) {
            if (mergeable && list_mode != '0') {
                // Without lists, the constant buffer / upload words still go as runs.
                if (list_count != 0 && list_engine != engine) {
                    FlushState();
                }
                if (state_count != 0) {
                    FlushState();
                }
                list_engine = engine;
                list[list_count++] = u64{method & 0xffff} | Mergeable | (u64{argument} << 32);
                if (list_count == list.size()) {
                    FlushMacroList();
                }
                return;
            }
            FlushState();
            SelectEngine(engine);
            u64* const entry = Reserve(1);
            entry[0] = static_cast<u64>(Kind::Method) | IsLast | Direct |
                       (u64{method & 0xffff} << 8) | (u64{argument} << 32);
            Commit(1);
            return;
        }
        // State gathered since goes first (after the list gathered before it: FlushState).
        if (state_count != 0 ||
            (list_count != 0 && (list_engine != engine || list_count == list.size()))) {
            FlushState();
        }
        list_engine = engine;
        list[list_count++] =
            u64{method & 0xffff} | (mergeable ? Mergeable : 0) | (u64{argument} << 32);
        // Out as soon as it would have been published as entries of its own: held longer, the
        // back waited for whole lists (draws come from macros too) and ran ~2% fewer frames.
        if (write_position + list_count + 1 - published_local >= publish_every) {
            FlushMacroList();
        }
    }
    void FlushMacroList();

    /// Runs `func` on the back after everything sent before it. Must be trivially copyable.
    template <typename Func>
    void Defer(const Func& func) {
        ReleaseFrontHold(HoldDefer); // W1 GPUPARAM v2 front hold
        static_assert(std::is_trivially_copyable_v<Func> && alignof(Func) <= alignof(u64));
        constexpr size_t payload = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        FlushState();
        u64* const entry = Reserve(2 + payload);
        entry[0] = static_cast<u64>(Kind::Call) | (u64{payload} << 8);
        entry[1] = reinterpret_cast<u64>(&CallThunk<Func>);
        std::memcpy(entry + 2, &func, sizeof(Func));
        Commit(2 + payload);
    }
    void DeferFunction(std::function<void()>&& func);

    // ---- W1 GPUPARAM v2 (mt/gpuparam2.h): the front hold ----
    /// A draw the front holds back instead of sending it (a pending group of GPU-parameter
    /// draws that the next call may join). Every function that sends to the ring or waits for
    /// the back runs it first (ReleaseFrontHold at its top), so it is sent before anything that
    /// follows it in the stream. `why`: Hold* kind | method (for the method kinds), statistics.
    /// Publish has no release: it runs inside Reserve (WaitForSpace) mid-entry, and every wait
    /// that publishes releases first; DmaPusher::DispatchCalls releases at its end.
    using FrontHoldFn = void (*)(void* context, u32 why);
    static constexpr u32 HoldMethod = 1U << 16;
    static constexpr u32 HoldMultiMethod = 2U << 16;
    static constexpr u32 HoldMacroWrite = 3U << 16;
    static constexpr u32 HoldStateWrite = 4U << 16;
    static constexpr u32 HoldDefer = 5U << 16;
    static constexpr u32 HoldDeferFunction = 6U << 16;
    static constexpr u32 HoldDrain = 7U << 16;
    static constexpr u32 HoldWait = 8U << 16;
    static constexpr u32 HoldRun = 9U << 16;
    static constexpr u32 HoldDispatchEnd = 10U << 16;
    static constexpr u32 HoldNonJoinable = 11U << 16;
    static constexpr u32 HoldFull = 12U << 16;
    static constexpr u32 HoldOther = 13U << 16;
    /// Installs the hold (front thread only; replaces none: release the old one first).
    void SetFrontHold(FrontHoldFn fn, void* context) {
        hold_fn = fn;
        hold_ctx = context;
        hold_thread = true;
    }
    /// The installed hold's context (nullptr: none).
    [[nodiscard]] void* FrontHoldContext() const {
        return hold_fn != nullptr ? hold_ctx : nullptr;
    }
    /// Takes the hold off without running it (its context; nullptr: none). Resume with
    /// SetFrontHold. Also used to drop it (Stop, its owner going away).
    void* TakeFrontHold() {
        void* const context = hold_fn != nullptr ? hold_ctx : nullptr;
        hold_fn = nullptr;
        hold_ctx = nullptr;
        return context;
    }
    /// Runs the hold (cleared first: it sends with Defer) if one is installed.
    void ReleaseFrontHold(u32 why) {
        if (hold_fn != nullptr) [[unlikely]] {
            ReleaseFrontHoldSlow(why);
        }
    }
    void ReleaseFrontHoldSlow(u32 why) {
        if (!hold_thread) {
            return; // not the front thread that installed it
        }
        const FrontHoldFn fn = hold_fn;
        void* const context = hold_ctx;
        hold_fn = nullptr;
        hold_ctx = nullptr;
        fn(context, why);
    }
    FrontHoldFn hold_fn{};
    void* hold_ctx{};
    static inline thread_local bool hold_thread{};
    // ---- end W1 front hold ----

    /// A write to a register that is not executable (state only), with the shadow RAM already
    /// applied by the front: `value` is what the register will hold, and `track_shadow` whether
    /// the shadow copy takes it too. Writes are gathered until the next entry of another kind and
    /// sent as one block holding each register's last value, so the back applies the state
    /// instead of running every write through the engine (~3 million a second in title 1).
    void StateWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 value,
                    bool track_shadow) {
        ReleaseFrontHold(HoldStateWrite | (method & 0xffff)); // W1 GPUPARAM v2 front hold
        if (engine != state_engine || track_shadow != state_track) [[unlikely]] {
            FlushState();
            state_engine = engine;
            state_track = track_shadow;
        }
        const u64 pair = u64{method} | (u64{value} << 32);
        if (state_generation[method] == state_current) {
            state_pairs[state_slot[method]] = pair;
        } else {
            state_generation[method] = state_current;
            state_slot[method] = static_cast<u16>(state_count);
            state_pairs[state_count++] = pair;
        }
    }
    /// Sends the gathered state writes (before any other entry, publish or wait).
    void FlushState() {
        if (list_count != 0) {
            FlushMacroList();
        }
        if (state_count != 0) {
            FlushStateSlow();
        }
    }

    /// EDEN_NX2_SPLIT_CHECK=1 (debug): reports, with its stack, a thread other than the back
    /// using back-owned state (the Vulkan scheduler) while the back runs entries.
    static inline bool check_owner{};
    static void CheckOwner(const char* where);

    /// Makes what was sent visible to the back.
    void Publish();
    /// Waits until the back executed everything sent so far.
    /// Why the front waits for the back (counted with EDEN_NX2_GPU_PROFILE).
    enum class DrainReason : u32 {
        Other,
        Sync,                ///< sync_memory_operations fence
        GpuWrittenCommands,  ///< command list on GPU-written pages
        FrontState,          ///< first macro on the front
        DmaRead,             ///< MME DMA read (indirect arguments)
        Verify,              ///< EDEN_NX2_SPLIT_VERIFY
        Puller,              ///< semaphores, syncpoints, puller reads
        // Partial waits (not full drains), counted on the same log line:
        WritersCommands,     ///< WaitForWriters before reading a command list
        WritersDmaRead,      ///< WaitForWriters before an MME DMA read
        Position,            ///< WaitForPosition
        RingFull,            ///< the ring has no room (WaitForSpace)
        WritersCpuCommands,  ///< WaitForWriters before a command list not known GPU-written
                             ///< (EDEN_NX2_MT_LITE_WRITERS; else WritersCommands)
        Count,
    };
    void Drain(DrainReason reason = DrainReason::Other);
    /// Front, before an MME DMA read of pages the GPU has not written: everything sent has run
    /// on the sequencer, and the committer has run every guest memory write queued so far.
    /// Without the multithreaded back (or EDEN_NX2_MT_DMA_CONSUMED=0) a Drain.
    void DrainForRead(DrainReason reason);

    /// Runs `func` on the back and waits for it: for work on back-owned state, such as a buffer
    /// cache download that records into the Vulkan scheduler. A drained back still submits from
    /// its idle hook, so the front never touches that state itself.
    template <typename Func>
    void Run(const Func& func, DrainReason reason = DrainReason::Other) {
        ReleaseFrontHold(HoldRun); // W1 GPUPARAM v2 front hold
        if (on_back_thread || !active) {
            func();
            return;
        }
        Defer(func);
        Drain(reason);
    }
    /// As Run, for a `func` that ends with a cache flush whose memory the caller reads after
    /// this returns. Deferred, the drain waits for the committer to pass its mark (MtActive), so
    /// the multithreaded back may queue the flush instead of waiting (EDEN_NX2_MT_FLUSHCMD).
    /// Inline (on the back or without the split), the flush stays synchronous.
    template <typename Func>
    void RunFlush(const Func& func, DrainReason reason = DrainReason::Other) {
        ReleaseFrontHold(HoldRun); // W1 GPUPARAM v2 front hold
        if (on_back_thread || !active) {
            func();
            return;
        }
        Defer([func] {
            const MT::AsyncFlushScope scope;
            func();
        });
        Drain(reason);
    }

    /// A semaphore release (payload written at `address`) was just sent to the back.
    void NoteRelease(GPUVAddr address, u32 payload, bool at_least = false) {
        if (PredictAcquires()) {
            FlushState();
            pending_releases[address] = PendingRelease{payload,          write_position,
                                                       at_least,         C10NextNoteSeq(),
                                                       ++c11_note_order, c11_channel};
        }
    }
    /// The value a release still on its way will write at `address`: one sent but not yet in
    /// memory (`current` is what memory holds now). An acquire it meets need not wait for the
    /// back: everything after the acquire goes behind that release in the ring.
    struct Prediction {
        u32 value;
        bool at_least; ///< The release leaves at least `value` (a max release): only >= acquires
    };
    /// Guest memory a method sent to the back writes when it runs (inline uploads, reports): a
    /// front read of it must drain first. Cleared by Drain. Copy and compute engines are noted as
    /// writers instead (NoteWriter), their ranges aren't known here.
    /// Ring positions: what the front has sent so far, and what the back has run.
    [[nodiscard]] u64 SentPosition() {
        FlushState();
        return write_position;
    }
    [[nodiscard]] u64 ConsumedPosition() const {
        return consumed.load(std::memory_order_acquire);
    }

    /// How a noted write is known to have run: once the back is past `position` (noted after
    /// the entry that makes it was sent), with the copy and compute writers (WaitForWriters),
    /// or only once everything sent so far has run.
    enum class WriteKnown : u8 { Position, Writers, Drain };
    /// NotePendingWrite `flags`. WriteDirect64 (EDEN_NX2_MT_LITE_OVERLAY64/SEQPOS): a 16-byte
    /// payload release the sequencer writes when it runs the entry (RasterizerVulkan::Query's
    /// direct path: no fence, GPU accuracy not high): word 0 = the value, word 1 = 0.
    static constexpr u8 WriteDirect64 = 1;
    void NotePendingWrite(GPUVAddr address, u64 size, WriteKnown known = WriteKnown::Position,
                          u8 tag = 0, std::optional<u32> value = std::nullopt, u8 flags = 0) {
        // Writes known with the writers only ever make reads of their range wait for the
        // writers, so one entry a range is enough: title 1's inline QMDs reuse their addresses
        // and, kept until a drain, overflowed the table a few times a second (every MME DMA
        // read then drained). Off by default: with it title 1 issued ~2x the compute dispatches a draw
        // (it adapts its work to what it measures?) and lost 10 fps. EDEN_NX2_WRITERS_DEDUP=1 (=a).
        static const char dedup_mode = Tegra::Nx2EnvMode("EDEN_NX2_WRITERS_DEDUP");
        if (known == WriteKnown::Writers && Tegra::Nx2Switch(dedup_mode, false)) {
            for (size_t i = 0; i < pending_write_count; ++i) {
                const auto& write = pending_writes[i];
                if (write.known == WriteKnown::Writers && write.begin == address &&
                    write.end == address + size) {
                    return;
                }
            }
        }
        const u64 position = known == WriteKnown::Position ? SentPosition() : 0;
        // EDEN_NX2_MT_LITE_OVERLAY: a wider write (a 16-byte release: payload + timestamp) keeps
        // the value of its first word; KnownPendingWords refuses reads past it.
        const bool has_value =
            value.has_value() && (size == 4 || (size > 4 && VideoCommon::MT::LiteOverlayFirst()));
        // EDEN_NX2_C10_ACQ_SEQ: notes not known by position get a sequence number (see
        // C10NextNoteSeq): the ring position by which their trigger was sent.
        PushPendingWrite({address, address + size, position, known, tag, value.value_or(0),
                          has_value, static_cast<u8>(size == 16 ? flags : 0),
                          known == WriteKnown::Position ? u64{0} : C10NextNoteSeq()});
    }

    /// EDEN_NX2_MT_LITE_I2MPOS (D4): a 3D inline upload (launch_dma) starts. Its destination is
    /// written as the back runs the data words that follow (each entry of them rewrites it), so
    /// the note stays open: NoteI2mData moves its position past each data entry sent, drains keep
    /// it (data may still follow), and the next upload closes it (into the table).
    void OpenI2mWrite(GPUVAddr address, u64 size, WriteKnown known) {
        CloseI2mWrite();
        open_i2m = {address, address + size, known == WriteKnown::Position ? SentPosition() : 0,
                    known, VideoCommon::MT::TagI2m, 0, false, 0,
                    known == WriteKnown::Position ? u64{0} : C10NextNoteSeq()};
        open_i2m_active = true;
    }
    /// D4: inline_data words were just sent to the back (one method or a multi-method run).
    void NoteI2mData() {
        if (open_i2m_active && open_i2m.known == WriteKnown::Position) {
            open_i2m.position = SentPosition();
        }
    }

    /// What a read of the range must wait for. `drain`: everything sent; else `writers` and
    /// `position` (0: nothing). `settle_position`: the newest of those positions whose write the
    /// committer makes (EDEN_NX2_MT_LITE_SEQPOS; else == position): 0, only the sequencer.
    struct ReadWait {
        bool drain;
        bool writers;
        u64 position;
        u64 settle_position{};
        u8 newest_tag{};       ///< Tag of the write at `position` (stats)
        bool newest_direct{};  ///< ... a WriteDirect64 one
        bool open_i2m{};       ///< An open I2M note overlaps (D4)
    };
    [[nodiscard]] ReadWait PendingWritesFor(GPUVAddr address, u64 size) const {
        ReadWait wait{pending_write_overflow, false, 0};
        const bool seq_pos = VideoCommon::MT::Lite6SeqPosition();
        const auto add = [&](const PendingWrite& write) {
            if (!(address < write.end && write.begin < address + size)) {
                return;
            }
            switch (write.known) {
            case WriteKnown::Position:
                if (write.position >= wait.position) {
                    wait.newest_tag = write.tag;
                    wait.newest_direct = (write.flags & WriteDirect64) != 0;
                }
                wait.position = (std::max)(wait.position, write.position);
                if (!seq_pos || (write.flags & WriteDirect64) == 0) {
                    wait.settle_position = (std::max)(wait.settle_position, write.position);
                }
                break;
            case WriteKnown::Writers:
                wait.writers = true;
                break;
            case WriteKnown::Drain:
                wait.drain = true;
                break;
            }
        };
        for (size_t i = 0; i < pending_write_count && !wait.drain; ++i) {
            add(pending_writes[i]);
        }
        if (open_i2m_active && !wait.drain) {
            wait.open_i2m = address < open_i2m.end && open_i2m.begin < address + size;
            add(open_i2m);
        }
        return wait;
    }
    /// For a read of [address, address + size): the words that writes the back hasn't run yet
    /// will hold, in the order they were noted, if all of those writes have a known value.
    /// Returns false when one doesn't (the read must wait for it). Writes the back has run
    /// are left to memory, which may have been written again since. `refusal` (stats): 1 a write
    /// without a value, 2 the read reaches past a wider write's known words.
    struct KnownWord {
        GPUVAddr address;
        u32 value;
    };
    /// EDEN_NX2_C11_POSFWD (F1b): `front_ticks` set, a direct 16-byte payload release also lays
    /// words 2-3 (the timestamp the sequencer writes) with that value.
    [[nodiscard]] bool KnownPendingWords(GPUVAddr address, u64 size, std::vector<KnownWord>& out,
                                         u8* refusal = nullptr,
                                         std::optional<u64> front_ticks = std::nullopt) {
        out.clear();
        if (pending_write_overflow) {
            return false;
        }
        const u64 done = SettledPosition();
        const bool overlay64 = VideoCommon::MT::Lite6Overlay64();
        const auto refuse = [&](u8 why) {
            if (refusal != nullptr) {
                *refusal = why;
            }
            return false;
        };
        if (open_i2m_active && address < open_i2m.end && open_i2m.begin < address + size &&
            (open_i2m.known != WriteKnown::Position || open_i2m.position > done)) {
            return refuse(1);
        }
        for (size_t i = 0; i < pending_write_count; ++i) {
            const auto& write = pending_writes[i];
            if (!(address < write.end && write.begin < address + size)) {
                continue;
            }
            if (write.known == WriteKnown::Position && write.position <= done) {
                continue;
            }
            if (!write.has_value || write.known != WriteKnown::Position) {
                return refuse(1);
            }
            // EDEN_NX2_MT_LITE_OVERLAY64 (D2): a direct 16-byte payload release also writes word
            // 1 (0, the payload's high half).
            if (overlay64 && (write.flags & WriteDirect64) != 0) {
                if (address + size > write.begin + 8) {
                    if (!front_ticks || address + size > write.begin + 16) {
                        return refuse(2);
                    }
                    out.push_back({write.begin, write.value});
                    out.push_back({write.begin + 4, 0});
                    out.push_back({write.begin + 8, static_cast<u32>(*front_ticks)});
                    out.push_back({write.begin + 12, static_cast<u32>(*front_ticks >> 32)});
                    continue;
                }
                out.push_back({write.begin, write.value});
                out.push_back({write.begin + 4, 0});
                continue;
            }
            // Only the first word of a wider write is known: the read must not reach past it.
            if (write.end - write.begin > 4 && address + size > write.begin + 4) {
                return refuse(2);
            }
            out.push_back({write.begin, write.value});
        }
        return true;
    }
    /// EDEN_NX2_C12_POSADJ (cycle 12 W1 P1, mt/c12_front.h): KnownPendingWords with a per-byte
    /// rule. A noted write only refuses the read when the read covers bytes OF THAT WRITE whose
    /// value the front does not know; bytes of the read past its end are other notes' (laid in
    /// note order) or memory's (no queued note over them: the plain no-wait read's rule, the
    /// caller has refused writers, drain-known notes, overflow and GPU-written pages). A direct
    /// 16-byte payload release (OVERLAY64) lays all 16 bytes: payload, 0, `front_ticks` (the
    /// sequencer writes its own GetTicks(), later). `ts_mask` (optional): bit i = read word i
    /// (i < 64) is such a timestamp word (C12_POSADJ_CHECK splits its mismatches). `refusal`: 1
    /// a write without a value / not known by position / open I2M, 2 a wider write whose other
    /// bytes the read covers.
    [[nodiscard]] bool C12KnownPendingWords(GPUVAddr address, u64 size,
                                            std::vector<KnownWord>& out, u8* refusal,
                                            u64 front_ticks, u64* ts_mask = nullptr) {
        out.clear();
        if (ts_mask != nullptr) {
            *ts_mask = 0;
        }
        if (pending_write_overflow) {
            return false;
        }
        const u64 done = SettledPosition();
        const bool overlay64 = VideoCommon::MT::Lite6Overlay64();
        const GPUVAddr read_end = address + size;
        const auto refuse = [&](u8 why) {
            if (refusal != nullptr) {
                *refusal = why;
            }
            return false;
        };
        if (open_i2m_active && address < open_i2m.end && open_i2m.begin < read_end &&
            (open_i2m.known != WriteKnown::Position || open_i2m.position > done)) {
            return refuse(1);
        }
        const auto mark_ts = [&](GPUVAddr word) {
            if (ts_mask == nullptr || word < address || ((word - address) & 3) != 0) {
                return;
            }
            const u64 index = (word - address) / 4;
            if (index < 64 && word < read_end) {
                *ts_mask |= u64{1} << index;
            }
        };
        for (size_t i = 0; i < pending_write_count; ++i) {
            const auto& write = pending_writes[i];
            if (!(address < write.end && write.begin < read_end)) {
                continue;
            }
            if (write.known == WriteKnown::Position && write.position <= done) {
                continue;
            }
            if (!write.has_value || write.known != WriteKnown::Position) {
                return refuse(1);
            }
            const bool direct = overlay64 && (write.flags & WriteDirect64) != 0 &&
                                write.end - write.begin == 16;
            const GPUVAddr known_end = (std::min)(write.end, write.begin + (direct ? 16 : 4));
            // The read covers bytes of this write past its known words: their value is unknown.
            if (known_end < write.end && known_end < read_end && address < write.end) {
                return refuse(2);
            }
            out.push_back({write.begin, write.value});
            if (direct) {
                out.push_back({write.begin + 4, 0});
                out.push_back({write.begin + 8, static_cast<u32>(front_ticks)});
                out.push_back({write.begin + 12, static_cast<u32>(front_ticks >> 32)});
                mark_ts(write.begin + 8);
                mark_ts(write.begin + 12);
            }
        }
        return true;
    }
    /// P1 stats: for a read the cycle 11 rule refuses as "past known", who writes the bytes past
    /// the end of the first queued write it reaches beyond (cls 0: none such; 1 a queued note,
    /// 2 a note the back already ran, 3 no note: guest memory as it is).
    struct C12PastCensus {
        u8 cls;
        u8 tag;
        bool direct;
        bool same_channel;
        bool adjacent; ///< The covering note starts where the first write ends
    };
    [[nodiscard]] C12PastCensus C12CensusPast(GPUVAddr address, u64 size) const {
        const u64 done = SettledPosition();
        const GPUVAddr read_end = address + size;
        const auto queued = [done](const PendingWrite& write) {
            return write.known != WriteKnown::Position || write.position > done;
        };
        size_t first = pending_write_count;
        for (size_t i = 0; i < pending_write_count; ++i) {
            const auto& write = pending_writes[i];
            if (address < write.end && write.begin < read_end && queued(write) &&
                read_end > write.end) {
                first = i;
                break;
            }
        }
        if (first == pending_write_count) {
            return C12PastCensus{0, 0, false, false, false};
        }
        const PendingWrite& head = pending_writes[first];
        const GPUVAddr past = head.end;
        for (size_t i = pending_write_count; i-- > 0;) {
            if (i == first) {
                continue;
            }
            const auto& write = pending_writes[i];
            if (past < write.end && write.begin < read_end) {
                return C12PastCensus{static_cast<u8>(queued(write) ? 1 : 2), write.tag,
                                     (write.flags & WriteDirect64) != 0,
                                     write.channel == head.channel, write.begin == past};
            }
        }
        if (open_i2m_active && past < open_i2m.end && open_i2m.begin < read_end) {
            return C12PastCensus{1, open_i2m.tag, false, false, open_i2m.begin == past};
        }
        return C12PastCensus{3, 0, false, false, false};
    }
    /// P3 census (kepler_compute.cpp QMD mirror): a dispatch of the current call runs `program`.
    void C12NoteDispatchProgram(u64 program) {
        c12_call_program = program;
    }
    /// P3 census: the program of the last writer call (0: not compute, or not mirrored).
    [[nodiscard]] u64 C12LastWriterProgram() const {
        return c12_writer_program;
    }
    /// EDEN_NX2_MT_LITE_PENDACQ (D5/D6): whether every write the front noted as still queued over
    /// the word at `address` has a known value there and `meets` none of them: no table overflow,
    /// no queued add, no writer/drain-known or valueless write, no open I2M note over it. Writes
    /// the back has run are memory's part (read memory after this).
    template <typename Meets>
    [[nodiscard]] bool PendingValuesFail(GPUVAddr address, const Meets& meets) {
        if (pending_write_overflow || PendingAddSum(address) != 0) {
            return false;
        }
        const u64 done = SettledPosition();
        if (open_i2m_active && address < open_i2m.end && open_i2m.begin < address + 4 &&
            (open_i2m.known != WriteKnown::Position || open_i2m.position > done)) {
            return false;
        }
        for (size_t i = 0; i < pending_write_count; ++i) {
            const auto& write = pending_writes[i];
            if (!(address < write.end && write.begin < address + 4)) {
                continue;
            }
            if (write.known == WriteKnown::Position && write.position <= done) {
                continue;
            }
            if (write.known != WriteKnown::Position || !write.has_value) {
                return false;
            }
            u32 value;
            if (write.begin == address) {
                value = write.value;
            } else if ((write.flags & WriteDirect64) != 0 && write.begin + 4 == address) {
                value = 0;
            } else {
                return false;
            }
            if (meets(value)) {
                return false;
            }
        }
        return true;
    }
    /// Tags (bit per tag) of the noted writes overlapping the range.
    [[nodiscard]] u32 PendingWriteTags(GPUVAddr address, u64 size) const {
        u32 tags = 0;
        for (size_t i = 0; i < pending_write_count; ++i) {
            const auto& write = pending_writes[i];
            if (address < write.end && write.begin < address + size) {
                tags |= 1U << write.tag;
            }
        }
        if (open_i2m_active && address < open_i2m.end && open_i2m.begin < address + size) {
            tags |= 1U << open_i2m.tag;
        }
        return tags;
    }
    [[nodiscard]] bool PendingWriteOverflow() const {
        return pending_write_overflow;
    }
    [[nodiscard]] bool OverlapsPendingWrite(GPUVAddr address, u64 size) const {
        const ReadWait wait = PendingWritesFor(address, size);
        return wait.drain || wait.writers || wait.position != 0;
    }
    /// Waits until the back has run everything sent before `position`.
    void WaitForPosition(u64 position, DrainReason reason = DrainReason::Position) {
        WaitForPosition(position, position, reason);
    }
    /// EDEN_NX2_MT_LITE_SEQPOS (D3): the sequencer past `position`, the committer only as far as
    /// `settle_position` needs (0: not at all; ReadWait::settle_position). Returns whether it
    /// found the back behind.
    bool WaitForPosition(u64 position, u64 settle_position, DrainReason reason);
    /// EDEN_NX2_MT_LITE_QMDLEDGER: waits until the sequencer (`consumed`) ran everything sent,
    /// not the committer (without the MT back, the back). Clears no noted write.
    void WaitSequencer(DrainReason reason);

    /// EDEN_NX2_MT_LITE_FOREIGNACQ (front): whether nothing the front noted as queued can still
    /// write the word at `address`: no release or add release pending, no noted write over it
    /// the back has not settled, no table overflow. (Back-only writers are the caller's part.)
    [[nodiscard]] bool NothingPendingFor(GPUVAddr address) {
        if (pending_write_overflow || HasPendingRelease(address)) {
            return false;
        }
        if (const auto it = pending_adds.find(address); it != pending_adds.end()) {
            if (PendingAddSum(address) != 0) {
                return false;
            }
        }
        const ReadWait wait = PendingWritesFor(address, 4);
        if (wait.drain || wait.writers) {
            return false;
        }
        return wait.position == 0 || wait.position <= SettledPosition();
    }

    /// A QMD release that adds `payload` to the word at `address` was just sent to the back.
    u64 noted_adds{}; ///< EDEN_NX2_ACQUIRE_STATS
    GPUVAddr last_add_address{};
    void NoteAddRelease(GPUVAddr address, u32 payload) {
        ++noted_adds;
        last_add_address = address;
        if (PredictAcquires()) {
            FlushState();
            auto& adds = pending_adds[address];
            if (adds.size() < 4096) {
                adds.push_back(
                    {write_position, payload, C10NextNoteSeq(), ++c11_note_order, c11_channel});
            }
        }
    }
    /// What the add releases sent to the back but not yet run there will add at `address`
    /// (the back runs them in order, a fence that writes them at once: memory has the others).
    /// Read memory before calling: an add run in between is then missed, never counted twice.
    [[nodiscard]] u64 PendingAddSum(GPUVAddr address) {
        const auto it = pending_adds.find(address);
        if (it == pending_adds.end()) {
            return 0;
        }
        const u64 done = SettledPosition();
        auto& adds = it->second;
        while (!adds.empty() && adds.front().position < done) {
            adds.pop_front();
        }
        u64 sum = 0;
        for (const auto& add : adds) {
            sum += add.payload;
        }
        if (adds.empty()) {
            pending_adds.erase(it);
        }
        return sum;
    }

    /// Whether a release the front sent may still be on its way to `address`.
    /// `writes_only`: only releases that write their payload count (max releases, which can only
    /// raise the word, are left out: for an acquire memory already meets, they change nothing).
    [[nodiscard]] bool HasPendingRelease(GPUVAddr address, bool writes_only = false) {
        const auto it = pending_releases.find(address);
        if (it == pending_releases.end()) {
            return false;
        }
        if (writes_only && it->second.at_least) {
            return false;
        }
        if (write_position - it->second.position > RingWords) {
            pending_releases.erase(it);
            return false;
        }
        return true;
    }
    std::optional<Prediction> PredictedRelease(GPUVAddr address, u32 current) {
        const auto it = pending_releases.find(address);
        if (it == pending_releases.end()) {
            return std::nullopt;
        }
        const bool landed = it->second.at_least ? current >= it->second.payload
                                                : current == it->second.payload;
        if (landed || write_position - it->second.position > RingWords) {
            pending_releases.erase(it);
            return std::nullopt;
        }
        return Prediction{it->second.payload, it->second.at_least};
    }
    static bool PredictAcquires();
    /// An acquire passed on a predicted release (counted in the drain report).
    void NotePredictedAcquire() {
        ++predicted_acquires;
    }

    /// Compute or copy work was just sent: what can write GPU-written command lists. `kind`: the
    /// engine type (Tegra::Engines::EngineTypes) for EDEN_NX2_LIST_STATS.
    void NoteWriter(u8 kind = 6) {
        last_writer_position = write_position;
        last_writer_kind = kind;
        // EDEN_NX2_C10_*: copy, I2M and 2D writers (anything but compute, kind 1): their guest
        // writes and semaphore releases are not noted with addresses on the front. Sent (and
        // committed by Method) before DmaPusher calls this.
        if (kind != 1) {
            c10_range_writer_position = write_position;
        }
        // Cycle 12 P3 census: the program of this call's last dispatch (0: none mirrored).
        c12_writer_program = kind == 1 ? c12_call_program : 0;
        c12_call_program = 0;
        // Cycle 13 W3 (mt/c13_front.h): census bookkeeping and the L2 early kick.
        if (kind == 1 && VideoCommon::MT::C13FrontTrack()) {
            C13NoteComputeWriter();
        }
        // EDEN_NX2_C11_LISTRANGE (F2): what the front learnt of this call's compute writes.
        if (VideoCommon::MT::C11ListRange()) {
            C11FinishWriterCall(kind);
        }
    }
    /// Waits until the back executed the compute and copy work sent so far (not what came after).
    /// Returns whether any of it was still queued.
    bool WaitForWriters(DrainReason reason = DrainReason::WritersCommands);

    /// EDEN_NX2_LIST_STATS: the ring position WaitForWriters waits for, and the engine type of
    /// the last writer noted (6: none yet).
    [[nodiscard]] u64 WritersTarget() {
        FlushState();
        return std::min(last_writer_position, write_position);
    }
    [[nodiscard]] u8 LastWriterKind() const {
        return last_writer_kind;
    }

    struct ListWait {
        bool waited;    ///< It found the back behind
        bool shortened; ///< The committer was not waited for up to every writer
        bool c10_shortened{}; ///< EDEN_NX2_C10_LIST_PRECISE: not up to L24's bound either
        bool c11_shortened{}; ///< EDEN_NX2_C11_LISTRANGE: the sequencer not up to every writer
    };
    /// EDEN_NX2_MT_LITE_LISTPOS (L24): WaitForWriters before a command list on GPU-written pages,
    /// with the multithreaded back: the sequencer past every writer sent (it marks the pages they
    /// write at capture, and records the command list pages in Tegra::g_nx2_list_writer_slots),
    /// then the committer only past `page_bound()` (1 + the last position a capture wrote one of
    /// the list's pages at, read once `consumed` passed the writers) and its queued guest memory
    /// writes: the copies and write counts Nx2ReadListCopy reads are made when the committer
    /// records those writers, and a page no captured writer wrote holds what guest memory holds.
    ///
    /// EDEN_NX2_C10_LIST_PRECISE (B2; `list_size` != 0): "its queued guest memory writes" become
    /// the bulk ones (DMA copies/clears, downloads: committer_bulk_write_position), the copy/I2M/2D
    /// writers sent (their semaphore releases are not noted: c10_range_writer_position) and the
    /// writes the front noted over [list_address, list_address + list_size) (semaphore/report
    /// words, inline uploads; their positions, which also raise the sequencer target). The
    /// committer's other writes (semaphore/report words and fence callbacks) are all noted by the
    /// front with their addresses (the same argument as EDEN_NX2_MT_LITE_WRITERS for CPU lists);
    /// one over the list's bytes that is not known by position (QMD releases, drain-known) keeps
    /// L24's bound. Invariant: noted writes are only dropped by drains that settled every
    /// committer guest write queued before them (C10NotedWritesBound also covers a bulk-only one).
    ///
    /// EDEN_NX2_C11_LISTRANGE (F2, `list_as`: the list's address space id): the sequencer part
    /// waits only for the writers C11ListSeqTarget keeps (with B2's noted bound).
    template <typename Bound>
    ListWait WaitForListWriters(const Bound& page_bound, GPUVAddr list_address = 0,
                                u64 list_size = 0, u64 list_as = ~u64{0}) {
        ReleaseFrontHold(HoldWait); // W1 GPUPARAM v2 front hold
        if (!MtActive()) {
            return ListWait{WaitForWriters(), false};
        }
        FlushState();
        const u64 l24_target = std::min(last_writer_position, write_position);
        u64 target = l24_target;
        // EDEN_NX2_C11_LISTRANGE (F2): the sequencer only past the writers that may write the
        // list's bytes (seq_target <= target); the committer bounds below keep `target`.
        u64 seq_target = l24_target;
        // B2: front-only state, read before the wait (no note is made or dropped meanwhile).
        const bool precise = list_size != 0 && VideoCommon::MT::C10ListPrecise();
        std::optional<u64> noted;
        if (precise) {
            ++c10_stats.list_calls;
            noted = C10NotedWritesBound(list_address, list_size);
            if (!noted) {
                ++c10_stats.list_kept;
            } else {
                // A noted write over the list's bytes sent after the last writer (a 3D inline
                // upload: not a NoteWriter writer) is waited for too.
                target = std::max(target, std::min(*noted, write_position));
                seq_target = target;
                // Cycle 12 P2 (EDEN_NX2_C12_LISTRANGE): the gate may have turned F2 off.
                if (VideoCommon::MT::C11ListRange() &&
                    VideoCommon::MT::C12ListRangeGate::Get().Use()) {
                    auto& c11 = VideoCommon::MT::C11FrontStats::Get();
                    ++c11.list_calls;
                    const u64 ranged = std::max(
                        C11ListSeqTarget(list_as, list_address, list_size),
                        std::min(*noted, write_position));
                    if (ranged < seq_target) {
                        seq_target = ranged;
                        ++c11.list_shorter;
                    } else {
                        ++c11.list_same;
                    }
                    VideoCommon::MT::C12ListRangeGate::Get().NoteCall(ranged < target);
                }
            }
        }
        // Read once `consumed` reached the target: the sequencer stores the committer write
        // positions (release) before it stores `consumed` (seq_cst) past their entries.
        const auto l24_bound = [&] {
            return std::min(l24_target,
                            std::max<u64>(page_bound(),
                                          committer_write_position.load(std::memory_order_acquire)));
        };
        const auto bound = [&] {
            if (!noted) {
                return l24_bound();
            }
            return std::min(
                target,
                std::max({page_bound(),
                          committer_bulk_write_position.load(std::memory_order_acquire), *noted}));
        };
        // F2: the sequencer was not waited for up to `target` (re-read below once it is past
        // seq_target: a writer F2 skipped may have been captured since and raise page_bound).
        const bool c11 = seq_target < target;
        consumed_cache = consumed.load(std::memory_order_acquire);
        c13_seq_ns = 0;
        c13_settle_ns = 0;
        if (consumed_cache >= seq_target) {
            const u64 settle = bound();
            const u64 now = settled.load(std::memory_order_acquire);
            if (now >= settle) {
                const bool c10 = noted.has_value() && now < l24_bound();
                c10_stats.list_nowait += c10 ? 1 : 0;
                CountWait(DrainReason::WritersCommands, false, 0);
                return ListWait{false, now < target, c10, c11 && consumed_cache < target};
            }
        }
        Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
        const u64 begin = Nx2Profile::Enabled() ? Nx2Profile::Now() : 0;
        // Cycle 13 W3 L1 census: the sequencer and committer parts of the wait.
        const bool c13_time = VideoCommon::MT::C13FrontStatsOn();
        const u64 c13_begin = c13_time ? Nx2Profile::Now() : 0;
        if (consumed_cache < seq_target) {
            wait_target.store(seq_target, std::memory_order_seq_cst);
            Publish();
            WaitConsumed(seq_target);
            C13EndWait(); // was: wait_target.store(~u64{0}); keeps a later kick pending
        }
        const u64 c13_mid = c13_time ? Nx2Profile::Now() : 0;
        const u64 settle = bound();
        TimedWaitSettled(settle);
        if (c13_time) {
            c13_seq_ns = c13_mid - c13_begin;
            c13_settle_ns = Nx2Profile::Now() - c13_mid;
        }
        const u64 now = settled.load(std::memory_order_acquire);
        const bool shortened = now < target;
        const bool c10 = noted.has_value() && now < l24_bound();
        c10_stats.list_short += c10 ? 1 : 0;
        CountWait(DrainReason::WritersCommands, true,
                  Nx2Profile::Enabled() ? Nx2Profile::Now() - begin : 0);
        return ListWait{true, shortened, c10,
                        c11 && consumed.load(std::memory_order_acquire) < target};
    }

    // ---- Cycle 13 W3 "front list writer waits" (mt/c13_front.h) ----
    /// NoteWriter, compute writer call (front): census bookkeeping, and the L2 early kick of a
    /// dispatch of a learned list writer program: published at once, its ring position becomes
    /// the back's wait target (the sequencer Marks there and clears it, LITE_SEQTARGET).
    void C13NoteComputeWriter() {
        c13_writer_calls = c13_calls;
        c13_writer_ns = Nx2Profile::Now();
        c13_writer_position = last_writer_position;
        c13_writer_kicked = false;
        c13_writer_learned = VideoCommon::MT::C13WriterLearn::Get().IsWriter(c12_writer_program);
        if (!c13_writer_learned || !MtActive()) {
            return;
        }
        auto& stats = VideoCommon::MT::C13FrontStats::Get();
        if (!VideoCommon::MT::C13Kick()) {
            ++stats.kicks_off;
            return;
        }
        FlushState();
        const u64 target = write_position;
        c13_writer_kicked = true;
        c13_writer_kick = target;
        c13_kick_target = target;
        c13_kick_back.store(target, std::memory_order_relaxed);
        // The front is not inside a wait here: the target is ~0 or an older kick's.
        wait_target.store(target, std::memory_order_seq_cst);
        Publish();
        ++stats.kicks;
    }
    /// The front's waits, instead of clearing wait_target: a kick sent after the position just
    /// waited for stays the back's target (the sequencer clears it once past it).
    void C13EndWait() {
        if (c13_kick_target != 0) [[unlikely]] {
            if (consumed_cache < c13_kick_target) {
                wait_target.store(c13_kick_target, std::memory_order_seq_cst);
                ++VideoCommon::MT::C13FrontStats::Get().kick_restored;
                return;
            }
            c13_kick_target = 0;
        }
        wait_target.store(~u64{0}, std::memory_order_relaxed);
    }
    /// DmaPusher census: the last compute writer and what was sent since.
    [[nodiscard]] bool C13WriterLearned() const {
        return c13_writer_learned;
    }
    [[nodiscard]] bool C13WriterKicked() const {
        return c13_writer_kicked;
    }
    [[nodiscard]] u64 C13WriterKickTarget() const {
        return c13_writer_kicked ? c13_writer_kick : 0;
    }
    [[nodiscard]] u64 C13CallsSinceWriter() const {
        return c13_calls - c13_writer_calls;
    }
    [[nodiscard]] u64 C13NsSinceWriter() const {
        const u64 now = Nx2Profile::Now();
        return now > c13_writer_ns ? now - c13_writer_ns : 0;
    }
    [[nodiscard]] u64 C13WordsSinceWriter() {
        FlushState();
        return write_position - std::min(c13_writer_position, write_position);
    }
    [[nodiscard]] u64 C13LastSeqNs() const {
        return c13_seq_ns;
    }
    [[nodiscard]] u64 C13LastSettleNs() const {
        return c13_settle_ns;
    }
    /// Sequencer thread (hook in vk_mt_seq.h): the entry it runs is before a pending kick.
    [[nodiscard]] bool C13SeqBeforeKick() const {
        if (!VideoCommon::MT::C13KickSeq()) {
            return false;
        }
        const u64 kick = c13_kick_back.load(std::memory_order_relaxed);
        return kick != 0 && back_exec_position < kick;
    }
    /// Committer thread (hook in vk_mt_pipeline.h): a Mark at `position` is the first one at or
    /// past the pending kick (clears it).
    [[nodiscard]] bool C13KickMark(u64 position) {
        u64 kick = c13_kick_back.load(std::memory_order_relaxed);
        if (kick == 0 || position < kick) {
            return false;
        }
        if (!c13_kick_back.compare_exchange_strong(kick, 0, std::memory_order_relaxed)) {
            return false;
        }
        VideoCommon::MT::g_c13_kick_marks.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // ---- end cycle 13 W3 ----

    // ---- Cycle 10 B1 (EDEN_NX2_C10_ACQ_SEQ, mt/c10_front.h) ----
    /// Front counters for the drains line (EDEN_NX2_GPU_PROFILE).
    VideoCommon::MT::C10FrontStats c10_stats;
    /// DmaPusher, at the start of each method call (front thread): the notes made so far
    /// (releases, adds, writer-known writes) get the ring position by which the method that
    /// triggers each of them was sent. The QMD mirror notes a QMD's releases (FrontApply) BEFORE
    /// the method that launches or signals it is sent (or while the macro that writes it gathers
    /// its writes in the list): their `position` is too early. Invariant: every note made during
    /// a DmaPusher call has its trigger sent, or gathered in the state/list buffers that this
    /// flushes, by the time the next call starts; a note not resolved yet counts as everything
    /// sent (C10NoteBound).
    void C10ResolveNotes() {
        if (c10_note_seq != c10_resolved_seq) [[unlikely]] {
            C10ResolveNotesSlow();
        }
    }
    struct C10Target {
        bool ok;       ///< false: some noted write over the word has no usable position
        u64 position;  ///< The back must be past this before memory answers for the word
    };
    /// B1: how far the back must have run before the word at `address` holds what everything
    /// sent so far leaves there, as far as the front noted writes to it (pending release(s),
    /// adds, noted writes, the open I2M note) plus the copy/I2M/2D writers sent so far (whose
    /// releases and writes are not noted). Compute shader writes are the caller's part (pages
    /// GPU work wrote are refused).
    [[nodiscard]] C10Target C10AcquireTarget(GPUVAddr address);
    /// B1: waits for the sequencer past `target` and the committer past its guest writes queued
    /// before it (ReadSettleTarget, puller reason: EDEN_NX2_MT_LITE_DRAIN). Counted as a puller
    /// wait. Clears no note. Returns whether the back was behind.
    bool C10WaitAcquireTarget(u64 target);
    // ---- end cycle 10 B1 ----

    // ---- Cycle 11 W1 "front forwarding" (mt/c11_front.h), front thread only ----
    /// DmaPusher::DispatchCalls: the channel whose commands the front runs now (the release and
    /// add notes carry it; F1 refuses words another channel's queued releases target).
    void C11SetChannel(s32 channel) {
        c11_channel = channel;
    }
    [[nodiscard]] s32 C11Channel() const {
        return c11_channel;
    }
    /// F1 (EDEN_NX2_C11_ACQFWD): what the word at `address` holds once the back ran everything
    /// sent so far, as far as the front noted it: the last release noted for it plus the QMD
    /// add releases noted after that release. Refused when an un-noted or valueless writer may
    /// change it (C11FrontStats::Refusal).
    struct C11AcqPrediction {
        u32 refusal; ///< VideoCommon::MT::C11FrontStats::Refusal; RefNone: `lower` holds
        u64 lower;   ///< The word holds at least this (release payload + adds after it)
        bool exact;  ///< ... exactly this (plain release, unsettled, no add queued before it)
    };
    [[nodiscard]] C11AcqPrediction C11PredictAcquire(GPUVAddr address);

    /// F1b (EDEN_NX2_C11_POSFWD stats): the noted write that makes KnownPendingWords refuse a
    /// read of [address, address + size) as "past known words" (found: false, none).
    struct C11PastKnown {
        bool found;
        GPUVAddr begin;
        u64 size;
        bool direct;
    };
    [[nodiscard]] C11PastKnown C11FindPastKnown(GPUVAddr address, u64 size);

    /// F2 (EDEN_NX2_C11_LISTRANGE). DmaPusher, at the start of each method call: forget what an
    /// earlier call that was no writer described.
    void C11BeginCall() {
        ++c13_calls; // Cycle 13 W3 census: method calls between a writer and its list fetch
        if (c11_call_dirty) [[unlikely]] {
            c11_call = {};
            c11_call_dirty = false;
        }
    }
    /// The QMD mirror handled a launch method of this call: its dispatches are described below.
    void C11NoteComputeDescribed() {
        c11_call.described = true;
        c11_call_dirty = true;
    }
    /// A dispatch of this call writes [begin, end) of address space `as` (GPU addresses).
    void C11NoteComputeRange(u64 as, GPUVAddr begin, GPUVAddr end) {
        c11_call_dirty = true;
        if (c11_call.count == c11_call.ranges.size()) {
            c11_call.unknown = true;
            return;
        }
        c11_call.ranges[c11_call.count++] = C11Range{as, begin, end};
    }
    /// A write of this call the front cannot range (a dispatch of an unknown program, a compute
    /// method that writes memory and is no QMD launch, a mirror error...).
    void C11NoteComputeUnknown() {
        c11_call.unknown = true;
        c11_call_dirty = true;
    }
    /// A 3D constant buffer data write (CB_DATA) into [begin, begin + size) of address space
    /// `as` was just sent (or gathered). `multi`: from the multi-method path (its hook ran).
    void C11NoteCbData(u64 as, GPUVAddr begin, u64 size, bool multi) {
        if (!VideoCommon::MT::C11ListRange()) {
            return;
        }
        c11_cb_hooked |= multi;
        const u64 position = C11PositionNow();
        if (c11_cb_count != 0) {
            C11CbWrite& last = c11_cb[(c11_cb_count - 1) % c11_cb.size()];
            if (last.as == as && last.begin == begin && last.end == begin + size) {
                last.position = position;
                return;
            }
        }
        if (c11_cb_count >= c11_cb.size()) {
            const C11CbWrite& dropped = c11_cb[c11_cb_count % c11_cb.size()];
            c11_cb_unranged_position = std::max(c11_cb_unranged_position, dropped.position);
        }
        c11_cb[c11_cb_count % c11_cb.size()] = C11CbWrite{as, begin, begin + size, position};
        ++c11_cb_count;
    }
    /// A 3D constant buffer data run whose buffer the front did not see (the multi-method path
    /// without its hook): every constant buffer read counts as stale until the back ran it.
    void C11NoteCbUnranged() {
        c11_cb_unranged_position = std::max(c11_cb_unranged_position, C11PositionNow());
    }
    [[nodiscard]] bool C11CbHooked() const {
        return c11_cb_hooked;
    }
    /// Whether memory may not yet hold what the back will read at [begin, begin + size) (a
    /// dispatch's constant buffer words): a queued 3D CB_DATA write, noted write, copy/I2M/2D
    /// writer, compute writer that may write it, not yet run.
    [[nodiscard]] bool C11CbufStale(u64 as, GPUVAddr begin, u64 size);
    /// F2: the ring position the sequencer must pass before a list at [address, address + size)
    /// of address space `as` holds what the writers sent so far leave there: the copy/I2M/2D
    /// writers, the compute writers the front could not describe and the described ones whose
    /// ranges overlap (or are in another address space). Call after FlushState.
    [[nodiscard]] u64 C11ListSeqTarget(u64 as, GPUVAddr address, u64 size);
    // ---- end cycle 11 W1 ----

private:
    /// EDEN_NX2_GPU_PROFILE: counts a front wait (`waited`: it found the back behind) and logs
    /// the counts by reason every 10 s ("GPU split drains per second:").
    void CountWait(DrainReason reason, bool waited, u64 wait_ns);
    std::array<u64, static_cast<size_t>(DrainReason::Count)> stat_waits{}, stat_wait_ns{},
        stat_calls{}, stat_settle_ns{};
    /// EDEN_NX2_GPU_PROFILE: time the current wait spent in WaitSettled (the committer's part),
    /// added to its reason by CountWait.
    u64 settle_wait_ns{};
    /// WaitSettled, timed for the drain report.
    void TimedWaitSettled(u64 target);
    u64 stat_window_start{};

    // ---- Cycle 10 (mt/c10_front.h), front thread only ----
    /// EDEN_NX2_C10_ACQ_SEQ: sequence number for a note whose trigger may still be unsent
    /// (C10ResolveNotes); 0 with the switch off (nothing is ever resolved then).
    u64 C10NextNoteSeq() {
        return VideoCommon::MT::C10AcqSeq() ? ++c10_note_seq : 0;
    }
    void C10ResolveNotesSlow();
    /// The ring position by which the trigger of note `seq` had been sent (an upper bound:
    /// resolutions dropped from the table answer with the oldest one kept; unresolved, or 0,
    /// with everything sent). Call after FlushState.
    [[nodiscard]] u64 C10NoteBound(u64 seq) const {
        if (seq == 0 || seq > c10_resolved_seq) {
            return write_position;
        }
        const u64 kept = std::min<u64>(c10_resolution_count, c10_resolutions.size());
        for (u64 i = c10_resolution_count - kept; i < c10_resolution_count; ++i) {
            const C10Resolution& resolution = c10_resolutions[i % c10_resolutions.size()];
            if (resolution.seq >= seq) {
                return resolution.position;
            }
        }
        return write_position;
    }
    /// EDEN_NX2_C10_LIST_PRECISE: the newest position of the writes noted over the range and of
    /// the copy/I2M/2D writers (nullopt: a note over it is not known by position, or the table
    /// overflowed). Also covers committer writes behind notes a bulk-only drain dropped.
    [[nodiscard]] std::optional<u64> C10NotedWritesBound(GPUVAddr address, u64 size) const {
        // The QMD mirror notes the QMD release words (committer fence callbacks / queries) only
        // when acquires are predicted: without it they are not all noted.
        if (pending_write_overflow || !PredictAcquires()) {
            return std::nullopt;
        }
        u64 bound = std::max(c10_range_writer_position, c10_bulk_clear_position);
        for (size_t i = 0; i < pending_write_count; ++i) {
            const auto& write = pending_writes[i];
            if (!(address < write.end && write.begin < address + size)) {
                continue;
            }
            if (write.known != WriteKnown::Position) {
                return std::nullopt;
            }
            bound = std::max(bound, write.position);
        }
        if (open_i2m_active && address < open_i2m.end && open_i2m.begin < address + size) {
            if (open_i2m.known != WriteKnown::Position) {
                return std::nullopt;
            }
            bound = std::max(bound, open_i2m.position);
        }
        return bound;
    }
    u64 c10_note_seq{};
    u64 c10_resolved_seq{};
    struct C10Resolution {
        u64 seq;      ///< Notes up to this sequence number ...
        u64 position; ///< ... had their triggers sent before this ring position
    };
    std::array<C10Resolution, 256> c10_resolutions{};
    u64 c10_resolution_count{};
    /// write_position when the last copy/I2M/2D writer (not compute) was noted (NoteWriter).
    u64 c10_range_writer_position{};
    /// write_position at the last drain that dropped the notes while settling only the bulk
    /// committer writes (EDEN_NX2_MT_LITE_WRITERS reasons; none today, kept for safety).
    u64 c10_bulk_clear_position{};

    // ---- Cycle 12 W1 (mt/c12_front.h), front thread only ----
    u64 c12_call_program{};   ///< P3: program of the current call's last mirrored dispatch
    u64 c12_writer_program{}; ///< P3: ... of the last writer call (0: not compute / unknown)

    // ---- Cycle 13 W3 (mt/c13_front.h), front thread only unless noted ----
    u64 c13_calls{};          ///< DmaPusher method calls (C11BeginCall)
    u64 c13_writer_calls{};   ///< c13_calls at the last compute writer
    u64 c13_writer_ns{};      ///< Nx2Profile::Now() at the last compute writer
    u64 c13_writer_position{};///< write_position at the last compute writer
    bool c13_writer_learned{};///< Its program was a learned list writer
    bool c13_writer_kicked{}; ///< ... and it was kicked
    u64 c13_writer_kick{};    ///< ... at this ring position (everything sent with it)
    u64 c13_kick_target{};    ///< Pending kick (0: none): the wait target the front set
    u64 c13_seq_ns{};         ///< Last WaitForListWriters: WaitConsumed part
    u64 c13_settle_ns{};      ///< ... TimedWaitSettled part
    /// Last kick target for the back hooks (sequencer reads, committer clears; 0: none).
    std::atomic<u64> c13_kick_back{};

    // ---- Cycle 11 W1 (mt/c11_front.h), front thread only ----
    s32 c11_channel{-1};
    u64 c11_note_order{};
    /// A ring position not known yet (the entry is still gathered: FlushMacroList resolves it).
    static constexpr u64 C11Unresolved = ~u64{0};
    [[nodiscard]] u64 C11PositionNow() {
        if (list_count == 0 && state_count == 0) {
            return write_position;
        }
        c11_unresolved = true;
        return C11Unresolved;
    }
    /// FlushMacroList / FlushStateSlow: gathered entries went out; positions noted meanwhile
    /// become the position after them.
    void C11ResolvePositions();
    void C11FinishWriterCall(u8 kind);
    bool c11_unresolved{};
    struct C11Range {
        u64 as;
        GPUVAddr begin;
        GPUVAddr end;
    };
    struct C11Call {
        bool described{};
        bool unknown{};
        u32 count{};
        std::array<C11Range, 8> ranges{};
    };
    C11Call c11_call{};
    bool c11_call_dirty{};
    struct C11Writer {
        u64 position;
        u32 count;
        std::array<C11Range, 8> ranges;
    };
    std::array<C11Writer, 256> c11_writers{};
    u64 c11_writer_count{};
    /// Newest position of a compute writer whose writes the front could not range.
    u64 c11_unknown_compute_position{};
    struct C11CbWrite {
        u64 as;
        GPUVAddr begin;
        GPUVAddr end;
        u64 position;
    };
    std::array<C11CbWrite, 64> c11_cb{};
    u64 c11_cb_count{};
    u64 c11_cb_unranged_position{};
    bool c11_cb_hooked{};

    enum class Kind : u64 {
        Wrap,   ///< The rest of the ring is unused: continue at its start
        Engine, ///< The engine the next methods go to: word 1
        Method, ///< Bits 8-23 method, 32-63 argument; flags IsLast, HasSegment (word 1)
        Multi,  ///< Bits 8-23 method, 32-63 amount; word 1 methods pending, [segment], words
        Call,   ///< Bits 8-31 payload size in words; word 1 thunk, then the payload
        State,  ///< Bits 32-63 count; flag IsLast: the shadow RAM tracks; then (method | value << 32)
        List,   ///< Bits 32-63 count; then (method | Mergeable | value << 32): macro writes (Direct)
    };
    static constexpr u64 Mergeable = u64{1} << 16; ///< List: consecutive ones go as one multi-method
    static constexpr u64 IsLast = 1 << 4;
    static constexpr u64 HasSegment = 1 << 5;
    static constexpr u64 Direct = 1 << 6; ///< Method: call it even if not executable (macros)

    static constexpr size_t RingWords = size_t{1} << 20; // 8 MiB
    static constexpr size_t RingMask = RingWords - 1;
    static constexpr u32 MaxMultiWords = 8192;
    static constexpr size_t PublishEvery = 256;

    template <typename Func>
    static void CallThunk(const u64* payload) {
        (*reinterpret_cast<const Func*>(payload))();
    }

    /// Room for an entry of `words` at the write position (never across the ring's end).
    u64* Reserve(size_t words) {
        const size_t index = write_position & RingMask;
        if (index + words <= RingWords && write_position + words - consumed_cache <= RingWords)
            [[likely]] {
            return &ring[index];
        }
        return ReserveSlow(words);
    }
    u64* ReserveSlow(size_t words);
    void Commit(size_t words) {
        write_position += words;
        if (write_position - published_local >= publish_every) {
            Publish();
        }
    }
    void WaitForSpace(size_t words);
    void WaitConsumed(u64 target);
    /// WaitConsumed's twin over `settled` (multithreaded back).
    void WaitSettled(u64 target);
    void StoreConsumed(u64 position);
    void SelectEngine(Tegra::Engines::EngineInterface* engine) {
        if (engine != front_engine) [[unlikely]] {
            SwitchEngine(engine);
        }
    }
    void SwitchEngine(Tegra::Engines::EngineInterface* engine);
    void FlushStateSlow();

    void BackLoop(std::stop_token stop_token);
    size_t Execute(const u64* entry);

    Core::System* system{};
    bool active{};
    bool front_macros{};
    std::unique_ptr<u64[]> ring;

    // Front side.
    u64 write_position{};
    u64 last_writer_position{};
    u8 last_writer_kind{6};
    std::array<u64, 2048> list{};
    size_t list_count{};
    Tegra::Engines::EngineInterface* list_engine{};
    struct PendingWrite {
        GPUVAddr begin;
        GPUVAddr end;
        u64 position;
        WriteKnown known;
        u8 tag; ///< Who noted it (VideoCommon::MT::Lite6WriteTag; EDEN_NX2_DMA_READ_WHY)
        u32 value;      ///< The word it writes, when has_value.
        bool has_value; ///< A 4-byte write whose value the front knows (a fence release).
        u8 flags;       ///< WriteDirect64
        u64 seq;        ///< EDEN_NX2_C10_ACQ_SEQ: note sequence number (0: known by position)
        s32 channel{-1}; ///< Cycle 12 P1 stats: the channel noting it (PushPendingWrite sets it)
    };
    std::array<PendingWrite, 256> pending_writes{};
    size_t pending_write_count{};
    bool pending_write_overflow{};
    /// EDEN_NX2_MT_LITE_I2MPOS: the 3D inline upload whose data words may still come.
    PendingWrite open_i2m{};
    bool open_i2m_active{};
    void PushPendingWrite(const PendingWrite& write) {
        if (pending_write_count == pending_writes.size()) {
            // Drop the ones the back has run.
            const u64 done = SettledPosition();
            size_t kept = 0;
            for (size_t i = 0; i < pending_write_count; ++i) {
                if (pending_writes[i].known != WriteKnown::Position ||
                    pending_writes[i].position > done) {
                    pending_writes[kept++] = pending_writes[i];
                }
            }
            pending_write_count = kept;
            if (pending_write_count == pending_writes.size()) {
                pending_write_overflow = true;
                return;
            }
        }
        pending_writes[pending_write_count] = write;
        pending_writes[pending_write_count].channel = c11_channel; // C12 P1 census
        ++pending_write_count;
    }
    /// D4, at a drain (everything sent has run): an open note known only by drains becomes known
    /// by position (the data words sent from now on move it).
    void DrainedOpenI2m() {
        if (open_i2m_active && open_i2m.known == WriteKnown::Drain) {
            open_i2m.known = WriteKnown::Position;
            open_i2m.position = write_position;
        }
    }
    /// D4: the open I2M note goes into the table (no more data for it), unless already run.
    void CloseI2mWrite() {
        if (!open_i2m_active) {
            return;
        }
        open_i2m_active = false;
        if (open_i2m.known == WriteKnown::Position && open_i2m.position <= SettledPosition()) {
            return;
        }
        PushPendingWrite(open_i2m);
    }
    u64 published_local{};
    u64 consumed_cache{};
    /// Words between publishes: small right after a drain, while the back has nothing to do.
    size_t publish_every{PublishEvery};
    Tegra::Engines::EngineInterface* front_engine{};

    struct PendingRelease {
        u32 payload;
        u64 position;
        bool at_least;
        u64 seq; ///< EDEN_NX2_C10_ACQ_SEQ (C10NextNoteSeq; 0: switch off)
        u64 order;   ///< EDEN_NX2_C11_ACQFWD: note order among releases and adds
        s32 channel; ///< EDEN_NX2_C11_ACQFWD: the channel whose commands noted it (-1: unknown)
    };
    std::unordered_map<GPUVAddr, PendingRelease> pending_releases;
    struct PendingAdd {
        u64 position;
        u32 payload;
        u64 seq; ///< EDEN_NX2_C10_ACQ_SEQ (C10NextNoteSeq; 0: switch off)
        u64 order;   ///< EDEN_NX2_C11_ACQFWD (as PendingRelease::order)
        s32 channel; ///< EDEN_NX2_C11_ACQFWD
    };
    std::unordered_map<GPUVAddr, std::deque<PendingAdd>> pending_adds;
    u64 predicted_acquires{};

    // Front side: the state block being gathered (StateWrite).
    static constexpr size_t MaxStateRegisters = 0x1000;
    bool state_blocks{};
    bool state_alternate{};
    std::chrono::steady_clock::time_point state_toggled{};
    bool state_track{};
    Tegra::Engines::EngineInterface* state_engine{};
    size_t state_count{};
    u32 state_current{1};
    std::array<u64, MaxStateRegisters> state_pairs{};
    std::array<u16, MaxStateRegisters> state_slot{};
    std::array<u32, MaxStateRegisters> state_generation{};

    // Back side.
    Tegra::Engines::EngineInterface* back_engine{};

    /// Whether the back is done with the Vulkan scheduler until it gets more work: set after its
    /// idle hook ran, cleared before it executes entries.
    [[nodiscard]] bool BackQuiet() const {
        if (!back_running.load(std::memory_order_acquire)) {
            return true;
        }
        return back_quiet.load(std::memory_order_acquire) && (!MtActive() || mt_back->Quiet());
    }

    static GpuSplit instance;
    static inline thread_local bool on_back_thread{};
    static inline thread_local bool on_gpu_thread{};
    static inline thread_local bool on_committer_thread{};
    MtBackInterface* mt_back{};
    std::atomic<bool> mt_active{};
    alignas(128) std::atomic<u64> settled{};
    /// EDEN_NX2_MT_DMA_CONSUMED: 1 + the ring position of the last entry that queued a committer
    /// guest write (sequencer stores it before it stores `consumed` past that entry).
    alignas(128) std::atomic<u64> committer_write_position{};
    /// EDEN_NX2_MT_LITE_WRITERS: as committer_write_position, for the bulk writes only (no
    /// semaphore/report words, no fences). Stored before committer_write_position.
    std::atomic<u64> committer_bulk_write_position{};
    /// Sequencer-owned: ring position of the entry Execute runs.
    u64 back_exec_position{};
    /// Sequencer-owned: the entry Execute runs queued a committer guest write (NoteCommitterWrite).
    bool exec_wrote_committer{};
    /// The `settled` position a front read must wait for once `consumed` reached `target`.
    [[nodiscard]] u64 ReadSettleTarget(u64 target, DrainReason reason) const;
    /// EDEN_NX2_MT_LITE_DRAIN: the reasons whose waits only need the sequencer and the
    /// committer's guest memory writes (semaphore/syncpoint reads, fence signals, reports).
    [[nodiscard]] static bool LiteDrainReason(DrainReason reason) {
        return VideoCommon::MT::LiteDrain() &&
               (reason == DrainReason::Puller || reason == DrainReason::Sync ||
                reason == DrainReason::Position);
    }
    /// EDEN_NX2_MT_LITE_WRITERS: the writer waits whose reads the front noted no write for: they
    /// only need the committer's bulk writes (committer_bulk_write_position).
    [[nodiscard]] static bool LiteWritersReason(DrainReason reason) {
        return VideoCommon::MT::LiteWritersSettle() &&
               (reason == DrainReason::WritersCpuCommands ||
                reason == DrainReason::WritersDmaRead);
    }
    /// DrainForRead's wait: `consumed` past everything sent, `settled` past ReadSettleTarget.
    void DrainToSettleTarget(DrainReason reason);

public:
    /// Set by the committer while its thread runs.
    std::atomic<bool> committer_running{};

private:
    void (*idle_hook)(void*){};
    void* idle_argument{};
    alignas(64) std::atomic<bool> hook_requested{};
    std::atomic<bool> back_running{};
    std::atomic<bool> back_quiet{true};

    // 128 bytes: x86 spatial prefetchers pull 64-byte lines in pairs. back_waiting and wake are
    // always touched together by both threads, so they share a block.
    alignas(128) std::atomic<u64> published{};
    alignas(128) std::atomic<u64> consumed{};
    /// A position the front waits for (WaitForWriters): the back stores `consumed` once past it.
    alignas(128) std::atomic<u64> wait_target{~u64{0}};
    alignas(128) std::atomic<bool> front_waiting{};
    std::atomic<u32> front_wake{};
    alignas(128) std::atomic<bool> back_waiting{};
    std::atomic<u32> wake{};
    std::jthread back_thread;
};

} // namespace VideoCommon
