// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

#include "common/common_types.h"
#include "video_core/mt/mt_switches.h"

/// Thread roles of the multithreaded GPU back (docs/mt-plan.md, phase 4) and the hooks code
/// outside the rasterizer uses to reach the committer. Everything here is inert while
/// EDEN_NX2_MT is off: no thread has a role and no hook is installed.
namespace VideoCommon::MT {

enum class Role : u8 { None, Front, Sequencer, Committer, Resolver };

inline thread_local Role g_role = Role::None;

[[nodiscard]] inline bool OnSequencer() noexcept {
    return g_role == Role::Sequencer;
}
[[nodiscard]] inline bool OnCommitter() noexcept {
    return g_role == Role::Committer;
}

/// Set while the committer runs (the sequencer then routes its rasterizer work to it).
inline std::atomic<bool> g_active{};
[[nodiscard]] inline bool Active() noexcept {
    return g_active.load(std::memory_order_relaxed);
}

/// EDEN_NX2_MT_LOCKSTEP=1: the sequencer waits for each packet/command to be committed.
[[nodiscard]] inline bool Lockstep() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_LOCKSTEP");
    return Tegra::Nx2Switch(mode, false);
}
/// EDEN_NX2_MT_VERIFY=1 (implies lockstep): compare captures with guest memory and the mirror
/// with the real engine at every commit.
[[nodiscard]] inline bool Verify() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_VERIFY");
    return Tegra::Nx2Switch(mode, false);
}
/// EDEN_NX2_MT_STATS=1: log the pipeline's counters every 10 s.
[[nodiscard]] inline bool Stats() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_STATS");
    return Tegra::Nx2Switch(mode, false);
}
/// EDEN_NX2_MT_CHECK=1: thread role asserts.
[[nodiscard]] inline bool Check() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_CHECK");
    return Tegra::Nx2Switch(mode, false);
}
/// EDEN_NX2_MT_COMPUTE=packet: compute dispatches as packets (default serial).
[[nodiscard]] inline bool ComputePackets() {
    static const bool packet = [] {
        const char* value = std::getenv("EDEN_NX2_MT_COMPUTE");
        return value != nullptr && value[0] == 'p';
    }();
    return packet;
}
/// EDEN_NX2_MT_DRAWS=serial: every draw as a Serial item (debug: everything on the committer).
[[nodiscard]] inline bool SerialDraws() {
    static const bool serial = [] {
        const char* value = std::getenv("EDEN_NX2_MT_DRAWS");
        return value != nullptr && value[0] == 's';
    }();
    return serial;
}
/// EDEN_NX2_MT_STRICT_TEX=1: every guest write by the sequencer waits for the committer.
[[nodiscard]] inline bool StrictTextures() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_STRICT_TEX");
    return Tegra::Nx2Switch(mode, false);
}

/// Installed by the committer pipeline while it runs.
struct Hooks {
    void* context = nullptr;
    /// Sequencer: runs fn(arg) on the committer after everything emitted so far and waits.
    void (*run_serial)(void* context, void (*fn)(void*), void* arg) = nullptr;
    /// Sequencer: runs fn(a, b) on the committer after everything emitted so far (async).
    void (*post_command)(void* context, void (*fn)(void*, void*), void* a, void* b) = nullptr;
    /// Sequencer: before it writes guest memory at [device_addr, +size).
    void (*before_write)(void* context, u64 device_addr, u64 size) = nullptr;
};
inline Hooks g_hooks{};
inline std::atomic<bool> g_write_hook_on{};

/// Runs `func` on the committer (Serial) when called on the sequencer, else inline. Returns its
/// result; exceptions are rethrown on the caller.
template <typename Func>
decltype(auto) RunOnCommitter(Func&& func) {
    using Result = std::invoke_result_t<Func&>;
    if (!OnSequencer() || g_hooks.run_serial == nullptr) {
        return func();
    }
    struct Context {
        Func* func;
        std::conditional_t<std::is_void_v<Result>, char,
                           std::optional<std::remove_reference_t<Result>>>
            result;
        std::exception_ptr error;
    } context{&func, {}, {}};
    g_hooks.run_serial(
        g_hooks.context,
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

/// Sequencer: queues fn(a, b) for the committer; returns false (nothing queued) elsewhere.
inline bool PostToCommitter(void (*fn)(void*, void*), void* a, void* b) {
    if (!OnSequencer() || g_hooks.post_command == nullptr) {
        return false;
    }
    g_hooks.post_command(g_hooks.context, fn, a, b);
    return true;
}

/// MemoryManager's write paths: before the sequencer writes guest memory.
inline void BeforeGuestWrite(u64 device_addr, u64 size) {
    if (g_write_hook_on.load(std::memory_order_relaxed) && OnSequencer()) [[unlikely]] {
        g_hooks.before_write(g_hooks.context, device_addr, size);
    }
}

} // namespace VideoCommon::MT
