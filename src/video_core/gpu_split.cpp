// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <fmt/format.h>
#include <cstdlib>
#include <thread>

#ifdef _MSC_VER
#include <intrin.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdio>

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"

namespace VideoCommon {

namespace {

void Pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

[[maybe_unused]] std::atomic<u32> g_back_thread_id{};
std::atomic<bool> g_back_busy{};

} // Anonymous namespace

void GpuSplit::CheckOwner(const char* where) {
#ifdef _WIN32
    if (!g_back_busy.load(std::memory_order_acquire)) {
        return;
    }
    const DWORD thread = GetCurrentThreadId();
    if (thread == g_back_thread_id.load(std::memory_order_relaxed)) {
        return;
    }
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1) >= 12) {
        return;
    }
    std::fprintf(stderr, "GPU split owner check: %s on thread %lu while the back runs\n", where,
                 thread);
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module{};
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module)) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* base_name = std::strrchr(name, '\\');
        std::fprintf(stderr, "  frame %s+%#llx\n", base_name ? base_name + 1 : name,
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                                     reinterpret_cast<uintptr_t>(module)));
    }
    std::fflush(stderr);
#else
    (void)where;
#endif
}

GpuSplit GpuSplit::instance;

void GpuSplit::MaybeStart(Core::System& system_) {
    static const char mode = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT");
        return value != nullptr ? value[0] : '2';
    }();
    const bool wanted = mode == '1' || mode == '2';
    if (!wanted || !Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
        return;
    }
    // EDEN_NX2_SPLIT_STATE=alt: turn the state blocks on and off every 20 s (A/B in one scene).
    if (active && state_alternate) [[unlikely]] {
        const auto now = std::chrono::steady_clock::now();
        if (now - state_toggled >= std::chrono::seconds{20}) {
            FlushState();
            state_blocks = !state_blocks;
            state_toggled = now;
            LOG_INFO(HW_GPU, "GPU split state blocks: {}", state_blocks ? "on" : "off");
        }
    }
    // The front reads command lists straight from guest memory; a read that flushes the caches
    // first (safe DMA, accurate GPU) would touch back-owned state. The settings can change
    // mid-game: hand the engines back to the front (and start again when they allow it).
    const bool safe_reads = Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh()
                                                          : Settings::IsDMALevelSafe();
    const bool allowed = !safe_reads && !Settings::IsGPULevelHigh() &&
                         Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan;
    if (active && !allowed) {
        Drain();
        Stop();
        LOG_INFO(HW_GPU, "GPU thread split: stopped, the accuracy settings need the caches on the front");
        return;
    }
    if (active || !allowed) {
        return;
    }
    system = &system_;
    check_owner = std::getenv("EDEN_NX2_SPLIT_CHECK") != nullptr;
    ring = std::make_unique<u64[]>(RingWords);
    back_thread = std::jthread([this](std::stop_token stop_token) { BackLoop(stop_token); });
    active = true;
#ifdef ARCHITECTURE_x86_64
    // Front macros run compiled (macro_turing_jit.h) only.
    const char* jit = std::getenv("EDEN_NX2_MME_JIT");
    front_macros = mode == '2' && (jit == nullptr || jit[0] != '0');
#endif
    // EDEN_NX2_SPLIT_STATE=1: send state writes as blocks of final values (=alt: switch every
    // 20 s). Off by default: in title 1 it cut the back's entries by ~35% but not its time.
    const char* state = std::getenv("EDEN_NX2_SPLIT_STATE");
    state_blocks = front_macros && state != nullptr && (state[0] == '1' || state[0] == 'a');
    state_alternate = state_blocks && state != nullptr && state[0] == 'a';
    state_toggled = std::chrono::steady_clock::now();
    LOG_INFO(HW_GPU, "GPU thread split: engine methods run on a second thread{}{}",
             front_macros ? ", macros on the first" : "",
             state_blocks ? ", state writes in blocks" : "");
}

bool GpuSplit::PredictAcquires() {
    // EDEN_NX2_PREDICT_ACQUIRE=1 turns it on. Off by default: in title 1's attract demo the
    // front ran no faster with it (~52 fps without, ~48-50 with it).
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PREDICT_ACQUIRE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

void GpuSplit::Stop() {
    if (!back_thread.joinable()) {
        return;
    }
    back_thread.request_stop();
    wake.fetch_add(1, std::memory_order_seq_cst);
    wake.notify_one();
    back_thread.join();
    active = false;
    // The split is static and outlives the emulation session. The next session's back thread
    // starts reading at 0: with the old positions kept it skipped through the new, empty ring a
    // wrap at a time and never reached the old write position (the next game's first GPU work
    // never ran, it waited on its syncpoint forever), and the engines it held were freed.
    ring.reset();
    write_position = 0;
    last_writer_position = 0;
    published_local = 0;
    consumed_cache = 0;
    publish_every = PublishEvery;
    front_engine = nullptr;
    back_engine = nullptr;
    front_macros = false;
    state_blocks = false;
    state_engine = nullptr;
    state_count = 0;
    pending_releases.clear();
    published.store(0, std::memory_order_relaxed);
    consumed.store(0, std::memory_order_relaxed);
    back_waiting.store(false, std::memory_order_relaxed);
    front_waiting.store(false, std::memory_order_relaxed);
}

u64* GpuSplit::ReserveSlow(size_t words) {
    size_t index = write_position & RingMask;
    if (index + words > RingWords) {
        // Entries never wrap: skip to the start of the ring.
        const size_t rest = RingWords - index;
        WaitForSpace(rest);
        ring[index] = static_cast<u64>(Kind::Wrap);
        write_position += rest;
        index = 0;
    }
    WaitForSpace(words);
    return &ring[index];
}

void GpuSplit::WaitForSpace(size_t words) {
    if (write_position + words - consumed_cache <= RingWords) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    WaitConsumed(write_position + words - RingWords);
}

// The front waits for the back to pass `target`: a short spin, then asleep until the back
// stores its position (it wakes the front only when the front sleeps). It spun up to 16,384
// pauses and then yielded in a loop: ~20% of the front's samples in title 1, and a busy hardware
// thread slows its SMT sibling (often the back or a game core). EDEN_NX2_SPLIT_FUTEX=1 sleeps;
// off by default: in title 1's attract demo it cost ~5% (the wake-ups are slower than the spin).
void GpuSplit::WaitConsumed(u64 target) {
    static const bool futex = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT_FUTEX");
        return value != nullptr && value[0] == '1';
    }();
    for (u32 spins = 0; spins < (futex ? 256U : ~0U); ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache >= target) {
            return;
        }
        if ((spins & 1023) == 1023 && !back_running.load(std::memory_order_acquire)) {
            return;
        }
        if (spins < 16384) {
            Pause();
        } else {
            std::this_thread::yield();
        }
    }
    front_waiting.store(true, std::memory_order_seq_cst);
    while (true) {
        const u32 seen = front_wake.load(std::memory_order_seq_cst);
        consumed_cache = consumed.load(std::memory_order_seq_cst);
        if (consumed_cache >= target || !back_running.load(std::memory_order_seq_cst)) {
            break;
        }
        front_wake.wait(seen, std::memory_order_seq_cst);
    }
    front_waiting.store(false, std::memory_order_relaxed);
    consumed_cache = consumed.load(std::memory_order_acquire);
}

void GpuSplit::StoreConsumed(u64 position) {
    consumed.store(position, std::memory_order_seq_cst);
    if (front_waiting.load(std::memory_order_seq_cst)) {
        front_wake.fetch_add(1, std::memory_order_seq_cst);
        front_wake.notify_one();
    }
}

void GpuSplit::Publish() {
    FlushState();
    if (published_local == write_position) {
        return;
    }
    published_local = write_position;
    publish_every = std::min<size_t>(publish_every * 2, PublishEvery);
    published.store(write_position, std::memory_order_seq_cst);
    if (back_waiting.load(std::memory_order_seq_cst)) {
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }
}

void GpuSplit::Drain(DrainReason reason) {
    // Front thread only: waits per reason, logged every 10 s with EDEN_NX2_GPU_PROFILE.
    static std::array<u64, static_cast<size_t>(DrainReason::Count)> waits{}, wait_ns{}, calls{};
    static u64 window_start{};
    const auto index = static_cast<size_t>(reason);
    ++calls[index];
    FlushState();
    if (Nx2Profile::Enabled()) {
        const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
        if (window_start == 0) {
            window_start = now;
        } else if (now - window_start >= 10'000'000'000ULL) {
            static constexpr std::array<const char*, static_cast<size_t>(DrainReason::Count)>
                names{"other", "sync", "gpu-written commands", "front state", "dma read",
                      "verify", "puller"};
            const double seconds = static_cast<double>(now - window_start) / 1e9;
            std::string line;
            for (size_t i = 0; i < names.size(); ++i) {
                if (calls[i] != 0) {
                    line += fmt::format(" | {} {:.0f}/s ({:.0f} waited, {:.1f} ms)", names[i],
                                        static_cast<double>(calls[i]) / seconds,
                                        static_cast<double>(waits[i]) / seconds,
                                        static_cast<double>(wait_ns[i]) / 1e6 / seconds);
                }
            }
            line += fmt::format(" | predicted acquires {:.0f}/s",
                                static_cast<double>(predicted_acquires) / seconds);
            predicted_acquires = 0;
            LOG_INFO(HW_GPU, "GPU split drains per second:{}", line);
            waits = {};
            wait_ns = {};
            calls = {};
            window_start = now;
        }
    }
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache == write_position) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    const auto begin = std::chrono::steady_clock::now();
    ++waits[index];
    WaitConsumed(write_position);
    publish_every = 8;
    wait_ns[index] += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - begin)
                                           .count());
}

bool GpuSplit::WaitForWriters() {
    FlushState();
    const u64 target = std::min(last_writer_position, write_position);
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache >= target) {
        return false;
    }
    wait_target.store(target, std::memory_order_seq_cst);
    Publish();
    WaitConsumed(target);
    wait_target.store(~u64{0}, std::memory_order_relaxed);
    return true;
}

void GpuSplit::SwitchEngine(Tegra::Engines::EngineInterface* engine) {
    front_engine = engine;
    u64* const entry = Reserve(2);
    entry[0] = static_cast<u64>(Kind::Engine);
    entry[1] = reinterpret_cast<u64>(engine);
    Commit(2);
}

void GpuSplit::FlushStateSlow() {
    const size_t count = state_count;
    // Cleared first: Commit may publish, which flushes the state again.
    state_count = 0;
    if (++state_current == 0) {
        state_generation.fill(0);
        state_current = 1;
    }
    SelectEngine(state_engine);
    u64* const entry = Reserve(1 + count);
    entry[0] = static_cast<u64>(Kind::State) | (state_track ? IsLast : 0) | (u64{count} << 32);
    std::memcpy(entry + 1, state_pairs.data(), count * sizeof(u64));
    Commit(1 + count);
}

void GpuSplit::MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                           u32 amount, u32 methods_pending, const GPUVAddr* segment) {
    FlushState();
    SelectEngine(engine);
    // Long runs (constant buffer and inline uploads) go in pieces, as when a method's data
    // continues in the next GPFIFO entry.
    while (amount > 0) {
        const u32 piece = std::min(amount, MaxMultiWords);
        const size_t head = segment != nullptr ? 3 : 2;
        const size_t size = head + (piece + 1) / 2;
        u64* const entry = Reserve(size);
        entry[0] = static_cast<u64>(Kind::Multi) | (segment != nullptr ? HasSegment : 0) |
                   (u64{method & 0xffff} << 8) | (u64{piece} << 32);
        entry[1] = methods_pending;
        if (segment != nullptr) {
            entry[2] = *segment;
        }
        entry[size - 1] = 0;
        std::memcpy(entry + head, words, size_t{piece} * sizeof(u32));
        Commit(size);
        words += piece;
        amount -= piece;
        methods_pending -= piece;
    }
}

void GpuSplit::DeferFunction(std::function<void()>&& func) {
    auto* const heap = new std::function<void()>(std::move(func));
    Defer([heap] {
        (*heap)();
        delete heap;
    });
}

size_t GpuSplit::Execute(const u64* entry) {
    const u64 head = entry[0];
    switch (static_cast<Kind>(head & 0xf)) {
    case Kind::Wrap:
        return 0;
    case Kind::Engine:
        back_engine = reinterpret_cast<Tegra::Engines::EngineInterface*>(entry[1]);
        return 2;
    case Kind::Method: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 argument = static_cast<u32>(head >> 32);
        auto* const engine = back_engine;
        if ((head & Direct) != 0) {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            engine->CallMethod(*system, method, argument, true);
        } else if (!engine->execution_mask[method]) {
            engine->method_sink.emplace_back(method, argument);
        } else {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            if ((head & HasSegment) != 0) {
                engine->current_dma_segment = entry[1];
            }
            engine->CallMethod(*system, method, argument, (head & IsLast) != 0);
        }
        return (head & HasSegment) != 0 ? 2 : 1;
    }
    case Kind::Multi: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 amount = static_cast<u32>(head >> 32);
        const size_t data = (head & HasSegment) != 0 ? 3 : 2;
        auto* const engine = back_engine;
        Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
        engine->ConsumeSink(*system);
        if ((head & HasSegment) != 0) {
            engine->current_dma_segment = entry[2];
        }
        engine->CallMultiMethod(*system, method, reinterpret_cast<const u32*>(entry + data),
                                amount, static_cast<u32>(entry[1]));
        return data + (amount + 1) / 2;
    }
    case Kind::State: {
        const u32 count = static_cast<u32>(head >> 32);
        Nx2Profile::Scope state_scope{Nx2Profile::Zone::StateApply};
        back_engine->ApplyState(*system, entry + 1, count, (head & IsLast) != 0);
        return 1 + count;
    }
    case Kind::Call: {
        const size_t payload = static_cast<size_t>((head >> 8) & 0xffffff);
        Nx2Profile::Scope deferred_scope{Nx2Profile::Zone::Deferred};
        reinterpret_cast<void (*)(const u64*)>(entry[1])(entry + 2);
        return 2 + payload;
    }
    }
    return 1;
}

void GpuSplit::BackLoop(std::stop_token stop_token) {
    Common::SetCurrentThreadName("GPUBack");
    on_back_thread = true;
    back_running.store(true, std::memory_order_release);
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
    system->RegisterHostThread();
#ifdef _WIN32
    g_back_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
#endif
    u64 read_position = 0;
    while (!stop_token.stop_requested()) {
        Nx2Profile::Report("GPU back thread");
        u64 available = published.load(std::memory_order_acquire);
        if (available == read_position) {
            Nx2Profile::Scope scope{Nx2Profile::Zone::SplitWait};
            // The front usually sends more within microseconds: spin briefly before sleeping.
            for (u32 spins = 0; spins < 8192 && available == read_position &&
                                !hook_requested.load(std::memory_order_relaxed);
                 ++spins) {
                Pause();
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position && idle_hook != nullptr) {
                hook_requested.store(false, std::memory_order_seq_cst);
                idle_hook(idle_argument);
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                // Publish bumps `wake` when it sees back_waiting: read it before the last look
                // at `published`, so a publish in between ends the wait at once.
                back_waiting.store(true, std::memory_order_seq_cst);
                const u32 seen = wake.load(std::memory_order_seq_cst);
                available = published.load(std::memory_order_seq_cst);
                // A request made after the hook ran (RequestIdleHook) runs it before sleeping.
                if (available == read_position && !stop_token.stop_requested() &&
                    !hook_requested.load(std::memory_order_seq_cst)) {
                    wake.wait(seen, std::memory_order_seq_cst);
                }
                back_waiting.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        u64 last_store = read_position;
        g_back_busy.store(true, std::memory_order_relaxed);
        while (read_position != available) {
            const size_t index = read_position & RingMask;
            const size_t size = Execute(&ring[index]);
            read_position += size != 0 ? size : RingWords - index;
            if (hook_requested.load(std::memory_order_relaxed) &&
                idle_hook != nullptr) [[unlikely]] {
                hook_requested.store(false, std::memory_order_seq_cst);
                idle_hook(idle_argument);
            }
            // Hand ring space back now and then while working through a long batch, and at once
            // when the front waits for this position (WaitForWriters).
            if ((read_position - last_store >= 4096 ||
                 read_position >= wait_target.load(std::memory_order_relaxed)) &&
                read_position != available) {
                StoreConsumed(read_position);
                last_store = read_position;
            }
        }
        g_back_busy.store(false, std::memory_order_release);
        StoreConsumed(read_position);
    }
    back_running.store(false, std::memory_order_seq_cst);
    front_wake.fetch_add(1, std::memory_order_seq_cst);
    front_wake.notify_all();
}

} // namespace VideoCommon
