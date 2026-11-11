// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/c13_memo.h"
#include "video_core/mt/lite7_capture_switches.h"
#include "video_core/mt/mt_switches.h"
#include "video_core/mt/seq_diet.h"
#include "video_core/nx2.h"

/// Cycle 12, workstream W3 (docs/cycle12-plan.md): the sequencer's capture repeat.
///
/// CaptureDraw's pass 1 (the guest reads ConfigureImpl repeats: storage buffer descriptors and
/// texture/image handles from constant buffers, TIC/TSC entries) is a function of the register
/// side (the bound constant buffer addresses, the TIC/TSC tables and limits, the sampler binding,
/// the channel's memory manager and its page table layout) and of the bytes it reads. Per
/// graphics pipeline (direct mapped, C12MemoSlots) the sequencer keeps its last pass 1: the reads
/// in capture order with their bytes, the storage buffer entries and bindings derived from them,
/// the TIC validity bits and the C11 repeat key, plus the device page and page offset of every
/// read. A later capture of the same pipeline with the same register key and page table layout
/// re-validates the bytes instead of re-doing pass 1:
///   - a page whose write generation (seq_diet.h, MT_UBO_GEN's table) is the one armed at the
///     last validation, and that the buffer tracker still protects (not CPU-modified, not a query
///     page: every guest CPU write to it traps and bumps it, every GPU-side write through the
///     memory manager or the buffer cache bumps it), is unchanged: its reads are not compared
///     (EDEN_NX2_C12_CAPTURE_REPEAT_GENS);
///   - every other page: the reads are compared with the kept bytes (a memcmp through the host
///     pointer, no translation, no handle evaluation, no recording). Equal bytes at the same
///     addresses give the same pass 1, by induction over the reads (each address is a function
///     of the key and of the bytes read before it). The generation is read before the compare,
///     so a protected page whose bytes compare equal is armed with it.
/// A hit restores the kept pass 1 and continues with pass 2 (the buffer tracker: uploads, written
/// ranges, uniform staging), which always runs. The packet gets `c12_repeat` (seq distance to the
/// packet whose pass 1 was the same) and `c12_flags` (what else matched) for the committer
/// (W2's whole-draw repeat). Contingent: EDEN_NX2_C12_CAPTURE_REF sends a hit's overlay "by
/// reference" (the readers keep the decoded overlay of the slot's last stored packet).
namespace VideoCommon::MT {

namespace C12CaptureDetail {
/// '0', '1' or 'a' (LITE default on, like the lite 7 capture pieces).
[[nodiscard]] inline char LiteMode(const char* name) {
    return Lite7Detail::Mode(name);
}
} // namespace C12CaptureDetail

/// EDEN_NX2_C12_CAPTURE_REPEAT: the capture repeat above. On by default under
/// EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 (the byte compare is exact: it reads the same guest bytes
/// pass 1 would read), '0' off (the cycle 11 capture), 'a' alternating per capture (Nx2AltOn).
/// Inactive with EDEN_NX2_MT_VERIFY, lockstep and resolvers.
[[nodiscard]] inline char C12CaptureRepeatMode() {
    static const char mode = C12CaptureDetail::LiteMode("EDEN_NX2_C12_CAPTURE_REPEAT");
    return mode;
}

/// EDEN_NX2_C12_CAPTURE_REPEAT_GENS (with REPEAT; LITE default on, '0' off, 'a' per capture):
/// the write generation skip above. Turns the generation tracking (g_ubo_gen_on, the table of
/// EDEN_NX2_MT_UBO_GEN) on; not the uniform reuse of UBO_GEN itself, which keeps its own switch.
/// Its only risk is a guest write that bypasses the tracker (CHECK's "gen stale" must be 0).
[[nodiscard]] inline char C12CaptureGensMode() {
    static const char mode = [] {
        if (C12CaptureRepeatMode() == '0') {
            return '0';
        }
        return C12CaptureDetail::LiteMode("EDEN_NX2_C12_CAPTURE_REPEAT_GENS");
    }();
    return mode;
}

/// EDEN_NX2_C12_CAPTURE_REPEAT_CHECK=1 (diagnostic, off; not part of LITE): a hit runs the full
/// pass 1 too and compares every read (address, size, kind, bytes), the storage buffer entries
/// and bindings, the TIC validity bits and the C11 key; the full results are used. Also compares
/// every page the generations skipped ("gen stale": must be 0). Mismatches: rate-limited ERROR
/// "C12 capture repeat check".
[[nodiscard]] inline bool C12CaptureRepeatCheck() {
    // Cycle 13 (mt/c13_memo.h): EDEN_NX2_C13_MEMO_CHECK=1 turns it on too (it covers every way).
    static const bool on = Enabled() && C12CaptureRepeatMode() != '0' &&
                           (Tegra::Nx2EnvMode("EDEN_NX2_C12_CAPTURE_REPEAT_CHECK") == '1' ||
                            C13MemoCheck());
    return on;
}

/// The readers decode C12 overlay references (ParsePacket calls PacketDecoder::C12ApplyRef,
/// docs/cycle12-hooks-W3.md). False until the coordinator applies that hook: the sequencer never
/// sends a reference without it.
inline constexpr bool kC12CaptureRefWired = false;

/// EDEN_NX2_C12_CAPTURE_REF=1 (contingent, off; not part of LITE; needs EDEN_NX2_MT_COMPACT and
/// kC12CaptureRefWired): a hit whose slot's overlay is stored on the readers goes without an
/// overlay (no wire entries, slot references or inserts; FinishOverlay skipped) and the readers
/// use the stored one; the first compact packet of a recorded memo stores its decoded overlay.
[[nodiscard]] inline bool C12CaptureRef() {
    static const bool on = kC12CaptureRefWired && Enabled() &&
                           C12CaptureRepeatMode() != '0' &&
                           Tegra::Nx2EnvMode("EDEN_NX2_C12_CAPTURE_REF") == '1';
    return on;
}

/// EDEN_NX2_C12_STATS=1 (or EDEN_NX2_MT_STATS=1): the "C12 capture (10 s)" line.
[[nodiscard]] inline bool C12CaptureStats() {
    static const bool on = Enabled() && (Tegra::Nx2EnvMode("EDEN_NX2_C12_STATS") == '1' ||
                                         Tegra::Nx2EnvMode("EDEN_NX2_MT_STATS") == '1');
    return on;
}

/// Draw packet header bits next to `c12_repeat` (both layouts). Valid only with c12_repeat != 0
/// and relative to the packet seq - c12_repeat (the same pipeline's earlier packet).
enum C12PacketFlags : u32 {
    C12SameReads = 1u << 0,    ///< pass 1 reads byte-identical: TIC/TSC entries, handles,
                               ///< storage descriptors (same addresses, tables, limits, layout)
    C12SameSsbo = 1u << 1,     ///< storage buffer entries identical (gpu/device address, size)
    C12SameGeometry = 1u << 2, ///< index/vertex entries identical (kind, stream, size, address)
    C12SameUboBinds = 1u << 3, ///< uniform entries' stage, slot, size, address, cached/streamed
    C12NoUploads = 1u << 4,    ///< this packet carries no CPU-modified uploads
    C12SameTicValid = 1u << 5, ///< TIC validity bits identical
};

/// C12 overlay reference operations (compact header `c12_ref_op`).
enum class C12RefOp : u8 {
    None = 0,
    Store = 1, ///< after decoding, keep this packet's overlay in slot c12_ref_slot
    Use = 2,   ///< the overlay is slot c12_ref_slot's (the packet carries none)
};
inline constexpr u32 C12RefSlotCount = 1024; ///< = the sequencer's memo slots

/// CaptureDraw's storage buffer bindings of pass 1 (kept by the memo).
struct C12SsboCapture {
    u64 device_addr;
    u32 size;
    bool is_written;
    size_t stage;
};

/// A memo's device page (4 KiB) and its generation state.
struct C12MemoPage {
    u64 page = 0;      ///< device address of the page
    u64 gen = 0;       ///< armed generation (0: none)
    u32 epoch = 0;     ///< tracking epoch the generation belongs to
    u8 unprotected = 0; ///< consecutive validations with the page unprotected (saturates)
    u8 pad[3]{};
};

/// One read of the memo: where it is in its page and in the memo's bytes.
struct C12MemoCheck {
    u16 page;   ///< index in the memo's pages
    u16 offset; ///< in the page
    u32 size;
    u32 bytes_offset;
    u32 pad;
};

inline constexpr u64 C12PageBits = 12;
inline constexpr u64 C12PageSize = u64{1} << C12PageBits;
inline constexpr size_t C12MaxPages = 32;
inline constexpr size_t C12MaxReads = 4096;
/// A page unprotected this many validations in a row is not asked again (no tracker lock for
/// it) except every C12ProbeEvery validations.
inline constexpr u8 C12UnprotectedStreak = 8;
inline constexpr u32 C12ProbeEvery = 64;
/// Misses in a row after which a slot records only every C12RecordEvery captures.
inline constexpr u32 C12MissStreak = 8;
inline constexpr u32 C12RecordEvery = 16;

/// The generation of one device page (0 never matches: the table read is never 0).
[[nodiscard]] inline u64 C12PageGeneration(u64 page) noexcept {
    return UboWriteGenerations().Read(page, C12PageSize);
}

/// Counters (relaxed; the sequencer writes them, it also logs them).
struct C12CaptureCounters {
    std::atomic<u64> captures{};    ///< captures with REPEAT on for them
    std::atomic<u64> hits{};        ///< pass 1 restored (or, CHECK, compared)
    std::atomic<u64> miss_empty{};  ///< no memo for this pipeline in its slot
    std::atomic<u64> miss_key{};    ///< register key / layout / channel moved
    std::atomic<u64> miss_bytes{};  ///< a read's bytes changed (or its page went unmapped)
    std::atomic<u64> miss_throttled{}; ///< slot in a miss streak: neither validated nor recorded
    std::atomic<u64> records{};
    std::atomic<u64> record_refused{}; ///< unmapped / cross-page / shim read / too many pages
    std::atomic<u64> pages_gen{};      ///< pages skipped by their generation
    std::atomic<u64> pages_cmp{};      ///< pages compared
    std::atomic<u64> pages_unprot{};   ///< pages asked and found unprotected
    std::atomic<u64> pages_armed{};
    std::atomic<u64> reads_restored{};
    std::atomic<u64> flagged{};        ///< packets with c12_repeat
    std::atomic<u64> flag_ssbo{};
    std::atomic<u64> flag_geometry{};
    std::atomic<u64> flag_ubo{};
    std::atomic<u64> flag_no_uploads{};
    std::atomic<u64> ref_store{};
    std::atomic<u64> ref_use{};
    std::atomic<u64> checks{};
    std::atomic<u64> check_mismatches{};
    std::atomic<u64> gen_stale{};      ///< CHECK: a generation-skipped page whose bytes changed
    std::atomic<u64> validate_ns{};    ///< time in the validation (stats only)
};

inline C12CaptureCounters g_c12_capture{};

/// Start line (once, at the first capture).
inline void C12CaptureLogStart(bool active) {
    static std::atomic<bool> logged{};
    if (logged.load(std::memory_order_relaxed) || !Enabled() || logged.exchange(true)) {
        return;
    }
    C13MemoLogStart();
    LOG_INFO(HW_GPU,
             "Cycle 12 capture: repeat {}{}, gens {}, check {}, ref {} (wired {}), stats {}",
             Lite7Detail::Name(C12CaptureRepeatMode()),
             active ? "" : " (inactive: verify/lockstep/resolvers)",
             Lite7Detail::Name(C12CaptureGensMode()), C12CaptureRepeatCheck(), C12CaptureRef(),
             kC12CaptureRefWired, C12CaptureStats());
}

/// A check mismatch (rate limited: the first 16, then every 4096th).
inline void C12CaptureReportMismatch(const char* what, u64 detail) {
    const u64 n = g_c12_capture.check_mismatches.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<u64> total{};
    const u64 all = total.fetch_add(1, std::memory_order_relaxed);
    if (all < 16 || all % 4096 == 0) {
        LOG_ERROR(HW_GPU, "C12 capture repeat check: {} differs (detail {:#x}, #{} total, {} now)",
                  what, detail, all + 1, n + 1);
    }
}

/// Sequencer: logs and resets the counters every 10 s (STATS or CHECK). Cheap to call per capture
/// (a clock read every 256 calls).
inline void C12CaptureMaybeLog() {
    if (!C12CaptureStats() && !C12CaptureRepeatCheck()) {
        return;
    }
    static u32 calls = 0;
    if ((++calls & 255) != 0) {
        return;
    }
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds{10}) {
        return;
    }
    last = now;
    C12CaptureCounters& c = g_c12_capture;
    const auto take = [](std::atomic<u64>& value) {
        return value.exchange(0, std::memory_order_relaxed);
    };
    const u64 captures = take(c.captures);
    const u64 hits = take(c.hits);
    const u64 m_empty = take(c.miss_empty);
    const u64 m_key = take(c.miss_key);
    const u64 m_bytes = take(c.miss_bytes);
    const u64 m_throttled = take(c.miss_throttled);
    const u64 records = take(c.records);
    const u64 refused = take(c.record_refused);
    const u64 p_gen = take(c.pages_gen);
    const u64 p_cmp = take(c.pages_cmp);
    const u64 p_unprot = take(c.pages_unprot);
    const u64 p_armed = take(c.pages_armed);
    const u64 restored = take(c.reads_restored);
    const u64 flagged = take(c.flagged);
    const u64 f_ssbo = take(c.flag_ssbo);
    const u64 f_geometry = take(c.flag_geometry);
    const u64 f_ubo = take(c.flag_ubo);
    const u64 f_uploads = take(c.flag_no_uploads);
    const u64 r_store = take(c.ref_store);
    const u64 r_use = take(c.ref_use);
    const u64 checks = take(c.checks);
    const u64 mismatches = take(c.check_mismatches);
    const u64 stale = take(c.gen_stale);
    const u64 validate_ns = take(c.validate_ns);
    LOG_INFO(HW_GPU,
             "C12 capture (10 s): {} captures, {} hits ({:.1f}%, {} reads restored, validate "
             "{} ms); misses: no memo {}, key {}, bytes {}, throttled {}; records {} (refused "
             "{}); pages: gen-skipped {}, compared {}, unprotected {}, armed {}; flagged {} "
             "(ssbo {}, geometry {}, ubo binds {}, no uploads {}); ref store {} / use {}; check "
             "{} / mismatches {} / gen stale {}",
             captures, hits,
             captures != 0 ? 100.0 * static_cast<double>(hits) / static_cast<double>(captures)
                           : 0.0,
             restored, validate_ns / 1000000, m_empty, m_key, m_bytes, m_throttled, records,
             refused, p_gen, p_cmp, p_unprot, p_armed, flagged, f_ssbo, f_geometry, f_ubo,
             f_uploads, r_store, r_use, checks, mismatches, stale);
    if (mismatches != 0 || stale != 0) {
        LOG_ERROR(HW_GPU, "C12 capture repeat check: {} mismatches, {} stale generations (10 s)",
                  mismatches, stale);
    }
}

} // namespace VideoCommon::MT
