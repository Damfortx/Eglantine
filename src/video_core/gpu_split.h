// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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

class GpuSplit {
public:
    static GpuSplit& Get() {
        return instance;
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
        return Get().BackRunning() ? on_back_thread : on_gpu_thread;
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

    /// A write to a register that is not executable (state only), with the shadow RAM already
    /// applied by the front: `value` is what the register will hold, and `track_shadow` whether
    /// the shadow copy takes it too. Writes are gathered until the next entry of another kind and
    /// sent as one block holding each register's last value, so the back applies the state
    /// instead of running every write through the engine (~3 million a second in title 1).
    void StateWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 value,
                    bool track_shadow) {
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
        Count,
    };
    void Drain(DrainReason reason = DrainReason::Other);

    /// Runs `func` on the back and waits for it: for work on back-owned state, such as a buffer
    /// cache download that records into the Vulkan scheduler. A drained back still submits from
    /// its idle hook, so the front never touches that state itself.
    template <typename Func>
    void Run(const Func& func, DrainReason reason = DrainReason::Other) {
        if (on_back_thread || !active) {
            func();
            return;
        }
        Defer(func);
        Drain(reason);
    }

    /// A semaphore release (payload written at `address`) was just sent to the back.
    void NoteRelease(GPUVAddr address, u32 payload, bool at_least = false) {
        if (PredictAcquires()) {
            FlushState();
            pending_releases[address] = PendingRelease{payload, write_position, at_least};
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
    void NotePendingWrite(GPUVAddr address, u64 size, WriteKnown known = WriteKnown::Position,
                          u8 tag = 0, std::optional<u32> value = std::nullopt) {
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
        if (pending_write_count == pending_writes.size()) {
            // Drop the ones the back has run.
            const u64 done = consumed.load(std::memory_order_acquire);
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
        const u64 position = known == WriteKnown::Position ? SentPosition() : 0;
        pending_writes[pending_write_count++] = {address,          address + size, position,
                                                 known,            tag,
                                                 value.value_or(0), value.has_value() && size == 4};
    }
    /// What a read of the range must wait for. `drain`: everything sent; else `writers` and
    /// `position` (0: nothing).
    struct ReadWait {
        bool drain;
        bool writers;
        u64 position;
    };
    [[nodiscard]] ReadWait PendingWritesFor(GPUVAddr address, u64 size) const {
        ReadWait wait{pending_write_overflow, false, 0};
        for (size_t i = 0; i < pending_write_count && !wait.drain; ++i) {
            const auto& write = pending_writes[i];
            if (!(address < write.end && write.begin < address + size)) {
                continue;
            }
            switch (write.known) {
            case WriteKnown::Position:
                wait.position = (std::max)(wait.position, write.position);
                break;
            case WriteKnown::Writers:
                wait.writers = true;
                break;
            case WriteKnown::Drain:
                wait.drain = true;
                break;
            }
        }
        return wait;
    }
    /// For a read of [address, address + size): the words that writes the back hasn't run yet
    /// will hold, in the order they were noted, if all of those writes have a known value.
    /// Returns false when one doesn't (the read must wait for it). Writes the back has run
    /// are left to memory, which may have been written again since.
    struct KnownWord {
        GPUVAddr address;
        u32 value;
    };
    [[nodiscard]] bool KnownPendingWords(GPUVAddr address, u64 size, std::vector<KnownWord>& out) {
        out.clear();
        if (pending_write_overflow) {
            return false;
        }
        const u64 done = consumed.load(std::memory_order_acquire);
        for (size_t i = 0; i < pending_write_count; ++i) {
            const auto& write = pending_writes[i];
            if (!(address < write.end && write.begin < address + size)) {
                continue;
            }
            if (write.known == WriteKnown::Position && write.position <= done) {
                continue;
            }
            if (!write.has_value || write.known != WriteKnown::Position) {
                return false;
            }
            out.push_back({write.begin, write.value});
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
    void WaitForPosition(u64 position, DrainReason reason = DrainReason::Position);

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
                adds.push_back({write_position, payload});
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
        const u64 done = consumed.load(std::memory_order_acquire);
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

    /// Compute or copy work was just sent: what can write GPU-written command lists.
    void NoteWriter() {
        last_writer_position = write_position;
    }
    /// Waits until the back executed the compute and copy work sent so far (not what came after).
    /// Returns whether any of it was still queued.
    bool WaitForWriters(DrainReason reason = DrainReason::WritersCommands);

private:
    /// EDEN_NX2_GPU_PROFILE: counts a front wait (`waited`: it found the back behind) and logs
    /// the counts by reason every 10 s ("GPU split drains per second:").
    void CountWait(DrainReason reason, bool waited, u64 wait_ns);
    std::array<u64, static_cast<size_t>(DrainReason::Count)> stat_waits{}, stat_wait_ns{},
        stat_calls{};
    u64 stat_window_start{};

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
    std::array<u64, 2048> list{};
    size_t list_count{};
    Tegra::Engines::EngineInterface* list_engine{};
    struct PendingWrite {
        GPUVAddr begin;
        GPUVAddr end;
        u64 position;
        WriteKnown known;
        u8 tag; ///< Who noted it (EDEN_NX2_DMA_READ_WHY): 1 I2M, 2 3D report, 0 other.
        u32 value;      ///< The word it writes, when has_value.
        bool has_value; ///< A 4-byte write whose value the front knows (a fence release).
    };
    std::array<PendingWrite, 256> pending_writes{};
    size_t pending_write_count{};
    bool pending_write_overflow{};
    u64 published_local{};
    u64 consumed_cache{};
    /// Words between publishes: small right after a drain, while the back has nothing to do.
    size_t publish_every{PublishEvery};
    Tegra::Engines::EngineInterface* front_engine{};

    struct PendingRelease {
        u32 payload;
        u64 position;
        bool at_least;
    };
    std::unordered_map<GPUVAddr, PendingRelease> pending_releases;
    struct PendingAdd {
        u64 position;
        u32 payload;
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
        return back_quiet.load(std::memory_order_acquire) ||
               !back_running.load(std::memory_order_acquire);
    }

    static GpuSplit instance;
    static inline thread_local bool on_back_thread{};
    static inline thread_local bool on_gpu_thread{};
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
