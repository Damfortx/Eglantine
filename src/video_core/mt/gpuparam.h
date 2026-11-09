// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/macro_turing.h"
#include "video_core/mt/lite_switches.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"

/// GPU-written macro parameters (docs/gpuparam-plan.md). NVN on Switch 2 implements indexed
/// indirect draws by starting a draw macro call in the CPU pushbuffer (its first parameter, the
/// draw.begin word) and pointing the next GPFIFO entry at the indirect buffer the GPU (culling
/// compute) fills: the GPU-written "command list" is the macro's remaining parameters,
/// {index count, instance count, first index, vertex offset, first instance} (Vulkan's
/// VkDrawIndexedIndirectCommand), or a draw count followed by N such records. Mario Kart World:
/// ~420 such fetches a second, each waiting for the back (DmaPusher::ReadGpuWrittenCommands).
///
/// Instead of waiting, the front runs the macro with zero GPU words (a call that draws nothing:
/// it leaves the registers as the call would apart from the GPU-dependent draw registers) and
/// sends the back an indirect draw reading the parameters where the GPU wrote them
/// (vkCmdDrawIndexedIndirect / ...IndirectCount through DrawManager::DrawIndexedIndirect). The
/// macro must be one whose effect is exactly that draw: checked by running its native code
/// against a reference model on test parameters (Classifier), not by a fixed code hash.
namespace VideoCommon::MT::GpuParam {

// ---------------------------------------------------------------------------------- switches

/// EDEN_NX2_MT_LITE_GPUPARAM (=0/1/a): on by default under EDEN_NX2_MT_LITE=1 (with the split
/// front running macros). Read for every call ('a' alternates).
[[nodiscard]] inline bool MtSwitch() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_LITE_GPUPARAM");
    return Enabled() && Tegra::Nx2Switch(mode, Lite());
}
/// EDEN_NX2_GPUPARAM (=0/1/a, default off): the same without the MT back (single GPU thread or
/// the split without EDEN_NX2_MT).
[[nodiscard]] inline bool PlainSwitch() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_GPUPARAM");
    return Tegra::Nx2Switch(mode, false);
}
[[nodiscard]] inline bool On(bool mt) {
    return mt ? MtSwitch() : PlainSwitch();
}
/// EDEN_NX2_GPUPARAM_CHECK=1 (diagnostic): every deferred call also waits and reads the words
/// as before, runs the macro on them and compares its draws with the indirect draw's.
[[nodiscard]] inline bool CheckOn() {
    static const bool on = Tegra::Nx2EnvMode("EDEN_NX2_GPUPARAM_CHECK") == '1';
    return on;
}
/// Statistics line every 10 s: EDEN_NX2_GPUPARAM_STATS=1, EDEN_NX2_GPU_PROFILE or the check.
[[nodiscard]] inline bool StatsOn() {
    static const bool on = Tegra::Nx2EnvMode("EDEN_NX2_GPUPARAM_STATS") == '1' || CheckOn();
    return on || VideoCommon::Nx2Profile::Enabled();
}

// ------------------------------------------------------------------- 3D registers (word index)
// static_asserted against MAXWELL3D_REG_INDEX in maxwell_3d.cpp.
inline constexpr u32 RegDrawEnd = 0x585;
inline constexpr u32 RegDrawBegin = 0x586;
inline constexpr u32 RegIndexFormat = 0x5f6;
inline constexpr u32 RegIndexFirst = 0x5f7;
inline constexpr u32 RegIndexCount = 0x5f8;
inline constexpr u32 RegVertexIdBase = 0x446;
inline constexpr u32 RegBaseVertex = 0x50d;
inline constexpr u32 RegBaseInstance = 0x50e;
inline constexpr u32 RegMeshControl = 0x114c / 4;
inline constexpr u32 MacroRegistersStart = 0xe00;
inline constexpr u32 MmeDmaRead = 0x157;
inline constexpr u32 MmeDmaReadFifoed = 0x158;

/// Registers whose values the indirect draw replaces (the draw takes them from the words) or
/// does not read: a call may leave them GPU-dependent (the front and the back then both hold
/// the zero-parameter call's values).
[[nodiscard]] constexpr bool IsDrawRegister(u32 method) {
    switch (method) {
    case RegDrawEnd:
    case RegDrawBegin:
    case RegIndexFirst:
    case RegIndexCount:
    case RegVertexIdBase:
    case RegBaseVertex:
    case RegBaseInstance:
        return true;
    default:
        return false;
    }
}

/// draw.begin bits 0-15 (Maxwell3D::Regs::PrimitiveTopology). Quads, quad strips and polygons
/// need the index conversion the indirect path can't do (as HLE_DrawIndexedIndirect's check).
[[nodiscard]] constexpr bool IsTopologySafe(u32 topology) {
    switch (topology) {
    case 0x0:  // Points
    case 0x1:  // Lines
    case 0x2:  // LineLoop
    case 0x3:  // LineStrip
    case 0x4:  // Triangles
    case 0x5:  // TriangleStrip
    case 0x6:  // TriangleFan
    case 0xa:  // LinesAdjacency
    case 0xb:  // LineStripAdjacency
    case 0xc:  // TrianglesAdjacency
    case 0xd:  // TriangleStripAdjacency
    case 0xe:  // Patches
        return true;
    default:
        return false;
    }
}

// --------------------------------------------------------------------------------- the calls

/// One GPFIFO entry of the call's GPU-written parameters.
struct Segment {
    u64 gpu_addr;
    u32 words;
};

enum class Kind : u8 {
    /// p0 (CPU) + {count, instances, first, vertex offset, first instance}.
    IndexedSingle,
    /// p0 (CPU) + draw count + N records of 5 words.
    IndexedMulti,
};

/// What the back draws (trivially copyable: it travels in a GpuSplit::Defer entry).
struct Call {
    u64 records_addr;
    u64 count_addr; ///< 0: no count word (IndexedSingle)
    u32 begin;      ///< p0, the draw.begin word
    u32 max_draws;
    Kind kind;
};

/// Why a call with GPU-written parameters was not deferred (statistics).
enum class Reason : u8 {
    Deferred,
    CpuParams,     ///< not exactly one CPU parameter (the draw.begin word) before the GPU words
    Layout,        ///< GPU words not 5 or 1 + 5N in one or two entries
    InstanceBits,  ///< p0 not a first-instance begin
    Topology,      ///< quads/quad strip/polygon
    ShadowReplay,  ///< shadow RAM replay mode (writes replaced)
    Mesh,          ///< task/mesh pipelines on (the indirect path skips them)
    IndexU8,       ///< 8-bit indices
    NoCode,        ///< not a native Turing macro / code missing
    Rejected,      ///< the macro's code does not match the reference model
    Count,
};
inline constexpr std::array<std::string_view, static_cast<size_t>(Reason::Count)> ReasonNames{
    "deferred", "cpu params", "layout", "instance bits", "topology",
    "shadow replay", "mesh", "u8 indices", "no code", "rejected"};

/// Why the Classifier rejected a macro.
enum class Reject : u8 {
    None,
    RunError,          ///< interpreter error (underflow, invalid op, ...)
    ForbiddenEmit,     ///< macro method or MME DMA read emitted
    ExecutableWrite,   ///< an executable method other than the draw registers
    DrawsDiffer,       ///< draws differ from the reference model
    PlaceholderDraws,  ///< the zero-parameter call draws
    StateAfterDraw,    ///< a non-draw register written after the first draw
    GpuDependentState, ///< non-draw register writes depend on the GPU words
    GpuDependentDram,  ///< data RAM writes depend on the GPU words
    Count,
};
inline constexpr std::array<std::string_view, static_cast<size_t>(Reject::Count)> RejectNames{
    "none", "run error", "forbidden emit", "executable write", "draws differ",
    "placeholder draws", "state after draw", "gpu-dependent state", "gpu-dependent dram"};

// ---------------------------------------------------------------------------- the reference

/// One draw as the 3D engine's draw manager sees it at draw.end.
struct DrawRecord {
    u32 begin;
    u32 count;
    u32 first;
    u32 base_vertex;
    u32 base_instance;
    bool indexed;
    bool operator==(const DrawRecord&) const = default;
};

/// What the indirect draw draws for these GPU words (the reference model): for each record
/// (up to the draw count), `instances` draws with the record's values, the first with p0 as
/// draw.begin, the others with draw.begin's instance id set to Subsequent (1). As the native
/// DrawElementsInstanced macro (Mario Kart World's macro 41, docs/gpuparam-plan.md) does.
inline void ReferenceDraws(Kind kind, u32 begin, std::span<const u32> words, u32 max_draws,
                           std::vector<DrawRecord>& out, u32 instance_cap = 64) {
    out.clear();
    const auto record = [&](std::span<const u32> r) {
        const u32 instances = std::min(r[1], instance_cap);
        for (u32 i = 0; i < instances; ++i) {
            const u32 b = i == 0 ? begin : (begin & ~(3U << 26)) | (1U << 26);
            out.push_back({b, r[0], r[2], r[3], r[4], true});
        }
    };
    if (kind == Kind::IndexedSingle) {
        if (words.size() >= 5) {
            record(words.subspan(0, 5));
        }
        return;
    }
    if (words.empty()) {
        return;
    }
    const u32 draws = std::min(words[0], max_draws);
    for (u32 k = 0; k < draws && 1 + size_t{k} * 5 + 5 <= words.size(); ++k) {
        record(words.subspan(1 + size_t{k} * 5, 5));
    }
}

/// What a macro run did, by the register writes it emitted.
struct Trace {
    bool ok{};
    std::string_view error;
    bool forbidden{};
    bool too_many_draws{};
    bool executable_other{};
    bool other_after_draw{};
    bool read_state{};
    bool read_dram{};
    std::vector<DrawRecord> draws;
    std::vector<std::pair<u32, u32>> other_writes; ///< non-draw registers, in order
    std::vector<std::pair<u32, u32>> dram_writes;
};

using DataRam = Tegra::TuringMacroInterpreter::DataRam;

/// Runs native macro code (`code`: the whole instruction RAM, `start`: the macro's first
/// instruction) on `params` in a scratch interpreter: `regs` is the register file the macro's
/// state reads see (its own writes on top), `dram` a copy of the data RAM it may change,
/// `executable` whether a method has side effects beyond the register (execution_mask).
template <typename Executable>
void RunTrace(Tegra::TuringMacroInterpreter& interp, std::span<const u32> code, u32 start,
              std::vector<u32>& params, std::span<const u32> regs, DataRam& dram,
              Executable&& executable, Trace& t, u32 draw_cap = 4096) {
    t = Trace{};
    std::vector<std::pair<u32, u32>> overlay;
    overlay.reserve(32);
    const auto current = [&](u32 method) -> u32 {
        for (auto it = overlay.rbegin(); it != overlay.rend(); ++it) {
            if (it->first == method) {
                return it->second;
            }
        }
        return method < regs.size() ? regs[method] : 0U;
    };
    bool indexed_pending = false;
    interp.active_dram = &dram;
    const auto result = interp.Execute(
        code, start, params,
        [&](u32 address) {
            t.read_state = true;
            return address < regs.size() ? current(address) : 0U;
        },
        [&](u32 method, u32 value) {
            if (method >= MacroRegistersStart || method == MmeDmaRead ||
                method == MmeDmaReadFifoed) {
                t.forbidden = true;
                return false;
            }
            overlay.emplace_back(method, value);
            if (method == RegIndexCount) {
                indexed_pending = true;
            }
            if (method == RegDrawEnd) {
                if (t.draws.size() >= draw_cap) {
                    t.too_many_draws = true;
                    return false;
                }
                const u32 begin = current(RegDrawBegin);
                t.draws.push_back({begin, current(RegIndexCount), current(RegIndexFirst),
                                   current(RegBaseVertex), current(RegBaseInstance),
                                   indexed_pending});
                // DrawManager::DrawEnd clears draw_indexed after a first-instance draw.
                if (((begin >> 26) & 3) == 0) {
                    indexed_pending = false;
                }
                return true;
            }
            if (!IsDrawRegister(method)) {
                if (executable(method)) {
                    t.executable_other = true;
                }
                if (!t.draws.empty()) {
                    t.other_after_draw = true;
                }
                t.other_writes.emplace_back(method, value);
            }
            return true;
        });
    t.ok = result.success;
    t.error = result.error;
    t.read_dram = !interp.data_ram_reads.empty();
    t.dram_writes = interp.data_ram_writes;
}

/// FNV-1a over macro code words (the list dump's hash).
[[nodiscard]] inline u64 HashCode(std::span<const u32> code) {
    u64 hash = 0xcbf29ce484222325ULL;
    for (const u32 word : code) {
        hash = (hash ^ word) * 0x100000001b3ULL;
    }
    return hash;
}

/// The macro's code words: from `start` to the next macro's start (at most 256 instructions),
/// as the list dump's lookup.
[[nodiscard]] inline std::span<const u32> MacroCode(std::span<const u32> code_ram,
                                                    std::span<const u32> positions, u32 start) {
    u32 end = start + 256;
    for (const u32 position : positions) {
        if (position > start && position < end) {
            end = position;
        }
    }
    const size_t first = std::min<size_t>(size_t{start} * 3, code_ram.size());
    const size_t last = std::min<size_t>(size_t{end} * 3, code_ram.size());
    return code_ram.subspan(first, last - first);
}

// --------------------------------------------------------------------------- the classifier

/// Decides whether a macro call with GPU-written parameters is exactly the reference indirect
/// draw. The macro runs on test words (zeros first: the placeholder call the front makes) and
/// every run must: finish, emit no macro/DMA-read method and no executable method but the draw
/// registers, draw exactly what ReferenceDraws gives, write the other registers and the data RAM
/// the same in every run (independent of the GPU words) and none of them after a draw. The
/// verdict is cached per (macro start, code hash, kind, N, p0) unless the runs read registers or
/// the data RAM (then every call is checked against the current state).
class Classifier {
public:
    struct Verdict {
        bool ok{};
        Reject reject{};
        bool cached{};
    };

    template <typename Executable>
    Verdict Verify(std::span<const u32> code_ram, std::span<const u32> positions, u32 index,
                   u64 generation, Kind kind, u32 begin, u32 max_draws,
                   std::span<const u32> regs, const DataRam& dram, Executable&& executable) {
        const u32 start = positions[index % positions.size()];
        const u64 code_hash = CodeHash(code_ram, positions, start, generation);
        const u64 key = Key(start, code_hash, kind, max_draws, begin);
        if (const auto it = verdicts.find(key); it != verdicts.end()) {
            return {it->second.ok, it->second.reject, true};
        }
        const auto [reject, stateful] =
            Run(code_ram, start, kind, begin, max_draws, regs, dram, executable);
        const bool ok = reject == Reject::None;
        if (!stateful) {
            verdicts.emplace(key, Entry{ok, reject});
        }
        if (logged.insert(code_hash ^ (u64{static_cast<u8>(kind)} << 60) ^ (ok ? 1 : 0)).second) {
            const auto code = MacroCode(code_ram, positions, start);
            size_t used = code.size();
            while (used >= 3 && code[used - 1] == 0 && code[used - 2] == 0 && code[used - 3] == 0) {
                used -= 3; // trailing empty instructions (last macro of the RAM)
            }
            std::string words;
            for (size_t i = 0; i < used; ++i) {
                words += fmt::format("{}{:x}", i ? "," : "", code[i]);
            }
            LOG_INFO(HW_GPU,
                     "GPU params: macro {} start={:x} hash={:016x} {} N={} begin={:#x}: {}{}{} "
                     "code={}",
                     index, start, HashCode(code), kind == Kind::IndexedSingle ? "single" : "multi",
                     max_draws, begin, ok ? "deferrable" : "rejected (",
                     ok ? "" : RejectNames[static_cast<size_t>(reject)], ok ? "" : ")", words);
            if (!ok && !last_error.empty()) {
                LOG_INFO(HW_GPU, "GPU params: macro {} run error: {}", index, last_error);
            }
        }
        return {ok, reject, false};
    }

    /// EDEN_NX2_GPUPARAM_CHECK: the macro's draws for the real words against the reference's.
    /// Returns whether they match (`actual`/`expected` hold both).
    template <typename Executable>
    bool Compare(std::span<const u32> code_ram, u32 start, Kind kind, u32 begin, u32 max_draws,
                 std::span<const u32> words, std::span<const u32> regs, const DataRam& dram,
                 Executable&& executable, std::vector<DrawRecord>& actual,
                 std::vector<DrawRecord>& expected, std::string_view& error) {
        params.assign(1, begin);
        params.insert(params.end(), words.begin(), words.end());
        scratch_dram = dram;
        RunTrace(interp, code_ram, start, params, regs, scratch_dram, executable, trace, 1U << 20);
        error = trace.error;
        actual = trace.draws;
        ReferenceDraws(kind, begin, words, max_draws, expected, 1U << 20);
        return trace.ok && actual == expected;
    }

private:
    struct Entry {
        bool ok;
        Reject reject;
    };

    static u64 Key(u32 start, u64 code_hash, Kind kind, u32 max_draws, u32 begin) {
        u64 key = code_hash;
        for (const u64 v : {u64{start}, u64{static_cast<u8>(kind)}, u64{max_draws}, u64{begin}}) {
            key = (key ^ v) * 0x100000001b3ULL;
        }
        return key;
    }

    u64 CodeHash(std::span<const u32> code_ram, std::span<const u32> positions, u32 start,
                 u64 generation) {
        auto& slot = code_hashes[start];
        if (!slot.valid || slot.generation != generation) {
            slot.hash = HashCode(MacroCode(code_ram, positions, start));
            slot.generation = generation;
            slot.valid = true;
        }
        return slot.hash;
    }

    /// GPU words of test vector `v` (0: all zero, the placeholder call).
    static void TestWords(Kind kind, u32 max_draws, u32 v, std::vector<u32>& out) {
        static constexpr std::array<std::array<u32, 5>, 7> singles{{
            {0, 0, 0, 0, 0},
            {3, 1, 0, 0, 0},
            {0x1234, 2, 0x56, 0x789a, 0x11},
            {0x9c, 3, 0x40, 0xf3, 0x10},
            {0x00ffff01, 1, 0x00abcdef, 0xfffffff0, 0x00012345},
            {7, 0, 0x33, 0x44, 0x55},
            {0x80000001, 2, 0x7fffffff, 0x80000000, 0xffff},
        }};
        out.clear();
        if (kind == Kind::IndexedSingle) {
            out.assign(singles[v].begin(), singles[v].end());
            return;
        }
        out.assign(1 + size_t{max_draws} * 5, 0);
        if (v == 0) {
            return;
        }
        static constexpr std::array<u32, 7> counts{0, 1, 2, 3, 0xffffffff, 1, 0};
        out[0] = v == 4 ? max_draws : counts[v];
        if (v == 6) {
            out[0] = std::min<u32>(max_draws, 5);
        }
        for (u32 k = 0; k < max_draws; ++k) {
            u32* r = out.data() + 1 + size_t{k} * 5;
            const auto& s = singles[1 + (k + v) % 6];
            r[0] = s[0] + k;
            r[1] = v == 4 ? (k % 3 == 0 ? 1U : 0U) : s[1];
            r[2] = s[2] + 0x10 * k;
            r[3] = s[3] - 5 * k;
            r[4] = s[4] + k;
        }
    }

    template <typename Executable>
    std::pair<Reject, bool> Run(std::span<const u32> code_ram, u32 start, Kind kind, u32 begin,
                                u32 max_draws, std::span<const u32> regs, const DataRam& dram,
                                Executable& executable) {
        last_error = {};
        bool stateful = false;
        std::vector<std::pair<u32, u32>> base_writes;
        std::vector<std::pair<u32, u32>> base_dram;
        for (u32 v = 0; v < 7; ++v) {
            TestWords(kind, max_draws, v, words);
            params.assign(1, begin);
            params.insert(params.end(), words.begin(), words.end());
            scratch_dram = dram;
            RunTrace(interp, code_ram, start, params, regs, scratch_dram, executable, trace);
            stateful |= trace.read_state || trace.read_dram;
            if (!trace.ok) {
                last_error = trace.error;
                return {trace.forbidden        ? Reject::ForbiddenEmit
                        : trace.too_many_draws ? Reject::DrawsDiffer
                                               : Reject::RunError,
                        stateful};
            }
            if (trace.forbidden) {
                return {Reject::ForbiddenEmit, stateful};
            }
            if (trace.executable_other) {
                return {Reject::ExecutableWrite, stateful};
            }
            if (trace.other_after_draw) {
                return {Reject::StateAfterDraw, stateful};
            }
            ReferenceDraws(kind, begin, words, max_draws, expected);
            if (trace.draws != expected) {
                return {v == 0 && !trace.draws.empty() ? Reject::PlaceholderDraws
                                                      : Reject::DrawsDiffer,
                        stateful};
            }
            if (v == 0) {
                base_writes = trace.other_writes;
                base_dram = trace.dram_writes;
            } else if (trace.other_writes != base_writes) {
                return {Reject::GpuDependentState, stateful};
            } else if (trace.dram_writes != base_dram) {
                return {Reject::GpuDependentDram, stateful};
            }
        }
        return {Reject::None, stateful};
    }

    struct CodeSlot {
        u64 generation{};
        u64 hash{};
        bool valid{};
    };
    std::unordered_map<u32, CodeSlot> code_hashes;
    std::unordered_map<u64, Entry> verdicts;
    std::unordered_set<u64> logged;
    Tegra::TuringMacroInterpreter interp;
    DataRam scratch_dram{};
    Trace trace;
    std::vector<u32> params;
    std::vector<u32> words;
    std::vector<DrawRecord> expected;
    std::string_view last_error;
};

// ------------------------------------------------------------------------------- statistics

struct Stats {
    std::array<u64, static_cast<size_t>(Reason::Count)> reasons{};
    std::array<u64, static_cast<size_t>(Reject::Count)> rejects{};
    u64 single{};
    u64 multi{};
    u64 entries{};        ///< GPFIFO entries not read
    u64 verified{};       ///< classifier runs (not cached)
    u64 check_compared{};
    u64 check_mismatch{};
    u64 check_unreadable{};
    u64 check_draws{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();

    void Note(Reason reason) {
        ++reasons[static_cast<size_t>(reason)];
    }

    void MaybeLog() {
        if (!StatsOn()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::seconds{10}) {
            return;
        }
        window = now;
        std::string text;
        for (size_t i = 1; i < reasons.size(); ++i) {
            if (reasons[i] != 0) {
                text += fmt::format(" {} {},", ReasonNames[i], reasons[i]);
            }
        }
        std::string rejected;
        for (size_t i = 1; i < rejects.size(); ++i) {
            if (rejects[i] != 0) {
                rejected += fmt::format(" {} {},", RejectNames[i], rejects[i]);
            }
        }
        LOG_INFO(HW_GPU,
                 "GPU params in 10 s: {} deferred ({} single, {} multi; {} entries not read), "
                 "kept waiting:{}{} rejects:{} verified {}; check {} compared, {} mismatched, "
                 "{} unreadable, {} draws",
                 reasons[0], single, multi, entries, text.empty() ? " none" : text, "",
                 rejected.empty() ? " none" : rejected, verified, check_compared, check_mismatch,
                 check_unreadable, check_draws);
        const auto keep = window;
        *this = Stats{};
        window = keep;
    }
};

} // namespace VideoCommon::MT::GpuParam
