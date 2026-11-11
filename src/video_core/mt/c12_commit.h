// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/c13_memo.h"
#include "video_core/mt/lite7_capture_switches.h"
#include "video_core/mt/mt_switches.h"
#include "video_core/nx2.h"

/// Cycle 12, workstream W2 of docs/cycle12-plan.md: the committer's whole-draw repeat. A draw
/// packet whose binding state is the same as the previous draw of its pipeline skips the
/// per-binding resolution of ConfigureImpl:
///  - texture half: the cycle 11 REPEAT (TextureMemo by packet, TryRepeatGraphicsBindings),
///    turned on by this switch (C11RepeatMode() returns '1' unless EDEN_NX2_C11_REPEAT=0);
///  - buffer half (EDEN_NX2_C12_BUFFER_REPEAT): the pipeline's C12GraphicsBufferMemo keeps the
///    buffer ids UpdateGraphicsBuffers resolved for its uniform and storage bindings; a draw
///    whose bindings (device address, GPU address, size, enabled/written masks) equal the
///    memo's under the same channel, memory manager, odd page table layout generation, buffer
///    deletion epoch and c12 buffer epoch (bumped by every buffer creation and deletion) takes
///    the ids without FindBuffer / ResolveStorageBinding; index and vertex buffers still go
///    through their own (memoized) updates;
///  - uniform bind memo (EDEN_NX2_C12_UBIND_MEMO): BindGraphicsUniformBuffer of a (gpu address,
///    size) the slot had before under the same layout generation and epochs reuses the device
///    address and the resolved buffer id (no GpuToCpuAddress, and UpdateUniformBuffers finds the
///    binding already resolved: no FindBuffer);
///  - index memo (EDEN_NX2_C12_INDEX_MEMO): the existing EDEN_NX2_INDEX_MEMO logic, on.
/// What still runs every draw: the uploads (MtApplyUploads), the per-stage binds
/// (BindHostStageBuffers: streamed uniforms, descriptor entries, MarkUsage, storage write
/// marks; SynchronizeBuffer is a no-op on the committer, the packet carries the bytes), render
/// targets, the descriptor update and the draw record.
///
/// Switch EDEN_NX2_C12_COMMIT_REPEAT: on by default under EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1, '0'
/// off (the cycle 11 path, C11 REPEAT back to its own switch), '1' on without LITE (still only
/// with EDEN_NX2_MT), 'a' alternating (decided per draw at commit; the memos are kept in both
/// halves). Each sub-piece can be turned off alone with its own switch '=0'.
namespace VideoCommon::MT {

[[nodiscard]] inline char C12CommitRepeatMode() {
    static const char mode = Lite7Detail::Mode("EDEN_NX2_C12_COMMIT_REPEAT");
    return mode;
}

namespace C12Detail {
/// A sub-piece: off when the main switch is off or the piece's own variable is '0'.
[[nodiscard]] inline bool Sub(const char* name) {
    return C12CommitRepeatMode() != '0' && Tegra::Nx2EnvMode(name) != '0';
}
} // namespace C12Detail

/// Whether this draw takes the repeat paths (per draw for 'a').
[[nodiscard]] inline bool C12CommitRepeatDraw() {
    return Lite7Detail::On(C12CommitRepeatMode());
}

/// EDEN_NX2_C12_BUFFER_REPEAT=0: no buffer half.
[[nodiscard]] inline bool C12BufferRepeatEnabled() {
    static const bool on = C12Detail::Sub("EDEN_NX2_C12_BUFFER_REPEAT");
    return on;
}

/// EDEN_NX2_C12_TEXTURE_REPEAT=0: no texture half (C11 REPEAT keeps its own switch then).
[[nodiscard]] inline bool C12TextureRepeatEnabled() {
    static const bool on = C12Detail::Sub("EDEN_NX2_C12_TEXTURE_REPEAT");
    return on;
}

/// EDEN_NX2_C12_UBIND_MEMO=0: no uniform bind memo.
[[nodiscard]] inline bool C12UbindMemoEnabled() {
    static const bool on = C12Detail::Sub("EDEN_NX2_C12_UBIND_MEMO");
    return on;
}
[[nodiscard]] inline bool C12UbindMemoOn() {
    return C12UbindMemoEnabled() && C12CommitRepeatDraw();
}

/// EDEN_NX2_C12_INDEX_MEMO=0: the index memo only with EDEN_NX2_INDEX_MEMO.
[[nodiscard]] inline bool C12IndexMemoEnabled() {
    static const bool on = C12Detail::Sub("EDEN_NX2_C12_INDEX_MEMO");
    return on;
}
[[nodiscard]] inline bool C12IndexMemoOn() {
    return C12IndexMemoEnabled() && C12CommitRepeatDraw();
}

/// EDEN_NX2_C12_COMMIT_REPEAT_CHECK=1 (diagnostic, off): every repeat the buffer half or the
/// uniform bind memo would take runs the full path too and the results are compared (buffer
/// ids, device addresses, sizes, segment counts); also turns on EDEN_NX2_C11_REPEAT_CHECK for
/// the texture half (view/sampler ids, descriptor entries, rescaling words). Mismatches must
/// stay 0 ("C12 commit repeat check" ERROR lines, counted in the stats line).
[[nodiscard]] inline bool C12CommitRepeatCheck() {
    // Cycle 13 (mt/c13_memo.h): EDEN_NX2_C13_MEMO_CHECK=1 turns it on too (it covers every way).
    static const bool on = C12CommitRepeatMode() != '0' &&
                           (Tegra::Nx2EnvMode("EDEN_NX2_C12_COMMIT_REPEAT_CHECK") == '1' ||
                            C13MemoCheck());
    return on;
}

/// EDEN_NX2_C12_STATS=1 (or EDEN_NX2_C12_COMMIT_REPEAT_STATS=1 / EDEN_NX2_MT_STATS=1): one 10 s
/// line "C12 commit repeat" from the committer (also turns on the C11 repeat line).
[[nodiscard]] inline bool C12CommitStats() {
    static const bool on = Enabled() && (Tegra::Nx2EnvMode("EDEN_NX2_C12_STATS") == '1' ||
                                         Tegra::Nx2EnvMode("EDEN_NX2_C12_COMMIT_REPEAT_STATS") == '1' ||
                                         Tegra::Nx2EnvMode("EDEN_NX2_MT_STATS") == '1');
    return on;
}

/// W3's packet flag (mt/c12_capture.h, docs/cycle12-hooks-W2.md): DrawHeader::c12_repeat, the
/// seq distance to the earlier packet of the same pipeline whose capture pass 1 (TIC/TSC
/// entries, handles, storage descriptors, same tables/limits/layout) was byte-identical (0:
/// none), and c12_flags (MT::C12PacketFlags, bit 0 C12SameReads). The texture half uses it as
/// repeat_of only with C12SameReads (pass 1 reads identical = the texture inputs the C11 key
/// holds are identical). Read through templates so this header needs neither W3's header nor its
/// fields; a header without c12_flags counts as "same reads" when c12_repeat is set.
template <typename Header>
[[nodiscard]] inline u64 C12SameAsDelta(const Header& header) {
    if constexpr (requires { header.c12_repeat; }) {
        if constexpr (requires { header.c12_flags; }) {
            if ((static_cast<u32>(header.c12_flags) & 1u) == 0) {
                return 0;
            }
        }
        return static_cast<u64>(header.c12_repeat);
    } else if constexpr (requires { header.c12_same_as; }) {
        return static_cast<u64>(header.c12_same_as);
    } else {
        return 0;
    }
}

/// ParsePacket (compact layout): copies W3's flag and its bits from the compact header when both
/// headers have them.
template <typename To, typename From>
inline void C12CopyRepeatFlag(To& to, const From& from) {
    if constexpr (requires { to.c12_repeat = from.c12_repeat; }) {
        to.c12_repeat = from.c12_repeat;
    } else if constexpr (requires { to.c12_same_as = from.c12_same_as; }) {
        to.c12_same_as = from.c12_same_as;
    }
    if constexpr (requires { to.c12_flags = from.c12_flags; }) {
        to.c12_flags = from.c12_flags;
    }
}

/// Counters (relaxed atomics, written by the committer only).
struct C12CommitCounters {
    std::atomic<u64> draws{};          ///< packets committed with the switch on for the draw
    std::atomic<u64> w3_flagged{};     ///< packets with W3's c12_repeat
    std::atomic<u64> w3_texture{};     ///< ... that gave the texture half its repeat_of
    // Buffer half.
    std::atomic<u64> buf_tries{};      ///< UpdateGraphicsBuffers calls through the memo
    std::atomic<u64> buf_taken{};
    std::atomic<u64> buf_no_memo{};    ///< memo empty / not reusable when saved
    std::atomic<u64> buf_state{};      ///< channel / layout / epochs / tfb / indirect moved
    std::atomic<u64> buf_masks{};      ///< enabled or written masks differ
    std::atomic<u64> buf_binding{};    ///< a binding's address / size / id differs
    std::atomic<u64> buf_fallback{};   ///< index/vertex update created or deleted a buffer
    std::atomic<u64> buf_saved{};      ///< memo (re)saved after the full path
    // Uniform bind memo.
    std::atomic<u64> ubind_binds{};
    std::atomic<u64> ubind_hits{};         ///< translations reused (no GpuToCpuAddress)
    std::atomic<u64> ubind_resolve_hits{}; ///< resolutions reused (no FindBuffer)
    std::atomic<u64> ubind_fills{};        ///< resolutions kept
    // Index memo.
    std::atomic<u64> index_hits{};
    // CHECK.
    std::atomic<u64> checks{};
    std::atomic<u64> check_mismatches{};
};

inline C12CommitCounters g_c12_commit{};

/// Counts when stats or checks are on (one branch on a static).
[[nodiscard]] inline bool C12Count() {
    static const bool on = C12CommitStats() || C12CommitRepeatCheck();
    return on;
}
inline void C12Add(std::atomic<u64>& counter, u64 value = 1) {
    if (C12Count()) {
        counter.fetch_add(value, std::memory_order_relaxed);
    }
}

/// A CHECK mismatch: counted, and the first 16 logged.
inline void C12CheckMismatch(const char* what, u64 a, u64 b) {
    g_c12_commit.check_mismatches.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<u32> reported{};
    if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
        LOG_ERROR(HW_GPU, "C12 commit repeat check: {} differs (repeat {:#x}, full {:#x})", what, a,
                  b);
    }
}

/// Start line (once, with the cycle 11 one, when the committer starts).
inline void C12CommitLogStart() {
    if (!Enabled()) {
        return;
    }
    C13MemoLogStart();
    LOG_INFO(HW_GPU,
             "Cycle 12 committer: commit repeat {} (texture {}, buffer {}, ubind memo {}, index "
             "memo {}), check {}, stats {}",
             Lite7Detail::Name(C12CommitRepeatMode()), C12TextureRepeatEnabled(),
             C12BufferRepeatEnabled(), C12UbindMemoEnabled(), C12IndexMemoEnabled(),
             C12CommitRepeatCheck(), C12CommitStats());
}

/// Committer: logs and resets the counters every 10 s.
inline void C12CommitMaybeLog() {
    if (!C12Count()) {
        return;
    }
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds{10}) {
        return;
    }
    last = now;
    C12CommitCounters& c = g_c12_commit;
    const auto take = [](std::atomic<u64>& value) {
        return value.exchange(0, std::memory_order_relaxed);
    };
    const u64 draws = take(c.draws);
    const u64 w3 = take(c.w3_flagged);
    const u64 w3_tex = take(c.w3_texture);
    const u64 tries = take(c.buf_tries);
    const u64 taken = take(c.buf_taken);
    const u64 no_memo = take(c.buf_no_memo);
    const u64 state = take(c.buf_state);
    const u64 masks = take(c.buf_masks);
    const u64 binding = take(c.buf_binding);
    const u64 fallback = take(c.buf_fallback);
    const u64 saved = take(c.buf_saved);
    const u64 binds = take(c.ubind_binds);
    const u64 hits = take(c.ubind_hits);
    const u64 resolve_hits = take(c.ubind_resolve_hits);
    const u64 fills = take(c.ubind_fills);
    const u64 index_hits = take(c.index_hits);
    const u64 checks = take(c.checks);
    const u64 mismatches = take(c.check_mismatches);
    const auto pct = [](u64 part, u64 whole) {
        return whole != 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
    };
    LOG_INFO(HW_GPU,
             "C12 commit repeat (10 s): {} draws ({} W3-flagged, {} gave the texture repeat); "
             "buffers {} tries, {} taken ({:.1f}%; refused: no memo {}, state {}, masks {}, "
             "binding {}; fallback {}; saved {}); ubind {} binds, {} translations reused ({:.1f}%), "
             "{} resolutions reused, {} kept; "
             "index memo hits {}; check {} / mismatches {}",
             draws, w3, w3_tex, tries, taken, pct(taken, tries), no_memo, state, masks, binding,
             fallback, saved, binds, hits, pct(hits, binds), resolve_hits, fills, index_hits, checks,
             mismatches);
    if (mismatches != 0) {
        LOG_ERROR(HW_GPU, "C12 commit repeat check: {} mismatches in 10 s", mismatches);
    }
}

} // namespace VideoCommon::MT
