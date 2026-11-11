// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <tuple>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/c10_front.h"
#include "video_core/mt/lite_switches.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"

/// Cycle 11, workstream W1 "front forwarding" (docs/cycle11-plan.md #1): the front stops paying
/// the back's backlog. Every piece has its own switch: '0' = the cycle 10 code path, '1' = on,
/// 'a' = alternating (Nx2AltOn; only where the switch is read per call). Nothing without
/// EDEN_NX2_MT. Start line: "Cycle 11 front: ..." (C11LogStart, once, from GpuSplit::MaybeStart).
namespace VideoCommon::MT {

namespace C11Detail {
/// Per-call switch that supports 'a': on by default under EDEN_NX2_MT_LITE.
[[nodiscard]] inline bool LiteCall(char mode) {
    return Enabled() && Tegra::Nx2Switch(mode, Lite());
}
/// Per-call switch that supports 'a': off by default ('1' or 'a' turn it on).
[[nodiscard]] inline bool OffCall(char mode) {
    return Enabled() && Tegra::Nx2Switch(mode, false);
}
} // namespace C11Detail

/// EDEN_NX2_C11_ACQFWD (F1, off by default, '1' on, 'a' alternates): a semaphore GEQ/EQ
/// acquire whose satisfying release is already queued in the ring before it passes without any
/// wait. The
/// value the word holds once the back ran everything sent so far is predicted from the last
/// release noted for it (puller release, QMD plain/max release from the front QMD mirror) plus
/// the QMD add releases noted after it (GpuSplit::C11PredictAcquire): the case PredictedRelease
/// misses ("site 1 op 41 pending": a QMD plain release resets a counter that add releases then
/// raise). Everything after the acquire runs after that release on the same queue (in-order
/// sequencer -> committer -> Vulkan queue); the front's own later reads keep their own guards.
/// Refused (the cycle 10 path runs): page GPU-written by shader work, table overflow, a
/// drain-known / I2M / report / other noted write over the word, a copy/I2M/2D writer sent after
/// the release (their releases are not noted), a release or add noted by another channel, a
/// predicted value that does not meet the acquire (plain release: known not to meet), EQ with
/// adds before the release or a settled release (adds may have been dropped), max release short.
///
/// Default OFF since the cycle 11 test round (docs/cycle11-analysis.md): in the race the
/// acquires that reach this path are 0-7 per 10 s (the earlier MetInMemory / PredictedRelease /
/// ledger / B1 paths take the rest) and all were refused ("other channel", "max short"): 0
/// forwarded, nothing to gain for its per-note bookkeeping. '1' or 'a' turn it on.
[[nodiscard]] inline bool C11AcqFwd() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_C11_ACQFWD");
    return C11Detail::OffCall(mode);
}

/// EDEN_NX2_C11_ACQFWD_VERIFY (on with ACQFWD; '0' turns it off): each forwarded acquire puts a
/// check into the ring at its position; the back turns it into an ordered fence callback
/// (RasterizerInterface::SignalFence: a committer Command under the MT back, so it runs after
/// every release callback queued before it and before the ones queued after it) that reads the
/// word. Unmet = counted + rate-limited ERROR "C11 acquire forwarded but not met". Must be 0.
[[nodiscard]] inline bool C11AcqFwdVerify() {
    static const bool on = Enabled() &&
                           Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_C11_ACQFWD_VERIFY"), true);
    return on;
}

/// EDEN_NX2_C11_ACQFWD_CHECK=1 (diagnostic, off): the front also waits the old way (drain +
/// fence release) for each acquire it would forward and compares; unmet = ERROR, and the old
/// path (suspension) handles it.
[[nodiscard]] inline bool C11AcqFwdCheck() {
    static const bool on = Enabled() && Tegra::Nx2EnvMode("EDEN_NX2_C11_ACQFWD_CHECK") == '1';
    return on;
}

/// EDEN_NX2_C11_POSFWD (F1b, contingent: off by default, '1' on, 'a' alternates): MME DMA reads
/// that KnownPendingWords refuses as "past known words" over a direct 16-byte payload release
/// (D2 OVERLAY64 lays words 0-1 only) get words 2-3 (the timestamp the sequencer writes with
/// gpu.GetTicks()) laid over as well, with the front's GetTicks() at the read (earlier than the
/// sequencer's by the backlog, never earlier than a timestamp the back already wrote). The
/// logging half (which words / sizes / macro) runs with EDEN_NX2_DMA_READ_WHY or the C11 stats.
[[nodiscard]] inline bool C11PosFwd() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_C11_POSFWD");
    return C11Detail::OffCall(mode);
}

/// EDEN_NX2_C11_LISTRANGE (F2, contingent on the list dump: off by default, '1' on): the
/// sequencer part of the L24/B2 list writer wait (GpuSplit::WaitForListWriters) waits only for
/// the compute writers whose SSBO write ranges (computed on the front from the QMD mirror's
/// dispatches, the program's written SSBO descriptors published by the sequencer at capture and
/// the CPU-written constant buffer words) overlap the list's bytes, plus every copy/I2M/2D writer
/// and every compute writer whose ranges the front could not compute. Needs C10_LIST_PRECISE.
namespace C13Shared {
/// Cycle 13 W3 L0 (mt/c13_front.h): EDEN_NX2_C13_LIST_CHECK=1 turns on, for the CHECK run, the
/// EDEN_NX2_C12_LISTRANGE gate, EDEN_NX2_C12_LISTWAIT_CHECK and EDEN_NX2_C13_KICK_CHECK (each
/// still off when its own variable is '0').
[[nodiscard]] inline bool ListCheckEnv() {
    static const bool on = Enabled() && Tegra::Nx2EnvMode("EDEN_NX2_C13_LIST_CHECK") == '1';
    return on;
}
} // namespace C13Shared

namespace C12Shared {
/// Cycle 12 W1 P2 (mt/c12_front.h C12ListRangeGate): EDEN_NX2_C12_LISTRANGE=1 turns F2 on too,
/// with every shortened fetch checked (C11ListRangeCheck) until the gate leaves probation.
/// Cycle 13 L0: also on, unset, with EDEN_NX2_C13_LIST_CHECK=1.
[[nodiscard]] inline bool ListRangeGateEnv() {
    static const bool on = [] {
        const char mode = Tegra::Nx2EnvMode("EDEN_NX2_C12_LISTRANGE");
        return Enabled() && (mode == '1' || (mode == 'd' && C13Shared::ListCheckEnv()));
    }();
    return on;
}
/// Set by the gate when it decides (front thread only).
inline bool listrange_probation_over{};
} // namespace C12Shared

[[nodiscard]] inline bool C11ListRange() {
    static const bool on = Enabled() &&
                           (Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_C11_LISTRANGE"), false) ||
                            C12Shared::ListRangeGateEnv());
    return on;
}

/// EDEN_NX2_C11_LISTRANGE_CHECK=1 (diagnostic): a fetch whose sequencer wait F2 shortened is
/// read again after WaitForWriters and compared; stale reads are counted and the second read is
/// used ("C11 list range check:" every 10 s).
/// EDEN_NX2_C12_LISTRANGE=1 (cycle 12 P2): also on while its gate is on probation.
[[nodiscard]] inline bool C11ListRangeCheck() {
    static const bool on = Enabled() && Tegra::Nx2EnvMode("EDEN_NX2_C11_LISTRANGE_CHECK") == '1';
    return on || (C12Shared::ListRangeGateEnv() && !C12Shared::listrange_probation_over);
}

/// EDEN_NX2_C11_LISTRANGE_IMAGES (with LISTRANGE, on by default; '0' off): storage image writes
/// (not image buffers) do not make a dispatch's ranges unknown (command lists are never in
/// texture memory). '0': any written storage image makes the dispatch unknown.
[[nodiscard]] inline bool C11ListRangeIgnoreImages() {
    static const bool on =
        Tegra::Nx2Switch(Tegra::Nx2EnvMode("EDEN_NX2_C11_LISTRANGE_IMAGES"), true);
    return on;
}

/// EDEN_NX2_C11_STATS=1 (also on with EDEN_NX2_GPU_PROFILE / EDEN_NX2_PULLER_STATS or a check
/// switch): the "C11 front per 10 s" line.
[[nodiscard]] inline bool C11Stats() {
    static const bool on = std::getenv("EDEN_NX2_C11_STATS") != nullptr ||
                           Nx2Profile::Enabled() ||
                           std::getenv("EDEN_NX2_PULLER_STATS") != nullptr ||
                           C11AcqFwdCheck() || C11ListRangeCheck();
    return on;
}

/// Start line (once, when the split starts with the MT back).
inline void C11LogStart() {
    if (!Enabled()) {
        return;
    }
    const auto mode = [](const char* name, bool default_on) {
        const char m = Tegra::Nx2EnvMode(name);
        return m == 'a' ? std::string{"alt"} : Tegra::Nx2Switch(m, default_on) ? "on" : "off";
    };
    LOG_INFO(HW_GPU,
             "Cycle 11 front: acqfwd {} (verify {}, check {}), posfwd {}, listrange {} (images "
             "ignored {}, check {}, needs listprecise {}); acqfwd refuses: GPU-written page, "
             "overflow, drain-known/I2M/report/other note over the word, copy/I2M/2D writer after "
             "the release, other channel's release/add, value short of the acquire; front-side "
             "puller effects between release and acquire: none (syncpoint/fence/refcnt/WFI go "
             "through the ring)",
             mode("EDEN_NX2_C11_ACQFWD", false), C11AcqFwdVerify(), C11AcqFwdCheck(),
             mode("EDEN_NX2_C11_POSFWD", false), C11ListRange(), C11ListRangeIgnoreImages(),
             C11ListRangeCheck(), C10ListPrecise());
}

/// Back-side verification counters (fence callbacks: any thread).
struct C11AcqVerifyStats {
    static C11AcqVerifyStats& Get() {
        static C11AcqVerifyStats instance;
        return instance;
    }
    std::atomic<u64> checked{};
    std::atomic<u64> unmet{};
    std::atomic<u64> unmet_total{};

    void Note(bool met, u64 address, u32 value, u32 word, u32 op) {
        checked.fetch_add(1, std::memory_order_relaxed);
        if (met) {
            return;
        }
        unmet.fetch_add(1, std::memory_order_relaxed);
        const u64 total = unmet_total.fetch_add(1, std::memory_order_relaxed);
        if (total < 16 || (total % 1000) == 0) {
            LOG_ERROR(HW_GPU,
                      "C11 acquire forwarded but not met: {:#x} op {} wants {:#x}, holds {:#x} "
                      "({} so far)",
                      address, op, value, word, total + 1);
        }
    }
};

/// Front-thread counters ("C11 front per 10 s").
struct C11FrontStats {
    static C11FrontStats& Get() {
        static C11FrontStats instance;
        return instance;
    }
    enum Refusal : u32 {
        RefNone,
        RefOverflow,
        RefNoRelease,
        RefOtherChannel,
        RefRangeWriter,
        RefOtherWrite,
        RefOpenI2m,
        RefShort,      ///< Plain release + adds known not to meet
        RefMaxShort,   ///< Max release (+ adds) short of the value: unknown
        RefEqual,      ///< EQ that cannot be predicted exactly
        RefGpuWritten,
        RefOp,         ///< Mask acquires
        RefCount,
    };
    // F1
    u64 acq_tries{};
    u64 acq_forwarded{};
    u64 acq_resumed{};      ///< CanResumeAcquire passes
    std::array<u64, RefCount> acq_refused{};
    u64 check_agree{};
    u64 check_unmet{};
    // F1b
    u64 pos_past_known{};
    u64 pos_extended{};
    std::map<std::tuple<u32, u32, u32, u32, bool>, u64> pos_shapes; ///< macro, off, size, write size, direct
    // F2
    u64 list_calls{};
    u64 list_shorter{};     ///< Sequencer target below every-writer-sent
    u64 list_same{};
    u64 dispatch_known{};
    u64 dispatch_unknown_program{};
    u64 dispatch_unknown_flags{};
    u64 dispatch_stale_cbuf{};
    u64 writer_unknown{};   ///< Compute writer calls the front could not describe
    u64 writer_known{};
    u64 window{};

    void MaybeLog() {
        if (!C11Stats()) {
            return;
        }
        const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
        if (window == 0) {
            window = now;
            return;
        }
        if (now - window < 10'000'000'000ULL) {
            return;
        }
        static constexpr std::array<const char*, RefCount> names{
            "none",  "overflow", "no release", "other channel", "range writer", "other write",
            "open i2m", "short", "max short", "equal", "gpu-written", "op"};
        std::string refused;
        for (u32 i = 1; i < RefCount; ++i) {
            if (acq_refused[i] != 0) {
                refused += fmt::format(" {} {}", names[i], acq_refused[i]);
            }
        }
        auto& verify = C11AcqVerifyStats::Get();
        std::string shapes;
        u32 shown = 0;
        for (const auto& [key, count] : pos_shapes) {
            if (shown++ == 8) {
                break;
            }
            shapes += fmt::format(" [macro {:#x} +{} {}B of {}B{}]: {}", std::get<0>(key),
                                  std::get<1>(key), std::get<2>(key), std::get<3>(key),
                                  std::get<4>(key) ? " direct" : "", count);
        }
        LOG_INFO(HW_GPU,
                 "C11 front per 10 s: acqfwd tries {}, forwarded {}, resumed {}, refused:{} | "
                 "verify checked {}, unmet {} | check agree {}, unmet {} | posfwd past-known {}, "
                 "extended {}, shapes:{} | listrange calls {}, shorter {}, same {}; dispatches "
                 "known {}, unknown program {}, unknown flags {}, stale cbuf {}; writers known "
                 "{}, unknown {}",
                 acq_tries, acq_forwarded, acq_resumed, refused.empty() ? " -" : refused,
                 verify.checked.exchange(0, std::memory_order_relaxed),
                 verify.unmet.exchange(0, std::memory_order_relaxed), check_agree, check_unmet,
                 pos_past_known, pos_extended, shapes.empty() ? " -" : shapes, list_calls,
                 list_shorter, list_same, dispatch_known, dispatch_unknown_program,
                 dispatch_unknown_flags, dispatch_stale_cbuf, writer_known, writer_unknown);
        *this = C11FrontStats{};
        window = now;
    }
};

/// F2: the written SSBO descriptors of each compute program, published by the sequencer when it
/// captures a dispatch (hook in Nx2DrawPipeline::CaptureCompute, docs/cycle11-hooks-W1.md) and
/// read by the front for the QMD mirror's dispatches. Fixed table, one seqlock per slot, every
/// field an atomic word (sequencer writes, front reads).
class C11ComputeWrites {
public:
    static constexpr u32 MaxSsbos = 16;
    static constexpr u32 Slots = 1024;
    static constexpr u64 FlagUnknown = 1;      ///< Writes the front cannot range (refuse)
    static constexpr u64 FlagMappingSized = 2; ///< DLSS-style bare pointers: 8 MiB ranges
    static constexpr u64 FlagImages = 4;       ///< Writes storage images (not image buffers)

    struct Entry {
        u64 flags{};
        u32 count{};
        std::array<u64, MaxSsbos> ssbos{}; ///< cbuf_index << 32 | cbuf_offset
    };

    static C11ComputeWrites& Get() {
        static C11ComputeWrites instance;
        return instance;
    }

    /// Key of a program in an address space (0: not representable).
    [[nodiscard]] static u64 Key(u64 address_space, u64 program) {
        if (address_space >= 0xfff || program == 0 || (program >> 52) != 0) {
            return 0;
        }
        return program | ((address_space + 1) << 52);
    }

    /// Sequencer: `info` is the program's Shader::Info. `mapping_sized`: this dispatch sizes its
    /// SSBOs by the mapping (IsDlssDispatch). `extra_unknown`: the dispatch writes guest memory
    /// in another way (e.g. the collision-map program's image publish).
    template <typename Info>
    void Publish(u64 address_space, u64 program, const Info& info, bool mapping_sized,
                 bool extra_unknown) {
        const u64 key = Key(address_space, program);
        if (key == 0) {
            return;
        }
        u64 flags = (mapping_sized ? FlagMappingSized : 0) | (extra_unknown ? FlagUnknown : 0);
        Slot& slot = slots[Index(key)];
        if (slot.key.load(std::memory_order_relaxed) == key &&
            (slot.flags.load(std::memory_order_relaxed) & flags) == flags) {
            return; // Published already (the per-dispatch fast path)
        }
        std::array<u64, MaxSsbos> ssbos{};
        u32 count = 0;
        for (const auto& desc : info.storage_buffers_descriptors) {
            if (!desc.is_written) {
                continue;
            }
            // sync-nx2: a pointer behind a table (upstream indirect_offset) isn't at the slot.
            if (desc.count != 1 || count == MaxSsbos || desc.indirect_offset != ~0U) {
                flags |= FlagUnknown;
                break;
            }
            ssbos[count++] = (u64{desc.cbuf_index} << 32) | u64{desc.cbuf_offset};
        }
        for (const auto& desc : info.image_buffer_descriptors) {
            if (desc.is_written) {
                flags |= FlagUnknown;
            }
        }
        for (const auto& desc : info.image_descriptors) {
            if (desc.is_written) {
                flags |= FlagImages;
            }
        }
        if (info.stores_global_memory) {
            flags |= FlagUnknown;
        }
        if (slot.key.load(std::memory_order_relaxed) == key) {
            flags |= slot.flags.load(std::memory_order_relaxed); // keep sticky flags
        }
        const u32 seq = slot.seq.load(std::memory_order_relaxed);
        slot.seq.store(seq + 1, std::memory_order_relaxed); // odd: being written
        std::atomic_thread_fence(std::memory_order_release);
        slot.key.store(key, std::memory_order_relaxed);
        slot.flags.store(flags, std::memory_order_relaxed);
        slot.count.store(count, std::memory_order_relaxed);
        for (u32 i = 0; i < MaxSsbos; ++i) {
            slot.ssbos[i].store(ssbos[i], std::memory_order_relaxed);
        }
        slot.seq.store(seq + 2, std::memory_order_release);
    }

    /// Front: the program's entry, if published and read consistently.
    [[nodiscard]] bool Lookup(u64 address_space, u64 program, Entry& out) const {
        const u64 key = Key(address_space, program);
        if (key == 0) {
            return false;
        }
        const Slot& slot = slots[Index(key)];
        const u32 before = slot.seq.load(std::memory_order_acquire);
        if ((before & 1) != 0 || slot.key.load(std::memory_order_relaxed) != key) {
            return false;
        }
        out.flags = slot.flags.load(std::memory_order_relaxed);
        out.count = std::min<u32>(slot.count.load(std::memory_order_relaxed), MaxSsbos);
        for (u32 i = 0; i < out.count; ++i) {
            out.ssbos[i] = slot.ssbos[i].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        return slot.seq.load(std::memory_order_relaxed) == before &&
               slot.key.load(std::memory_order_relaxed) == key;
    }

private:
    struct Slot {
        std::atomic<u32> seq{};
        std::atomic<u64> key{};
        std::atomic<u64> flags{};
        std::atomic<u32> count{};
        std::array<std::atomic<u64>, MaxSsbos> ssbos{};
    };
    [[nodiscard]] static u32 Index(u64 key) {
        return static_cast<u32>((key ^ (key >> 17) ^ (key >> 31)) * 0x9E3779B1u) % Slots;
    }
    std::array<Slot, Slots> slots{};
};

/// Hook entry for the sequencer (CaptureCompute): no-op unless F2 is on.
template <typename Info>
inline void C11PublishComputeWrites(u64 address_space, u64 program, const Info& info,
                                    bool mapping_sized, bool extra_unknown) {
    if (C11ListRange()) {
        C11ComputeWrites::Get().Publish(address_space, program, info, mapping_sized,
                                        extra_unknown);
    }
}

} // namespace VideoCommon::MT
