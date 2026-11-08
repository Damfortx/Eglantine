// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <fmt/format.h>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_map>
#include <sched.h>
#include <pthread.h>

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

namespace Tegra {
bool Nx2AltOn() {
    static std::atomic<bool> state{true};
    static std::once_flag started;
    std::call_once(started, [] {
        LOG_INFO(HW_GPU, "Nx2 alt: on");
        std::thread([] {
            const char* text = std::getenv("EDEN_NX2_ALT_SECONDS");
            const auto period = std::chrono::milliseconds{
                text != nullptr ? std::max(1000LL, static_cast<long long>(std::atof(text) * 1000))
                                : 30'000LL};
            u64 draws = g_nx2_draws.load(), busy = g_nx2_back_busy_ns.load(),
                presents = g_nx2_presents.load();
            auto begin = std::chrono::steady_clock::now();
            for (;;) {
                std::this_thread::sleep_for(period);
                const bool was = state.load(std::memory_order_relaxed);
                state.store(!was, std::memory_order_relaxed);
                const auto now = std::chrono::steady_clock::now();
                const double seconds = std::chrono::duration<double>(now - begin).count();
                const u64 d = g_nx2_draws.load() - draws, b = g_nx2_back_busy_ns.load() - busy,
                          p = g_nx2_presents.load() - presents;
                LOG_INFO(HW_GPU,
                         "Nx2 alt window {}: {:.1f} fps, {:.0f} draws/s, back busy {:.0f} ms/s, "
                         "{:.3f} us/draw",
                         was ? "on" : "off", static_cast<double>(p) / seconds,
                         static_cast<double>(d) / seconds, static_cast<double>(b) / 1e6 / seconds,
                         d != 0 ? static_cast<double>(b) / 1e3 / static_cast<double>(d) : 0.0);
                LOG_INFO(HW_GPU, "Nx2 alt: {}", !was ? "on" : "off");
                draws += d;
                busy += b;
                presents += p;
                begin = now;
            }
        }).detach();
    });
    return state.load(std::memory_order_relaxed);
}
} // namespace Tegra

namespace VideoCommon {

namespace {
inline void CpuRelax() {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}
void PinThisThread(int cpu) {
#ifdef __linux__
    if (cpu < 0 || cpu >= static_cast<int>(std::thread::hardware_concurrency())) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)cpu;
#endif
}
std::pair<int, int> HelperCpus() {
    int back = 2, helper = 3;
    if (const char* text = std::getenv("EDEN_NX2_HELPER_CPUS")) {
        std::sscanf(text, "%d,%d", &back, &helper);
    }
    if (std::thread::hardware_concurrency() < 8) {
        return {-1, -1};
    }
    return {back, helper};
}
} // Anonymous namespace

BackHelper& BackHelper::Get() {
    static BackHelper helper;
    return helper;
}

BackHelper::BackHelper() {
    thread = std::jthread([this](std::stop_token stop) { Loop(stop); });
}

bool BackHelper::Enabled() {
    // Off by default: with the back pinned it saved 0.3 us a draw (2.5%), with it unpinned
    // nothing, and a pinned back competes with whatever else runs on its core.
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_BACK_HELPER");
    if (!GpuSplit::OnBackThread() || !Tegra::Nx2Switch(mode, false)) {
        return false;
    }
    if (!back_pinned) {
        back_pinned = true;
        PinThisThread(HelperCpus().first);
    }
    return true;
}

void BackHelper::Post(void (*job)(void*), void* context) {
    job_function = job;
    job_context = context;
    posted.fetch_add(1, std::memory_order_release);
    if (sleeping.load(std::memory_order_seq_cst) != 0) {
        posted.notify_one();
    }
}

void BackHelper::Join() {
    const u64 target = posted.load(std::memory_order_relaxed);
    while (done.load(std::memory_order_acquire) != target) {
        CpuRelax();
    }
}

void BackHelper::Loop(std::stop_token stop) {
    Common::SetCurrentThreadName("GPUBackHelper");
    PinThisThread(HelperCpus().second);
    std::stop_callback wake{stop, [this] {
        posted.fetch_add(1, std::memory_order_release);
        posted.notify_one();
    }};
    u64 seen = 0;
    u32 idle = 0;
    while (!stop.stop_requested()) {
        const u64 now = posted.load(std::memory_order_acquire);
        if (now == seen) {
            if (++idle < (1U << 16)) {
                CpuRelax();
                continue;
            }
            sleeping.store(1, std::memory_order_seq_cst);
            if (posted.load(std::memory_order_seq_cst) == seen) {
                posted.wait(seen, std::memory_order_seq_cst);
            }
            sleeping.store(0, std::memory_order_relaxed);
            idle = 0;
            continue;
        }
        idle = 0;
        seen = now;
        if (job_function != nullptr) {
            job_function(job_context);
        }
        done.store(now, std::memory_order_release);
    }
}


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
    // On for title 1 (with g_nx2_skip_known_writer_waits): with the writer waits gone its attract
    // demo ran 53-57 fps with it, 50-53 without (vsync off). Measured with the writer waits it ran
    // no faster: they held the front instead. EDEN_NX2_PREDICT_ACQUIRE=1 or 0 forces it.
    static const char mode = [] {
        const char* value = std::getenv("EDEN_NX2_PREDICT_ACQUIRE");
        return value != nullptr ? value[0] : 'd';
    }();
    return mode == '1' ||
           (mode == 'd' && Tegra::g_nx2_skip_known_writer_waits.load(std::memory_order_relaxed));
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
    const u64 begin = Nx2Profile::Enabled() ? Nx2Profile::Now() : 0;
    WaitConsumed(write_position + words - RingWords);
    if (Nx2Profile::Enabled()) {
        CountWait(DrainReason::RingFull, true, Nx2Profile::Now() - begin);
    }
}

// The front waits for the back to pass `target`: a short spin, then asleep until the back
// stores its position (it wakes the front only when the front sleeps). It spun up to 16,384
// pauses and then yielded in a loop: ~20% of the front's samples in title 1, and a busy hardware
// thread slows its SMT sibling (often the back or a game core). EDEN_NX2_SPLIT_FUTEX=1 sleeps;
// off by default: in title 1's attract demo it cost ~5% (the wake-ups are slower than the spin).
// =a alternates (Tegra::Nx2AltOn), to A/B it within a run.
void GpuSplit::WaitConsumed(u64 target) {
    static const char futex_mode = Tegra::Nx2EnvMode("EDEN_NX2_SPLIT_FUTEX");
    const bool futex = Tegra::Nx2Switch(futex_mode, false);
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

void GpuSplit::CountWait(DrainReason reason, bool waited, u64 wait_ns) {
    // Front thread only: logged every 10 s with EDEN_NX2_GPU_PROFILE.
    const auto index = static_cast<size_t>(reason);
    ++stat_calls[index];
    stat_waits[index] += waited ? 1 : 0;
    stat_wait_ns[index] += wait_ns;
    if (!Nx2Profile::Enabled()) {
        return;
    }
    const u64 now = Nx2Profile::Now();
    if (stat_window_start == 0) {
        stat_window_start = now;
        return;
    }
    if (now - stat_window_start < 10'000'000'000ULL) {
        return;
    }
    static constexpr std::array<const char*, static_cast<size_t>(DrainReason::Count)> names{
        "other",   "sync",   "gpu-written commands", "front state",       "dma read",
        "verify",  "puller", "writers (commands)",   "writers (dma read)", "position",
        "ring full"};
    const double seconds = static_cast<double>(now - stat_window_start) / 1e9;
    std::string line;
    for (size_t i = 0; i < names.size(); ++i) {
        if (stat_calls[i] != 0) {
            line += fmt::format(" | {} {:.0f}/s ({:.0f} waited, {:.1f} ms)", names[i],
                                static_cast<double>(stat_calls[i]) / seconds,
                                static_cast<double>(stat_waits[i]) / seconds,
                                static_cast<double>(stat_wait_ns[i]) / 1e6 / seconds);
        }
    }
    line += fmt::format(" | predicted acquires {:.0f}/s",
                        static_cast<double>(predicted_acquires) / seconds);
    predicted_acquires = 0;
    LOG_INFO(HW_GPU, "GPU split drains per second:{}", line);
    stat_waits = {};
    stat_wait_ns = {};
    stat_calls = {};
    stat_window_start = now;
}

void GpuSplit::Drain(DrainReason reason) {
    FlushState();
    // Everything sent so far has run once this returns: so have the writes it noted.
    pending_write_count = 0;
    pending_write_overflow = false;
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache == write_position && BackQuiet()) {
        CountWait(reason, false, 0);
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    const u64 begin = Nx2Profile::Enabled() ? Nx2Profile::Now() : 0;
    WaitConsumed(write_position);
    // Everything consumed is not enough: the back submits deferred work (Scheduler::DeferFlush)
    // from its idle hook afterwards, and the caller may use the scheduler once this returns.
    if (!BackQuiet()) {
        RequestIdleHook();
        for (u32 spins = 0; !BackQuiet() && back_running.load(std::memory_order_acquire);
             ++spins) {
            if (spins < 16384) {
                Pause();
            } else {
                std::this_thread::yield();
            }
        }
    }
    publish_every = 8;
    CountWait(reason, true, Nx2Profile::Enabled() ? Nx2Profile::Now() - begin : 0);
}

void GpuSplit::WaitForPosition(u64 position, DrainReason reason) {
    FlushState();
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache >= position) {
        CountWait(reason, false, 0);
        return;
    }
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    const u64 begin = Nx2Profile::Enabled() ? Nx2Profile::Now() : 0;
    wait_target.store(position, std::memory_order_seq_cst);
    Publish();
    WaitConsumed(position);
    wait_target.store(~u64{0}, std::memory_order_relaxed);
    CountWait(reason, true, Nx2Profile::Enabled() ? Nx2Profile::Now() - begin : 0);
}

bool GpuSplit::WaitForWriters(DrainReason reason) {
    FlushState();
    const u64 target = std::min(last_writer_position, write_position);
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache >= target) {
        CountWait(reason, false, 0);
        return false;
    }
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    const u64 begin = Nx2Profile::Enabled() ? Nx2Profile::Now() : 0;
    wait_target.store(target, std::memory_order_seq_cst);
    Publish();
    WaitConsumed(target);
    wait_target.store(~u64{0}, std::memory_order_relaxed);
    CountWait(reason, true, Nx2Profile::Enabled() ? Nx2Profile::Now() - begin : 0);
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

void GpuSplit::FlushMacroList() {
    const size_t count = list_count;
    list_count = 0; // First: SelectEngine and Reserve may flush the state, which comes here.
    SelectEngine(list_engine);
    u64* const entry = Reserve(1 + count);
    entry[0] = static_cast<u64>(Kind::List) | (u64{count} << 32);
    std::memcpy(entry + 1, list.data(), count * sizeof(u64));
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
        // EDEN_NX2_METHOD_STATS: which methods the back runs one by one (top 16 every 10 s).
        static const bool method_stats = std::getenv("EDEN_NX2_METHOD_STATS") != nullptr;
        if (method_stats) [[unlikely]] {
            static std::unordered_map<u64, u64> counts;
            static auto window = std::chrono::steady_clock::now();
            const bool executed = (head & Direct) != 0 || engine->execution_mask[method];
            ++counts[(reinterpret_cast<u64>(engine) << 20) ^ (u64{method} << 1) ^ (executed ? 1 : 0)];
            if (std::chrono::steady_clock::now() - window > std::chrono::seconds{10}) {
                std::vector<std::pair<u64, u64>> top(counts.begin(), counts.end());
                std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
                std::string line;
                u64 total = 0;
                for (auto& [k, c] : top) total += c;
                for (size_t i = 0; i < std::min<size_t>(16, top.size()); ++i) {
                    line += fmt::format(" | {:#x}{} {}", (top[i].first >> 1) & 0x7ffff,
                                        (top[i].first & 1) ? "x" : "", top[i].second / 10);
                }
                LOG_INFO(HW_GPU, "Back methods per second: {} total{}", total / 10, line);
                counts.clear();
                window = std::chrono::steady_clock::now();
            }
        }
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
    case Kind::List: {
        const size_t count = static_cast<size_t>(head >> 32);
        auto* const engine = back_engine;
        Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
        engine->ConsumeSink(*system);
        const u64* const tokens = entry + 1;
        std::array<u32, 2048> words;
        for (size_t i = 0; i < count; ++i) {
            const u32 method = static_cast<u32>(tokens[i] & 0xffff);
            if ((tokens[i] & Mergeable) != 0) {
                size_t n = 0;
                while (i + n < count && (tokens[i + n] & Mergeable) != 0) {
                    words[n] = static_cast<u32>(tokens[i + n] >> 32);
                    ++n;
                }
                engine->CallMultiMethod(*system, method, words.data(), static_cast<u32>(n),
                                        static_cast<u32>(n));
                i += n - 1;
                continue;
            }
            engine->CallMethod(*system, method, static_cast<u32>(tokens[i] >> 32), true);
        }
        return 1 + count;
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
    Tegra::Nx2ApplyTimerSlack();
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
            // EDEN_NX2_BACK_SPIN=<count> sets the number of pauses (default 8192; 'a'
            // alternates 8192/32768) to trade idle CPU for fewer futex sleep/wake trips.
            static const char spin_mode = Tegra::Nx2EnvMode("EDEN_NX2_BACK_SPIN");
            static const u32 spin_count = [] {
                const char* text = std::getenv("EDEN_NX2_BACK_SPIN");
                if (text == nullptr || text[0] == 'a') {
                    return 8192U;
                }
                return static_cast<u32>(std::clamp(std::atoll(text), 0LL, 1LL << 30));
            }();
            const u32 max_spins =
                spin_mode == 'a' ? (Tegra::Nx2AltOn() ? 32768U : 8192U) : spin_count;
            for (u32 spins = 0; spins < max_spins && available == read_position &&
                                !hook_requested.load(std::memory_order_relaxed);
                 ++spins) {
                Pause();
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                hook_requested.store(false, std::memory_order_seq_cst);
                // The front may already see everything consumed: Drain() waits for back_quiet
                // too, so the deferred submit can't overlap whatever the front does next.
                if (!back_quiet.load(std::memory_order_relaxed)) {
                    if (idle_hook != nullptr) {
                        idle_hook(idle_argument);
                    }
                    back_quiet.store(true, std::memory_order_release);
                }
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
        // Before any entry runs, so a front that sees these entries consumed also sees this.
        back_quiet.store(false, std::memory_order_seq_cst);
        g_back_busy.store(true, std::memory_order_relaxed);
        // The A/B windows' busy time (Tegra::Nx2AltOn): only timed while they are logged.
        static const bool time_busy = std::getenv("EDEN_NX2_ALT_SECONDS") != nullptr;
        const auto busy_begin = time_busy ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
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
        if (time_busy) Tegra::g_nx2_back_busy_ns.fetch_add(
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - busy_begin)
                                 .count()),
            std::memory_order_relaxed);
        StoreConsumed(read_position);
    }
    back_running.store(false, std::memory_order_seq_cst);
    front_wake.fetch_add(1, std::memory_order_seq_cst);
    front_wake.notify_all();
}

} // namespace VideoCommon
