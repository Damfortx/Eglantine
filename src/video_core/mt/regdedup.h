// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/lite3_front_switches.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"

/// MT lite round 6, W2 (docs/mt-lite-plan6.md): front register-write dedup. A 3D state write to
/// a "latch-only" register (not executable, no front-side handling, not an instance loop method,
/// not the MME DMA read triggers 0x157/0x158, below the macro range) whose value the register
/// (and, with the shadow RAM in Track/TrackWithFilter, its shadow word) already holds changes
/// nothing anywhere: the split's front (FrontWrite) returns before sending it and the back
/// (CallMethod, ConsumeSinkImpl, ApplyState) skips it (no regs/shadow change, no dirty flags, no
/// DrawDeferred, no ProcessMethodCall). It is dropped where it enters instead: DmaPusher::CallMethod
/// (before FrontCallMethod / the method sink), the ProcessCommands batch paths (FrontWriteRun's
/// loop, the non-split sink batch), the split front's macro emit callback, and the leading words
/// of a non-incrementing run (FrontCallMultiMethod). Never with the shadow RAM in Replay.
namespace VideoCommon::MT::RegDedup {

/// EDEN_NX2_MT_LITE_REGDEDUP: '1' on (also without the MT back and without the split), '0' off,
/// 'a' alternating (Nx2AltOn), else on under EDEN_NX2_MT_LITE=1 (with EDEN_NX2_MT=1). Read once.
[[nodiscard]] inline char Mode() {
    static const char mode = [] {
        const char m = Tegra::Nx2EnvMode("EDEN_NX2_MT_LITE_REGDEDUP");
        const char result =
            m == 'a' ? 'a' : (Lite3Detail::Read("EDEN_NX2_MT_LITE_REGDEDUP") ? '1' : '0');
        LOG_INFO(HW_GPU, "MT lite 6: regdedup {}",
                 result == 'a' ? "alternating" : (result == '1' ? "on" : "off"));
        return result;
    }();
    return mode;
}

/// EDEN_NX2_MT_LITE_REGDEDUP_CHECK=1 (diagnostic, with the switch not '0'): nothing is dropped
/// early; the old paths run and every write the dedup would have dropped is checked: the count
/// of those the old path did not skip (sent / changed / sank with effect) should stay 0.
[[nodiscard]] inline bool Check() {
    static const bool on =
        Mode() != '0' &&
        Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_MT_LITE_REGDEDUP_CHECK"), false);
    return on;
}

/// Whether the dedup may ever run (the front view and the sink tracking are kept then).
[[nodiscard]] inline bool Armable() {
    static const bool on = Mode() != '0';
    return on;
}

/// Whether writes are dropped now (never in CHECK mode; 'a' follows Nx2AltOn).
[[nodiscard]] inline bool Active() {
    static const bool fixed = Mode() == '1' && !Check();
    if (fixed) {
        return true;
    }
    if (Mode() == 'a') [[unlikely]] {
        return !Check() && Tegra::Nx2AltOn();
    }
    return false;
}

/// Latch-only methods below the macro range (built once per process from Maxwell3D's
/// execution_mask; Maxwell3D::RegDedupArm).
class LatchTable {
public:
    static constexpr u32 Size = 0xE00; ///< MacroRegistersStart

    void Set(u32 method) {
        bits[method >> 6] |= u64{1} << (method & 63);
    }
    [[nodiscard]] bool Test(u32 method) const noexcept {
        return method < Size && ((bits[method >> 6] >> (method & 63)) & 1) != 0;
    }
    [[nodiscard]] u32 Count() const {
        u32 count = 0;
        for (const u64 word : bits) {
            count += static_cast<u32>(std::bitset<64>(word).count());
        }
        return count;
    }

private:
    std::array<u64, Size / 64> bits{};
};

/// The split front's registers (Maxwell3D::FrontState), armed once the front exists and skips
/// same-value writes (FrontState::skip_same). `Control` is Maxwell3D::Regs::ShadowRamControl.
/// `latch` set but `regs` null: arming was tried and the front does not skip same values.
template <typename Control>
struct FrontView {
    const u32* regs{};
    const u32* shadow{};
    const Control* control{};
    const bool* draw_pending{};
    const LatchTable* latch{};

    [[nodiscard]] bool Armed() const noexcept {
        return regs != nullptr;
    }

    /// FrontWrite's same-value return, restricted to latch registers and to the Track,
    /// TrackWithFilter and Passthrough modes (never Replay): regs and shadow keep their words.
    [[nodiscard]] bool Drops(u32 method, u32 value) const noexcept {
        if (regs == nullptr || !latch->Test(method) || regs[method] != value) {
            return false;
        }
        const auto mode = static_cast<u32>(*control);
        if (mode == 3) { // Replay
            return false;
        }
        return mode == 2 || shadow[method] == value; // Passthrough leaves the shadow alone
    }
};

/// Without the split: the 3D engine's method sink as DmaPusher fills it. `known` counts the
/// entries DmaPusher pushed (and stamped) since it last saw the sink empty; a register written
/// by a pending sunk entry must not be compared with `regs` (it changes before this write would
/// apply). An entry pushed elsewhere makes the sink's size differ from `known` (entries only go
/// away all at once): nothing is dropped until the sink is empty again.
struct SinkTrack {
    std::array<u32, LatchTable::Size> stamp{};
    u32 generation{1};
    std::size_t known{};

    void Clear() noexcept {
        if (known == 0) {
            return;
        }
        known = 0;
        if (++generation == 0) {
            stamp.fill(0);
            generation = 1;
        }
    }
    [[nodiscard]] bool Pending(u32 method) const noexcept {
        return method < stamp.size() && stamp[method] == generation;
    }
    void Note(u32 method) noexcept {
        if (method < stamp.size()) {
            stamp[method] = generation;
        }
        ++known;
    }
};

/// Where a write was dropped (stats).
enum class Site : u32 {
    Dma,   ///< DmaPusher::CallMethod and FrontWriteRun (command lists, split front)
    Macro, ///< The split front's macro emit callback
    Sink,  ///< DmaPusher without the split (before the method sink)
    Multi, ///< FrontCallMultiMethod (non-incrementing runs)
    Count,
};

/// Counts of the calling thread (kept with EDEN_NX2_GPU_PROFILE or CHECK).
struct Stats {
    std::array<u64, static_cast<std::size_t>(Site::Count)> dropped{};
    u64 checked{}; ///< CHECK: writes the dedup would have dropped (this window)
    u64 emitted{}; ///< CHECK: of all checked so far, writes the old path did not skip
    u64 window_start{};
    u32 events{};
};
inline thread_local Stats t_stats;

[[nodiscard]] inline bool StatsOn() {
    static const bool on = Mode() != '0' && (Nx2Profile::Enabled() || Check());
    return on;
}

/// Logs the calling thread's counts every 10 s.
inline void MaybeReport() {
    auto& s = t_stats;
    if ((++s.events & 1023) != 0) {
        return;
    }
    const u64 now = Nx2Profile::Now();
    if (s.window_start == 0) {
        s.window_start = now;
        return;
    }
    const u64 window = now - s.window_start;
    if (window < 10'000'000'000ULL) {
        return;
    }
    const double seconds = static_cast<double>(window) / 1e9;
    const auto per_second = [seconds](u64 value) {
        return static_cast<u64>(static_cast<double>(value) / seconds);
    };
    LOG_INFO(HW_GPU,
             "Register dedup: {} dropped/s early (dma), {} (macro), {} (sink), {} (multi); "
             "check: {} predicted/s, {} would have emitted (total)",
             per_second(s.dropped[0]), per_second(s.dropped[1]), per_second(s.dropped[2]),
             per_second(s.dropped[3]), per_second(s.checked), s.emitted);
    s.dropped.fill(0);
    s.checked = 0;
    s.window_start = now;
}

/// `count` writes dropped at `site`. `front_counted`: the old path counted them as Nx2Profile
/// MethodWrites and SameValueWrites (the split front's FrontWrite / FrontWriteRun did; the
/// method sink and FrontCallMultiMethod did not), so the profile's totals stay comparable.
inline void Dropped(Site site, u32 count, bool front_counted) {
    if (Nx2Profile::Enabled() && front_counted) [[unlikely]] {
        Nx2Profile::Add(Nx2Profile::Counter::MethodWrites, count);
        Nx2Profile::Add(Nx2Profile::Counter::SameValueWrites, count);
    }
    if (StatsOn()) [[unlikely]] {
        t_stats.dropped[static_cast<std::size_t>(site)] += count;
        MaybeReport();
    }
}

/// CHECK mode: a write the dedup would have dropped (`method`, `value`) reached the old path;
/// `emitted`: the old path did not skip it.
inline void Checked(bool emitted, u32 method, u32 value) {
    auto& s = t_stats;
    ++s.checked;
    if (emitted) [[unlikely]] {
        ++s.emitted;
        static std::atomic<u32> logged{};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
            LOG_ERROR(HW_GPU, "Register dedup CHECK: method {:#x} value {:#x} would have emitted",
                      method, value);
        }
    }
    MaybeReport();
}

} // namespace VideoCommon::MT::RegDedup
