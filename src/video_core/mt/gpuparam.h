// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
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

/// EDEN_NX2_MT_LITE_GPUPARAM (=0/1/a): off by default (randomized A/B with MT lite: -8.6 +/- 4.7 fps;
/// each deferred call's constant-buffer GPU copy is a Serial at ~10k calls/s). Was on under EDEN_NX2_MT_LITE=1 (with the split
/// front running macros). Read for every call ('a' alternates).
[[nodiscard]] inline bool MtSwitch() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_LITE_GPUPARAM");
    return Enabled() && Tegra::Nx2Switch(mode, false);
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
/// const_buffer: size, address high/low, offset, then 16 data registers (each data word goes
/// to address + offset and advances the offset by 4: Maxwell3D::ProcessCBMultiData).
inline constexpr u32 RegCbSize = 0x8e0;
inline constexpr u32 RegCbAddressLow = 0x8e2;
inline constexpr u32 RegCbOffset = 0x8e3;
inline constexpr u32 RegCbData = 0x8e4;
inline constexpr u32 RegCbDataEnd = RegCbData + 16;
[[nodiscard]] constexpr bool IsCbData(u32 method) {
    return method >= RegCbData && method < RegCbDataEnd;
}

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

/// A constant buffer word the call writes (at const_buffer address + `offset`): `value`, or the
/// call's GPU word `gpu_word` (index into its GPU-written words) when that is not NoGpuWord.
struct CbWord {
    u32 offset;
    u32 value;
    u8 gpu_word;
};
inline constexpr u8 NoGpuWord = 0xff;
inline constexpr size_t MaxCbWords = 8;

/// The call's constant buffer writes around its draws: `pre` what the draws see (written before
/// the first draw), `post` what the call leaves after them (offsets also in `pre`). The zero-word
/// call has already written every word (with zeros for GPU words) before the back's draw.
struct CbPlan {
    std::array<CbWord, MaxCbWords> pre{};
    std::array<CbWord, MaxCbWords> post{};
    u8 pre_count{};
    u8 post_count{};
};

/// Constant buffer words the back could not copy on the GPU (BufferCopy false: copied from
/// guest memory instead, possibly stale). Back thread; logged by Stats.
inline std::atomic<u64> g_cb_copy_fallbacks{};

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
    CbPlan cb;
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
    CbLayout,          ///< constant buffer writes: offset from state, address changed, offsets
                       ///< differ between runs, or too many words
    CbValue,           ///< a constant buffer word neither constant nor a GPU word
    CbBetweenDraws,    ///< a constant buffer word written between draws (or phases differ)
    CbPostOnly,        ///< a word written after the draws only (its old value is gone)
    CbGpuMulti,        ///< GPU words to the constant buffer in a multi-draw call
    Count,
};
inline constexpr std::array<std::string_view, static_cast<size_t>(Reject::Count)> RejectNames{
    "none", "run error", "forbidden emit", "executable write", "draws differ",
    "placeholder draws", "state after draw", "gpu-dependent state", "gpu-dependent dram",
    "cb layout", "cb value", "cb between draws", "cb post only", "cb gpu multi"};

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
    bool cb_offset_from_state{}; ///< constant buffer data before the macro set the offset
    bool cb_address_write{};     ///< the macro wrote const_buffer size/address
    struct CbWrite {
        u32 offset;
        u32 value;
        u32 draws_before;
    };
    std::vector<CbWrite> cb_writes;
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
    bool cb_offset_known = false;
    u32 cb_offset = RegCbOffset < regs.size() ? regs[RegCbOffset] : 0U;
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
            if (IsCbData(method)) {
                // ProcessCBMultiData: the word at address + offset, the offset advances.
                t.cb_offset_from_state |= !cb_offset_known;
                t.cb_writes.push_back({cb_offset, value, static_cast<u32>(t.draws.size())});
                cb_offset += 4;
                overlay.emplace_back(RegCbOffset, cb_offset);
                return true;
            }
            if (method == RegCbOffset) {
                cb_offset = value;
                cb_offset_known = true;
                overlay.emplace_back(method, value);
                t.other_writes.emplace_back(method, value); // must not depend on the GPU words
                return true;
            }
            if (method >= RegCbSize && method < RegCbOffset) {
                t.cb_address_write = true;
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
        CbPlan cb{};
    };

    template <typename Executable>
    Verdict Verify(std::span<const u32> code_ram, std::span<const u32> positions, u32 index,
                   u64 generation, Kind kind, u32 begin, u32 max_draws,
                   std::span<const u32> regs, const DataRam& dram, Executable&& executable) {
        const u32 start = positions[index % positions.size()];
        const u64 code_hash = CodeHash(code_ram, positions, start, generation);
        const u64 key = Key(start, code_hash, kind, max_draws, begin);
        if (const auto it = verdicts.find(key); it != verdicts.end()) {
            return {it->second.ok, it->second.reject, true, it->second.cb};
        }
        CbPlan cb{};
        const auto [reject, stateful] =
            Run(code_ram, start, kind, begin, max_draws, regs, dram, executable, cb);
        const bool ok = reject == Reject::None;
        if (!stateful) {
            verdicts.emplace(key, Entry{ok, reject, cb});
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
            if (ok && (cb.pre_count != 0 || cb.post_count != 0)) {
                std::string text;
                const auto add = [&text](const char* phase, const CbWord& w) {
                    text += w.gpu_word == NoGpuWord
                                ? fmt::format(" {} +{:#x}={:#x}", phase, w.offset, w.value)
                                : fmt::format(" {} +{:#x}=word{}", phase, w.offset, w.gpu_word);
                };
                for (u8 i = 0; i < cb.pre_count; ++i) {
                    add("pre", cb.pre[i]);
                }
                for (u8 i = 0; i < cb.post_count; ++i) {
                    add("post", cb.post[i]);
                }
                LOG_INFO(HW_GPU, "GPU params: macro {} constant buffer words:{}", index, text);
            }
        }
        return {ok, reject, false, cb};
    }

    /// EDEN_NX2_GPUPARAM_CHECK: the macro's draws for the real words against the reference's.
    /// Returns whether they match (`actual`/`expected` hold both).
    template <typename Executable>
    bool Compare(std::span<const u32> code_ram, u32 start, Kind kind, u32 begin, u32 max_draws,
                 const CbPlan& cb, std::span<const u32> words, std::span<const u32> regs,
                 const DataRam& dram, Executable&& executable, std::vector<DrawRecord>& actual,
                 std::vector<DrawRecord>& expected, std::string_view& error, bool& cb_same) {
        params.assign(1, begin);
        params.insert(params.end(), words.begin(), words.end());
        scratch_dram = dram;
        RunTrace(interp, code_ram, start, params, regs, scratch_dram, executable, trace, 1U << 20);
        error = trace.error;
        actual = trace.draws;
        ReferenceDraws(kind, begin, words, max_draws, expected, 1U << 20);
        // The constant buffer words the draws saw and the call left, against the plan's.
        cb_same = true;
        if (!trace.draws.empty()) {
            const auto resolve = [&words](const CbWord& w) {
                return w.gpu_word == NoGpuWord ? w.value
                       : w.gpu_word < words.size() ? words[w.gpu_word]
                                                   : 0U;
            };
            const auto net = [this](u32 offset, bool before_draws, u32& out) {
                bool found = false;
                for (const auto& write : trace.cb_writes) {
                    if (write.offset == offset && (!before_draws || write.draws_before == 0)) {
                        out = write.value;
                        found = true;
                    }
                }
                return found;
            };
            for (u8 i = 0; i < cb.pre_count; ++i) {
                u32 value{};
                cb_same &= net(cb.pre[i].offset, true, value) && value == resolve(cb.pre[i]);
            }
            for (u8 i = 0; i < cb.post_count; ++i) {
                u32 value{};
                cb_same &= net(cb.post[i].offset, false, value) && value == resolve(cb.post[i]);
            }
            size_t pre_writes = 0;
            for (const auto& write : trace.cb_writes) {
                pre_writes += write.draws_before == 0 ? 1 : 0;
            }
            cb_same &= (pre_writes == 0) == (cb.pre_count == 0);
        }
        return trace.ok && actual == expected && cb_same;
    }

private:
    struct Entry {
        bool ok;
        Reject reject;
        CbPlan cb;
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
                                Executable& executable, CbPlan& cb) {
        last_error = {};
        bool stateful = false;
        std::vector<std::pair<u32, u32>> base_writes;
        std::vector<std::pair<u32, u32>> base_dram;
        std::array<std::vector<Trace::CbWrite>, 7> cb_runs;
        std::array<std::array<u32, 5>, 7> single_words{};
        std::array<size_t, 7> draw_counts{};
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
            if (!trace.cb_writes.empty() &&
                (trace.cb_offset_from_state || trace.cb_address_write)) {
                return {Reject::CbLayout, stateful};
            }
            cb_runs[v] = std::move(trace.cb_writes);
            draw_counts[v] = trace.draws.size();
            if (kind == Kind::IndexedSingle) {
                std::copy_n(words.begin(), 5, single_words[v].begin());
            }
        }
        return {PlanConstantBuffer(kind, cb_runs, single_words, draw_counts, cb), stateful};
    }

    /// The constant buffer words of the runs as a plan: the same offsets in every run, each value
    /// a constant or one GPU word in every run, written before the first draw or after the last.
    static Reject PlanConstantBuffer(Kind kind,
                                     const std::array<std::vector<Trace::CbWrite>, 7>& runs,
                                     const std::array<std::array<u32, 5>, 7>& single_words,
                                     const std::array<size_t, 7>& draw_counts, CbPlan& cb) {
        cb = CbPlan{};
        const auto& first = runs[0];
        if (first.empty()) {
            return Reject::None;
        }
        if (first.size() > 2 * MaxCbWords) {
            return Reject::CbLayout;
        }
        for (const auto& run : runs) {
            if (run.size() != first.size()) {
                return Reject::CbLayout;
            }
            for (size_t j = 0; j < run.size(); ++j) {
                if (run[j].offset != first[j].offset) {
                    return Reject::CbLayout;
                }
            }
        }
        for (size_t j = 0; j < first.size(); ++j) {
            CbWord word{first[j].offset, first[j].value, NoGpuWord};
            const bool constant = std::ranges::all_of(
                runs, [&](const auto& run) { return run[j].value == first[j].value; });
            if (!constant) {
                if (kind != Kind::IndexedSingle) {
                    return Reject::CbGpuMulti;
                }
                for (u8 k = 0; k < 5 && word.gpu_word == NoGpuWord; ++k) {
                    bool all = true;
                    for (size_t v = 0; v < runs.size(); ++v) {
                        all &= runs[v][j].value == single_words[v][k];
                    }
                    if (all) {
                        word.gpu_word = k;
                        word.value = 0;
                    }
                }
                if (word.gpu_word == NoGpuWord) {
                    return Reject::CbValue;
                }
            }
            // Phase from the runs that draw: before every draw or after all of them.
            int phase = -1;
            for (size_t v = 0; v < runs.size(); ++v) {
                if (draw_counts[v] == 0) {
                    continue;
                }
                const u32 before = runs[v][j].draws_before;
                const int this_phase = before == 0 ? 0 : before == draw_counts[v] ? 1 : 2;
                if (this_phase == 2 || (phase != -1 && phase != this_phase)) {
                    return Reject::CbBetweenDraws;
                }
                phase = this_phase;
            }
            if (phase == -1) {
                return Reject::CbBetweenDraws; // no run drew: phases unknown
            }
            auto& list = phase == 0 ? cb.pre : cb.post;
            u8& count = phase == 0 ? cb.pre_count : cb.post_count;
            if (phase == 1 && std::ranges::none_of(
                                  std::span{cb.pre.data(), cb.pre_count},
                                  [&](const CbWord& w) { return w.offset == word.offset; })) {
                return Reject::CbPostOnly;
            }
            const auto same = std::ranges::find_if(std::span{list.data(), count},
                                                   [&](const CbWord& w) {
                                                       return w.offset == word.offset;
                                                   });
            if (same != std::span{list.data(), count}.end()) {
                *same = word; // the last write of an offset wins
            } else if (count == MaxCbWords) {
                return Reject::CbLayout;
            } else {
                list[count++] = word;
            }
        }
        return Reject::None;
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
                 "{} unreadable, {} draws; constant buffer copies from guest memory {}",
                 reasons[0], single, multi, entries, text.empty() ? " none" : text, "",
                 rejected.empty() ? " none" : rejected, verified, check_compared, check_mismatch,
                 check_unreadable, check_draws,
                 g_cb_copy_fallbacks.exchange(0, std::memory_order_relaxed));
        const auto keep = window;
        *this = Stats{};
        window = keep;
    }
};

} // namespace VideoCommon::MT::GpuParam
