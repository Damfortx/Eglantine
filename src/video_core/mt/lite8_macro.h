// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/mt_switches.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"

/// MT lite batch 8, U3 (docs/batch8-plan.md): cheaper 3D macros on the split's front
/// (Maxwell3D::FrontRunMacro). Each piece has its own switch: '0' off, '1' on (also without
/// EDEN_NX2_MT_LITE: both are exact), 'a' alternating (Nx2AltOn, decided per macro call), else on
/// under EDEN_NX2_MT_LITE=1 (with EDEN_NX2_MT=1) and off otherwise. Read once.
///
/// MEMIT: a macro's plain state write (not executable, not 0x157/0x158, not an instance loop
///   method, shadow RAM not replaying, front skipping same values, state blocks) is applied in
///   the JIT's emit callback (FrontWrite's non-executable branch inline, as FrontWriteRun's
///   EDEN_NX2_MT_LITE_FBATCH path does) instead of a FrontWrite call.
/// MHLE: native macros recognised by their exact code words (not by position: the same code
///   at any start; positions and code are re-matched whenever either changes) run as C++ that
///   makes exactly the JIT's emits (same methods, values, order) and data RAM writes, through
///   the same emit callback. A call the C++ can't take whole (too few parameters, an out of
///   range data RAM address, a loop count near the JIT's step limit) runs the JIT: the checks
///   come before any side effect. None of these macros reads registers (MME state reads).
namespace VideoCommon::MT::Lite8Macro {

namespace Detail {
[[nodiscard]] inline char Mode(const char* name) {
    const char mode = Tegra::Nx2EnvMode(name);
    if (mode == '0' || mode == '1' || mode == 'a') {
        return mode;
    }
    return Lite() ? '1' : '0';
}
[[nodiscard]] inline bool On(char mode) {
    return mode == '1' || (mode == 'a' && Tegra::Nx2AltOn());
}
[[nodiscard]] inline const char* Name(char mode) {
    return mode == 'a' ? "alternating" : (mode == '1' ? "on" : "off");
}
} // namespace Detail

/// EDEN_NX2_MT_LITE8_MEMIT.
[[nodiscard]] inline char EmitMode() {
    static const char mode = Detail::Mode("EDEN_NX2_MT_LITE8_MEMIT");
    return mode;
}
/// EDEN_NX2_MT_LITE8_MHLE.
[[nodiscard]] inline char HleMode() {
    static const char mode = Detail::Mode("EDEN_NX2_MT_LITE8_MHLE");
    return mode;
}
/// EDEN_NX2_MT_LITE8_MACRO_CHECK=1 (diagnostic, with MHLE not '0'): every call of a recognised
/// macro runs the C++ into a recorder (on a copy of the data RAM), then the JIT for real
/// (recorded too); emits and data RAM are compared, first 16 mismatches logged.
[[nodiscard]] inline bool Check() {
    static const bool on = HleMode() != '0' &&
                           Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_MT_LITE8_MACRO_CHECK"),
                                            false);
    return on;
}
/// The 10 s stats line: EDEN_NX2_MT_LITE8_MACRO_STATS=1, EDEN_NX2_GPU_PROFILE or CHECK.
[[nodiscard]] inline bool StatsOn() {
    static const bool on =
        Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_MT_LITE8_MACRO_STATS"), false) ||
        Nx2Profile::Enabled() || Check();
    return on;
}
/// Logs the modes once (first front macro).
inline void LogOnce() {
    static const bool logged = [] {
        LOG_INFO(HW_GPU, "MT lite 8: macro emit {}, macro HLE {}{}", Detail::Name(EmitMode()),
                 Detail::Name(HleMode()), Check() ? " (CHECK)" : "");
        return true;
    }();
    (void)logged;
}

/// Recognised macros (docs/gpuparam-plan.md, tools/mmedis.py; the list dump's index/hash).
enum class Kind : u8 {
    None,
    DrawInstanced, ///< macro 41 d11d56facfb0c568: DrawElementsInstanced (6 parameters)
    ClampByte,     ///< macro 45 f807be34d132c5e5: data RAM merge/clamp -> method 0xd8
    CbTable,       ///< macro 14 9fac2f473ee5f427: const buffer select from a data RAM table
    CbFixed,       ///< macro 27 0250b63ec70ca443: const buffer select + offset
    CbBind,        ///< macro 1 bf9686d3c4d7cddc: guarded data RAM update + const buffer bind
    Count,
};

[[nodiscard]] inline const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::DrawInstanced: return "draw-instanced";
    case Kind::ClampByte: return "clamp-byte";
    case Kind::CbTable: return "cb-table";
    case Kind::CbFixed: return "cb-fixed";
    case Kind::CbBind: return "cb-bind";
    default: return "-";
    }
}

namespace Code {
// Exact words of every instruction the macro can run (from its start; 3 words each, as
// MacroEngine::native_code holds them: high word first). Branches are pc-relative.
inline constexpr std::array<u32, 30> DrawInstanced{
    0x3,      0x1d080000, 0x31c10300, 0x940003, 0x18c0017d, 0xf1c00300, 0x3c741437, 0x18c00111,
    0xb18c0300, 0x78143b, 0x18c02001, 0x700c7700, 0x2c3417e3, 0x1c00161, 0xb02c0300, 0x1140003,
    0x18c00161, 0x718c0300, 0x7,      0x20009a10, 0x34214300, 0x3,      0x18c007ff, 0x700c7700,
    0x9d141437, 0x18c00111, 0xb18c0301, 0x1140003, 0x18c00143, 0xb18c0300};
inline constexpr std::array<u32, 21> ClampByte{
    0x3,      0x1a17812e, 0x31c18300, 0xa0,     0x7804012e, 0x7190fb00, 0x3,
    0x18c02000, 0xf00c7700, 0x0,     0x2304d840, 0x6214300, 0x3,      0x18c01840,
    0x2214002, 0x580363, 0x2c0012e, 0x879c7f01, 0x3,      0x18c00000, 0x318c0300};
inline constexpr std::array<u32, 12> CbTable{0x130f,     0x1907812f, 0x39908300, 0x3,
                                             0x18c00000, 0x3010fb00, 0x10586383, 0xc00046,
                                             0x38c4301,  0x200003,   0x18c008c0, 0x38c4300};
inline constexpr std::array<u32, 9> CbFixed{0x186383,   0x18c0012d, 0xf1907b00,
                                            0x20a00460, 0x18c40240, 0x318c0301,
                                            0x20202f03, 0x98c408c0, 0x18c4300};
// Instructions 0x21..0x2f of the dumped 17 (0x30/0x31 are never reached: 0x2e ENDs, 0x2a stops).
inline constexpr std::array<u32, 45> CbBind{
    0x3,      0x1d200000, 0x31c28300, 0x3,      0x1d100000, 0x31c18300, 0x23,     0x18c02001,
    0xf45c6700, 0x3,      0x18c00128, 0xb900300, 0x1003,   0x18c00510, 0xba0c300, 0x3,
    0x18c00120, 0x810c300, 0x3,      0x2f8000, 0x72100300, 0x3,      0x18c00000, 0x61c7f00,
    0x3,      0x18c00000, 0x40c7f00, 0x3f,     0x18c00400, 0x345c6700, 0x386383, 0x18c000a3,
    0x58c4300, 0x20200003, 0x3c00040, 0x58c4300, 0x3,      0x18c00000, 0xf2400300, 0x42413,
    0x18c00318, 0xbac4301, 0x200007, 0x18c004e0, 0x1ac4300};
} // namespace Code

/// The recognised macro whose code starts at instruction `start` of `code` (None: none).
[[nodiscard]] inline Kind Match(std::span<const u32> code, u32 start) {
    const std::size_t first = std::size_t{start} * 3;
    const auto at = [&](const auto& words) {
        return first + words.size() <= code.size() &&
               std::equal(words.begin(), words.end(), code.begin() + first);
    };
    if (at(Code::DrawInstanced)) return Kind::DrawInstanced;
    if (at(Code::ClampByte)) return Kind::ClampByte;
    if (at(Code::CbTable)) return Kind::CbTable;
    if (at(Code::CbFixed)) return Kind::CbFixed;
    if (at(Code::CbBind)) return Kind::CbBind;
    return Kind::None;
}

/// Per macro index: the kind matched for (code owner, start, code generation).
class KindCache {
public:
    [[nodiscard]] Kind Get(const void* owner, u32 index, u32 start, u64 generation,
                           std::span<const u32> code) {
        Entry& entry = entries[index % entries.size()];
        if (entry.owner == owner && entry.start == start && entry.generation == generation &&
            entry.index == index) [[likely]] {
            return entry.kind;
        }
        entry = {owner, generation, index, start, Match(code, start)};
        return entry.kind;
    }

private:
    struct Entry {
        const void* owner{};
        u64 generation{~u64{0}};
        u32 index{~0U};
        u32 start{~0U};
        Kind kind{Kind::None};
    };
    std::array<Entry, 128> entries{};
};

enum class Outcome : u8 {
    NotHandled, ///< nothing done: run the JIT
    Done,       ///< the call ran as the JIT would have (success)
    EmitFailed, ///< an emit returned false: the JIT stops there too ("unsupported emitted method")
};

/// Runs a recognised macro. `params`: the call's parameters (load FIFO); `dram`: the channel's
/// data RAM (0xc00 words); `emit(method, value) -> bool` as the JIT's emit callback. Exactly the
/// JIT's behaviour (TuringMacroJit / TuringMacroInterpreter semantics) for the code in `Code`.
template <typename Emit>
[[nodiscard]] Outcome Run(Kind kind, std::span<const u32> params, u32* dram, Emit&& emit) {
#define LITE8_EMIT(method, value)                                                                \
    do {                                                                                         \
        if (!emit(static_cast<u32>(method), static_cast<u32>(value))) {                          \
            return Outcome::EmitFailed;                                                          \
        }                                                                                        \
    } while (0)
    constexpr u32 DataRamWords = 0xc00;
    switch (kind) {
    case Kind::DrawInstanced: {
        // The loop's back edge goes through the JIT's dispatch (its 1,000,000 step limit).
        if (params.size() < 6 || params[2] > 500000) {
            return Outcome::NotHandled;
        }
        u32 begin = params[0];
        const u32 count = params[1];
        const u32 instances = params[2];
        LITE8_EMIT(0x5f7, params[3]); // index_buffer.first
        LITE8_EMIT(0x446, params[4]); // vertex_id_base
        LITE8_EMIT(0x50d, params[4]); // global_base_vertex_index
        LITE8_EMIT(0x50e, params[5]); // global_base_instance_index
        for (u32 left = instances; left != 0; --left) {
            LITE8_EMIT(0x586, begin); // draw.begin
            LITE8_EMIT(0x5f8, count); // index_buffer.count
            LITE8_EMIT(0x585, 0);     // draw.end
            begin = (begin & ~(3U << 26)) | (1U << 26); // instance id: Subsequent
        }
        LITE8_EMIT(0x446, 0);
        LITE8_EMIT(0x50d, 0);
        LITE8_EMIT(0x50e, 0);
        return Outcome::Done;
    }
    case Kind::ClampByte: {
        if (params.empty()) {
            return Outcome::NotHandled;
        }
        const u32 value = params[0];
        u32 word = dram[0x4b8];
        const u32 limit = dram[0x4b9];
        if (((value >> 8) & 1) != 0) {
            word = (word & 0x00ffffffU) | ((value & 0xff) << 24);
            if (!(static_cast<s32>(value) <= static_cast<s32>(limit))) {
                word = (word & 0x00ffffffU) | ((limit & 0xff) << 24);
            }
        }
        dram[0x4ba] = value;
        LITE8_EMIT(0xd8, word);
        return Outcome::Done;
    }
    case Kind::CbTable: {
        if (params.empty()) {
            return Outcome::NotHandled;
        }
        const u32 address = 0x4bc + params[0];
        if (address >= DataRamWords) {
            return Outcome::NotHandled;
        }
        const u32 size = dram[0x4c3];
        const u32 gpu_address = dram[address];
        LITE8_EMIT(0x8e0, size);
        LITE8_EMIT(0x8e1, gpu_address >> 24);
        LITE8_EMIT(0x8e2, (gpu_address & 0x00ffffffU) << 8);
        return Outcome::Done;
    }
    case Kind::CbFixed: {
        // The parameter is loaded after the first emits: too few runs the JIT (it fails there).
        if (params.empty()) {
            return Outcome::NotHandled;
        }
        const u32 gpu_address = dram[0x4b7];
        LITE8_EMIT(0x8e0, 0x900);
        LITE8_EMIT(0x8e1, gpu_address >> 24);
        LITE8_EMIT(0x8e2, (gpu_address & 0x00ffffffU) << 8);
        LITE8_EMIT(0x8e3, (params[0] & 0x3fffffffU) << 2);
        return Outcome::Done;
    }
    case Kind::CbBind: {
        if (params.size() < 4) {
            return Outcome::NotHandled;
        }
        u32 selector = params[0];
        const u32 slot = params[1];
        const u32 low = params[2];
        const u32 high = params[3];
        if (!(static_cast<s32>(selector) < 8)) {
            const u32 read_address = 0x4a0 + selector;
            if (read_address >= DataRamWords) {
                return Outcome::NotHandled;
            }
            u32 base = 0x400U | ((selector & 3) << 5);
            base = (base & ~(0xfU << 1)) | ((slot & 0xf) << 1);
            selector = dram[read_address];
            dram[base] = low;
            dram[base + 1] = high;
            if (!(static_cast<s32>(selector) < 0xf)) {
                return Outcome::Done; // branch to STOP
            }
        }
        LITE8_EMIT(0x8e0, (high >> 12) & 0xfffffU);
        LITE8_EMIT(0x8e1, high & 0xffU);
        LITE8_EMIT(0x8e2, low);
        const u32 method = (0x904U & ~(7U << 3)) | ((selector & 7) << 3);
        LITE8_EMIT(method & 0xfff, 1U | (((slot + 3) & 0x0fffffffU) << 4));
        return Outcome::Done;
    }
    default:
        return Outcome::NotHandled;
    }
#undef LITE8_EMIT
}

/// Counts of the front thread (StatsOn).
struct Stats {
    static constexpr std::size_t Indices = 128;
    std::array<u64, Indices> calls{};
    std::array<u64, Indices> emits{};
    std::array<u64, static_cast<std::size_t>(Kind::Count)> hle{};
    u64 total{};
    u64 not_handled{};
    u64 fast_emits{};
    u64 check_compared{};
    u64 check_mismatched{}; ///< total since start
    u64 window_start{};
    u32 events{};
};
inline thread_local Stats t_stats;

/// A finished call (stats on): macro `index`, `emits` emitted words, `kind` HLE taken (None:
/// JIT), `not_handled`: recognised but the JIT ran, `fast`: emits applied inline (MEMIT).
/// `describe(index)` -> std::pair<u32 start, u64 fnv64 of its code> for the report.
template <typename Describe>
inline void Note(u32 index, u32 emits, Kind kind, bool not_handled, u32 fast,
                 Describe&& describe) {
    auto& s = t_stats;
    const std::size_t slot = index % Stats::Indices;
    ++s.calls[slot];
    s.emits[slot] += emits;
    ++s.total;
    s.fast_emits += fast;
    if (kind != Kind::None) {
        ++s.hle[static_cast<std::size_t>(kind)];
    }
    s.not_handled += not_handled ? 1 : 0;
    if ((++s.events & 4095) != 0) {
        return;
    }
    const u64 now = Nx2Profile::Now();
    if (s.window_start == 0) {
        s = Stats{.check_mismatched = s.check_mismatched, .window_start = now};
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
    std::array<u32, Stats::Indices> order{};
    for (u32 i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::partial_sort(order.begin(), order.begin() + 10, order.end(),
                      [&s](u32 a, u32 b) { return s.calls[a] > s.calls[b]; });
    std::string top;
    for (u32 i = 0; i < 10 && s.calls[order[i]] != 0; ++i) {
        const u32 macro = order[i];
        const auto [start, hash] = describe(macro);
        top += fmt::format(" [{} start={:x} hash={:016x} {}/s {:.1f} emits]", macro, start, hash,
                           per_second(s.calls[macro]),
                           static_cast<double>(s.emits[macro]) /
                               static_cast<double>(s.calls[macro]));
    }
    std::string hle;
    for (std::size_t k = 1; k < s.hle.size(); ++k) {
        hle += fmt::format(" {} {}/s", KindName(static_cast<Kind>(k)), per_second(s.hle[k]));
    }
    LOG_INFO(HW_GPU,
             "Front macros in 10 s: {} calls/s, {} fast emits/s; HLE:{}; recognised but JIT {}/s; "
             "check {}/s compared, {} mismatched (total); top:{}",
             per_second(s.total), per_second(s.fast_emits), hle, per_second(s.not_handled),
             per_second(s.check_compared), s.check_mismatched, top);
    s = Stats{.check_mismatched = s.check_mismatched, .window_start = now};
}

/// CHECK: one compared call; logs the first 16 mismatches.
inline void Checked(bool equal, u32 index, u32 start, Kind kind, std::span<const u32> params,
                    std::span<const std::pair<u32, u32>> hle_writes,
                    std::span<const std::pair<u32, u32>> jit_writes, bool dram_equal,
                    Outcome outcome, bool jit_success) {
    auto& s = t_stats;
    ++s.check_compared;
    if (equal) {
        return;
    }
    ++s.check_mismatched;
    static std::atomic<u32> logged{};
    if (logged.fetch_add(1, std::memory_order_relaxed) >= 16) {
        return;
    }
    std::size_t first = 0;
    while (first < hle_writes.size() && first < jit_writes.size() &&
           hle_writes[first] == jit_writes[first]) {
        ++first;
    }
    std::string text;
    for (std::size_t i = 0; i < params.size() && i < 8; ++i) {
        text += fmt::format(" {:x}", params[i]);
    }
    const auto at = [first](std::span<const std::pair<u32, u32>> writes) {
        return first < writes.size()
                   ? fmt::format("{:x}={:x}", writes[first].first, writes[first].second)
                   : std::string{"none"};
    };
    LOG_ERROR(HW_GPU,
              "Front macro HLE CHECK: macro {} start={:x} {} params{}: outcome {} jit {}, "
              "{} vs {} writes, first difference #{} hle {} jit {}, data RAM {}",
              index, start, KindName(kind), text, static_cast<u32>(outcome),
              jit_success ? "ok" : "failed", hle_writes.size(), jit_writes.size(), first,
              at(hle_writes), at(jit_writes), dram_equal ? "equal" : "DIFFERENT");
}

} // namespace VideoCommon::MT::Lite8Macro
