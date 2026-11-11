// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstdlib>
#include <optional>

#include <map>
#include <tuple>
#include <string>
#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/nx2_profile.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/engines/puller.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/mt/c10_front.h"
#include "video_core/mt/c11_front.h"
#include "video_core/mt/lite4_switches.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra::Engines {

namespace {

constexpr u32 Gf100BindClassMask = 0xffff;
constexpr u32 Gf100BindValidMask =  0x1f0000 | Gf100BindClassMask;

// EDEN_NX2_PULLER_STATS: which puller sites wait for the back ("GPU split drains per second:
// puller" counts them all together). `behind`: the back hadn't run everything sent (a Run always
// waits: it sends its own work first). Logged every 10 s.
enum class PullerSite : u32 {
    TriggerDrain,    ///< SemaphoreOperation acquire not predicted: DrainBack
    TriggerFences,   ///< ... still unmet after it: ReleaseFences
    ResumeDrain,     ///< CanResumeAcquire polling a suspended acquire
    ExecDrain,       ///< SEM_EXECUTE acquire not predicted: DrainBack
    ExecFences,      ///< ... then ReleaseFences (always)
    ExecSkipped,     ///< ... neither, EDEN_NX2_SEM_EXECUTE_NODRAIN
    ReadFlush,       ///< ReadSemaphoreWord downloading a GPU-written word (Run)
    Legacy,          ///< pre-Ampere acquires
    AcquireNoDrain,  ///< EDEN_NX2_MT_LITE_FOREIGNACQ: nothing queued can write it, no drain
    AcquireLedger,   ///< EDEN_NX2_MT_LITE_QMDLEDGER: an in-flight QMD max release meets it
    AcquireSeqWait,  ///< EDEN_NX2_MT_LITE_QMDLEDGER: sequencer-only wait before the drain
    PendAcquire,     ///< EDEN_NX2_MT_LITE_PENDACQ: no queued value meets it, no drain
    PendPollLazy,    ///< EDEN_NX2_MT_LITE_PENDPOLL: resume poll with a release pending, no drain
    PendCheckMet,    ///< EDEN_NX2_MT_LITE_PENDCHECK: a D5/D6 skip the drain found met
    C10AcqSeq,       ///< EDEN_NX2_C10_ACQ_SEQ: partial wait instead of the drain (B1)
    C11AcqFwd,       ///< EDEN_NX2_C11_ACQFWD: forwarded, no wait (F1)
    Count,
};
void NotePullerSite(PullerSite site, bool run = false) {
    static const bool stats = std::getenv("EDEN_NX2_PULLER_STATS") != nullptr;
    if (!stats) [[likely]] {
        return;
    }
    auto& split = VideoCommon::GpuSplit::Get();
    const bool behind =
        split.Active() && (run || split.ConsumedPosition() < split.SentPosition());
    static std::array<u64, static_cast<size_t>(PullerSite::Count)> calls{}, behinds{};
    static auto window = std::chrono::steady_clock::now();
    ++calls[static_cast<size_t>(site)];
    behinds[static_cast<size_t>(site)] += behind ? 1 : 0;
    if (std::chrono::steady_clock::now() - window < std::chrono::seconds{10}) {
        return;
    }
    static constexpr std::array<const char*, static_cast<size_t>(PullerSite::Count)> names{
        "acquire drain", "acquire fences", "resume poll",   "sem_execute drain",
        "sem_execute fences", "sem_execute skipped", "word flush", "legacy acquire",
        "acquire nodrain", "acquire ledger", "acquire seq wait", "pendacq nodrain",
        "pendpoll lazy", "pendacq would meet", "c10 acq seq", "c11 acq forwarded"};
    std::string line;
    for (size_t i = 0; i < names.size(); ++i) {
        if (calls[i] != 0) {
            line += fmt::format(" | {} {} ({} behind)", names[i], calls[i], behinds[i]);
        }
    }
    LOG_INFO(HW_GPU, "Puller waits in 10 s:{}", line);
    calls = {};
    behinds = {};
    window = std::chrono::steady_clock::now();
}

// Reads a semaphore word, flushing a GPU-written copy out of the buffer cache first.
u32 ReadSemaphoreWord(DmaPusher& dma_pusher, GPUVAddr address) {
    if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
        device && dma_pusher.rasterizer->MustFlushRegion(*device, sizeof(u32),
                                                          VideoCommon::CacheType::BufferCache)) {
        NotePullerSite(PullerSite::ReadFlush, true);
        // The flush downloads through the Vulkan scheduler, which the split's back owns.
        VideoCommon::GpuSplit::Get().RunFlush([rasterizer = dma_pusher.rasterizer, device = *device] {
            rasterizer->FlushRegion(device, sizeof(u32), VideoCommon::CacheType::BufferCache);
        }, VideoCommon::GpuSplit::DrainReason::Puller);
    }
    return dma_pusher.memory_manager.Read<u32>(address);
}

// With the GPU thread split (gpu_split.h) the rasterizer belongs to the back thread: work that
// only feeds it runs there in order; reads of what it wrote first wait for it (DrainBack).
template <typename Func>
void OnBack(const Func& func) {
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.Defer(func);
    } else {
        func();
    }
}

void DrainBack() {
    VideoCommon::GpuSplit::Get().Drain(VideoCommon::GpuSplit::DrainReason::Puller);
}

// The fence manager records into the scheduler and downloads the CPU's writes over GPU data:
// the back's state, which a drained back still uses from its idle hook (it submits).
void ReleaseFences(DmaPusher& dma_pusher) {
    VideoCommon::GpuSplit::Get().Run(
        [rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); },
        VideoCommon::GpuSplit::DrainReason::Puller);
}

// EDEN_NX2_MT_LITE_ASYNCFENCES / FOREIGNACQ: the same fence release, sent to the back without
// waiting for it (it runs after everything sent so far, as the Run would).
void SendFences(DmaPusher& dma_pusher) {
    OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); });
}

// EDEN_NX2_MT_LITE_FOREIGNACQ: whether nothing queued for the back can write the word: nothing
// the front noted (GpuSplit::NothingPendingFor), no back-only release ever targeted it (QMD and
// copy engine releases), and the GPU never wrote its page through the caches (shaders, copies).
// What can still meet such an acquire is the CPU or another un-noted writer: draining first only
// made the front wait before suspending the channel.
bool NothingQueuedWrites(DmaPusher& dma_pusher, GPUVAddr address) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (!split.Active() || !split.PredictAcquires() || Tegra::Nx2QmdReleaseKind(address) != 0 ||
        VideoCommon::MT::BackReleaseTargets::Get().MayBeTarget(address)) {
        return false;
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
    if (!device || Tegra::Nx2IsGpuWritten(*device)) {
        return false;
    }
    return split.NothingPendingFor(address);
}

// EDEN_NX2_MT_LITE_QMDLEDGER: a QMD max release the back ran (not yet in memory) leaves the
// word >= `value` (GEQ acquires only).
bool LedgerMeets(DmaPusher& dma_pusher, GPUVAddr address, u32 value) {
    if (!VideoCommon::MT::LiteQmdLedger() || !VideoCommon::GpuSplit::Get().Active()) {
        return false;
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
    return device && VideoCommon::MT::QmdMaxLedger::Get().Meets(*device, value);
}

void NoteRelease(GPUVAddr address, u32 payload, u64 size = 16) {
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.NoteRelease(address, payload);
        // `size` bytes (16 with the timestamp, 4 for fence releases) once the back runs it (front
        // DMA reads). Noting fence releases as 16 bytes made title 1's MME reads of the words
        // packed next to them (~230 a second) drain the back: EDEN_NX2_RELEASE_SIZE=0 restores
        // that (=a alternates).
        static const char size_mode = Tegra::Nx2EnvMode("EDEN_NX2_RELEASE_SIZE");
        // EDEN_NX2_MT_LITE_OVERLAY64 / SEQPOS: a 16-byte release (not a fence) is written as a
        // 64-bit payload by the sequencer (RasterizerVulkan::Query).
        const bool direct =
            size == 16 &&
            (VideoCommon::MT::Lite6Overlay64() || VideoCommon::MT::Lite6SeqPosition()) &&
            VideoCommon::MT::Lite6DirectPayloads();
        split.NotePendingWrite(address, Tegra::Nx2Switch(size_mode, true) ? size : 16,
                               VideoCommon::GpuSplit::WriteKnown::Position,
                               VideoCommon::MT::TagPullerRelease, payload,
                               direct ? VideoCommon::GpuSplit::WriteDirect64 : u8{0});
    }
}

// EDEN_NX2_MT_LITE_PENDACQ / PENDPOLL: whether every write still queued for the back that the
// front noted over the word has a known value none of which `matches`, and nothing un-noted the
// back queued can write it (as NothingQueuedWrites): a drain could then only find it met through
// a write nobody noted.
template <typename Matches>
bool PendingValuesFail(DmaPusher& dma_pusher, GPUVAddr address, const Matches& matches) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (!split.Active() || !split.PredictAcquires() || Tegra::Nx2QmdReleaseKind(address) != 0 ||
        VideoCommon::MT::BackReleaseTargets::Get().MayBeTarget(address)) {
        return false;
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
    if (!device || Tegra::Nx2IsGpuWritten(*device)) {
        return false;
    }
    return split.PendingValuesFail(address, matches);
}

// The payload a release still queued for the back will write at `address`, if any (gpu_split.h).
std::optional<VideoCommon::GpuSplit::Prediction> PredictedRelease(DmaPusher& dma_pusher,
                                                                  GPUVAddr address) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (!split.Active()) {
        return std::nullopt;
    }
    return split.PredictedRelease(address, dma_pusher.memory_manager.Read<u32>(address));
}


// An acquire that waits for at least a value (GEQUAL, or SEM_EXECUTE's wrapping >=) is met by
// what memory holds now if no release the front sent to the back targets the address: releases
// only raise these counters, so anything still queued can't undo it. Title 1 drained the back
// for ~9,000 acquires a second, most of them already met (the front waited half its time).
bool MetInMemory(DmaPusher& dma_pusher, GPUVAddr address, u32 value, bool wrapping) {
    auto& split = VideoCommon::GpuSplit::Get();
    // Max releases still queued can only raise the word: title 1 drained ~970 times a second for
    // GEQ acquires memory already met, behind a queued QMD max release (EDEN_NX2_ACQUIRE_STATS).
    // Measured -0.4 fps when first tried; with frames ahead and the deferred present it is
    // +6.2 and +1.6 fps (in-run A/B means at 2x), so on by default. EDEN_NX2_MAX_RELEASE_MET=0
    // counts them again (=a alternates).
    static const char max_mode = Tegra::Nx2EnvMode("EDEN_NX2_MAX_RELEASE_MET");
    if (!split.Active() || !split.PredictAcquires() ||
        split.HasPendingRelease(address, Tegra::Nx2Switch(max_mode, true))) {
        return false;
    }
    const u32 current = dma_pusher.memory_manager.Read<u32>(address);
    if (wrapping ? static_cast<s32>(current - value) >= 0 : current >= value) {
        return true;
    }
    // Counters QMD add releases raise: what memory holds plus the adds still queued (read after
    // memory, so an add run in between is left out rather than counted twice).
    static const char adds_mode = Tegra::Nx2EnvMode("EDEN_NX2_PREDICT_ADDS");
    if (!Tegra::Nx2Switch(adds_mode, true)) {
        return false;
    }
    const u64 pending = split.PendingAddSum(address);
    if (pending == 0 || pending > 0xffffffffULL) {
        return false;
    }
    const u32 predicted = current + static_cast<u32>(pending);
    const bool met = wrapping ? static_cast<s32>(predicted - value) >= 0
                              : u64{current} + pending >= value;
    // EDEN_NX2_VERIFY_ADDS: drain anyway and check that the back's value meets it too.
    static const bool verify = std::getenv("EDEN_NX2_VERIFY_ADDS") != nullptr;
    if (verify && met) [[unlikely]] {
        DrainBack();
        ReleaseFences(dma_pusher);
        const u32 actual = ReadSemaphoreWord(dma_pusher, address);
        const bool actual_met =
            wrapping ? static_cast<s32>(actual - value) >= 0 : actual >= value;
        static u64 checks{}, wrong{};
        ++checks;
        wrong += actual_met ? 0 : 1;
        if (!actual_met || (checks % 20000) == 0) {
            LOG_INFO(HW_GPU, "Add prediction check {}: {:#x} wants {:#x}, predicted {:#x} "
                     "(memory {:#x} + {}), back {:#x}; {} wrong so far",
                     checks, address, value, predicted, current, pending, actual, wrong);
        }
    }
    return met;
}


// EDEN_NX2_ACQUIRE_STATS: why acquires still drain the back (site, operation, whether a release
// the front sent targets the address, whether memory met it before and after the drain).
void NoteAcquireDrain(DmaPusher& dma_pusher, int site, u32 op, GPUVAddr address, u32 value,
                      bool after_met) {
    static const bool stats = std::getenv("EDEN_NX2_ACQUIRE_STATS") != nullptr;
    if (!stats) {
        return;
    }
    static std::map<std::tuple<int, u32, bool, bool>, u64> counts;
    static auto window = std::chrono::steady_clock::now();
    const bool pending = VideoCommon::GpuSplit::Get().HasPendingRelease(address);
    ++counts[{site, op * 10 + Tegra::Nx2QmdReleaseKind(address), pending, after_met}];
    static std::map<GPUVAddr, u64> addresses;
    ++addresses[address & ~GPUVAddr{0xf}];
    if (addresses.size() > 4096) {
        addresses.clear();
    }
    (void)dma_pusher;
    (void)value;
    if (std::chrono::steady_clock::now() - window > std::chrono::seconds{10}) {
        std::string line;
        for (const auto& [key, count] : counts) {
            line += fmt::format(" | site {} op {} {} {}: {}", std::get<0>(key), std::get<1>(key),
                                std::get<2>(key) ? "pending" : "none",
                                std::get<3>(key) ? "met" : "unmet", count);
        }
        std::vector<std::pair<u64, GPUVAddr>> top;
        for (const auto& [a, c] : addresses) {
            top.emplace_back(c, a);
        }
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < std::min<size_t>(6, top.size()); ++i) {
            line += fmt::format(" | addr {:#x}: {}", top[i].second, top[i].first);
        }
        addresses.clear();
        LOG_INFO(HW_GPU, "Acquire drains in 10 s:{}", line);
        counts.clear();
        window = std::chrono::steady_clock::now();
    }
}

// EDEN_NX2_C10_ACQ_SEQ (B1, mt/c10_front.h): a GEQ acquire on a word with a release still
// noted as pending whose value does not meet it (PredictedRelease failed; ACQUIRE_STATS "site 1
// op 41 pending": MKW's QMD plain-release words, met by memory in every case). Instead of
// draining everything sent, wait for the back only up to the entries that make the writes the
// front noted over the word (GpuSplit::C10AcquireTarget) and look at memory. Same assumption
// as MetInMemory for GEQ words: what the front did not note (shader writes) does not lower them;
// pages GPU work wrote are refused anyway. Equal to the drain for every noted write: the drain,
// too, ends before fence callbacks (QMD releases) that are still waiting for the GPU. Returns
// true when memory meets it (the acquire passes); false: the caller drains as before.
bool C10AcquireWithoutDrain(DmaPusher& dma_pusher, GPUVAddr address, u32 value) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (!split.Active() || !split.PredictAcquires() || !VideoCommon::MT::LiteDrain() ||
        !split.HasPendingRelease(address)) {
        return false;
    }
    auto& stats = split.c10_stats;
    ++stats.acq_tries;
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
    if (!device || Tegra::Nx2IsGpuWritten(*device)) {
        ++stats.acq_refused;
        return false;
    }
    const auto target = split.C10AcquireTarget(address);
    if (!target.ok) {
        ++stats.acq_refused;
        return false;
    }
    const bool full = target.position >= split.SentPosition();
    NotePullerSite(PullerSite::C10AcqSeq);
    const bool waited = split.C10WaitAcquireTarget(target.position);
    // As after the drain: a word the buffer cache holds GPU data for is flushed first (a Run).
    if (ReadSemaphoreWord(dma_pusher, address) < value) {
        ++stats.acq_unmet;
        return false;
    }
    if (!waited) {
        ++stats.acq_nowait;
    } else if (full) {
        ++stats.acq_full;
    } else {
        ++stats.acq_short;
    }
    return true;
}

// EDEN_NX2_C11_ACQFWD (F1, mt/c11_front.h): an acquire whose satisfying release is already
// queued in the ring before it (GpuSplit::C11PredictAcquire: the last release noted for the word
// plus the QMD adds noted after it meet it) passes without any wait: everything after it runs
// behind that release (sequencer -> committer -> one Vulkan queue). `op`: 1 EQ, 4 GEQ (the
// GpuSemaphoreOperation values); masks are refused. With EDEN_NX2_C11_ACQFWD_VERIFY the back
// checks the word at the acquire's ring position in an ordered fence callback; with
// EDEN_NX2_C11_ACQFWD_CHECK=1 the front also drains and reads it here (unmet: false, the old
// path goes on). Returns true when the acquire passes.
bool C11ForwardAcquire(DmaPusher& dma_pusher, GPUVAddr address, u32 value, u32 op) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (!split.Active() || !split.PredictAcquires() || !VideoCommon::MT::C11AcqFwd() ||
        !split.HasPendingRelease(address)) {
        return false;
    }
    using Stats = VideoCommon::MT::C11FrontStats;
    auto& stats = Stats::Get();
    ++stats.acq_tries;
    const auto refuse = [&stats](u32 why) {
        ++stats.acq_refused[why];
        return false;
    };
    if (op != 1 && op != 4) {
        return refuse(Stats::RefOp);
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
    if (!device || Tegra::Nx2IsGpuWritten(*device)) {
        return refuse(Stats::RefGpuWritten);
    }
    const auto prediction = split.C11PredictAcquire(address);
    if (prediction.refusal != Stats::RefNone) {
        return refuse(prediction.refusal);
    }
    if (op == 4) {
        if (prediction.lower < value) {
            return refuse(prediction.exact ? Stats::RefShort : Stats::RefMaxShort);
        }
    } else if (!prediction.exact || prediction.lower != value) {
        return refuse(Stats::RefEqual);
    }
    if (VideoCommon::MT::C11AcqFwdCheck()) [[unlikely]] {
        // The old way: everything sent has run, then the fence callbacks (QMD releases).
        DrainBack();
        u32 word = ReadSemaphoreWord(dma_pusher, address);
        const auto met = [op, value](u32 current) {
            return op == 4 ? current >= value : current == value;
        };
        if (!met(word)) {
            ReleaseFences(dma_pusher);
            word = ReadSemaphoreWord(dma_pusher, address);
        }
        if (!met(word)) {
            if (stats.check_unmet++ < 16) {
                LOG_ERROR(HW_GPU,
                          "C11 acquire check: {:#x} op {} wants {:#x}, predicted {:#x}{}, "
                          "holds {:#x} after the drain",
                          address, op, value, prediction.lower,
                          prediction.exact ? " exactly" : " or more", word);
            }
            return false;
        }
        ++stats.check_agree;
    }
    if (VideoCommon::MT::C11AcqFwdVerify()) {
        // At the acquire's position in the ring; the fence callback runs after every release
        // callback queued before it (in order on the committer / fence thread).
        split.Defer([rasterizer = dma_pusher.rasterizer, memory = &dma_pusher.memory_manager,
                     address, value, op] {
            rasterizer->SignalFence([memory, address, value, op] {
                const u32 word = memory->Read<u32>(address);
                const bool met = op == 4 ? word >= value : word == value;
                VideoCommon::MT::C11AcqVerifyStats::Get().Note(met, address, value, word, op);
            });
        });
    }
    ++stats.acq_forwarded;
    NotePullerSite(PullerSite::C11AcqFwd);
    split.NotePredictedAcquire();
    return true;
}

} // Anonymous namespace

bool Puller::PendingAcquire::Matches(u32 current) const {
    switch (static_cast<GpuSemaphoreOperation>(operation)) {
    case GpuSemaphoreOperation::AcquireEqual:
        return current == value;
    case GpuSemaphoreOperation::AcquireGequal:
        return current >= value;
    case GpuSemaphoreOperation::AcquireMask:
        return (current & value) != 0;
    default:
        return true;
    }
}

bool Puller::CanResumeAcquire(DmaPusher& dma_pusher) {
    if (!pending_acquire) {
        return true;
    }
    if (pending_acquire->operation == static_cast<u32>(GpuSemaphoreOperation::AcquireGequal) &&
        MetInMemory(dma_pusher, pending_acquire->address, pending_acquire->value, false)) {
        VideoCommon::GpuSplit::Get().NotePredictedAcquire();
        return true;
    }
    if (const auto predicted = PredictedRelease(dma_pusher, pending_acquire->address);
        predicted && (!predicted->at_least || pending_acquire->operation ==
                                                  static_cast<u32>(GpuSemaphoreOperation::AcquireGequal)) &&
        pending_acquire->Matches(predicted->value)) {
        VideoCommon::GpuSplit::Get().NotePredictedAcquire();
        return true;
    }
    if (pending_acquire->operation == static_cast<u32>(GpuSemaphoreOperation::AcquireGequal) &&
        LedgerMeets(dma_pusher, pending_acquire->address, pending_acquire->value)) {
        NotePullerSite(PullerSite::AcquireLedger);
        VideoCommon::GpuSplit::Get().NotePredictedAcquire();
        return true;
    }
    // EDEN_NX2_C11_ACQFWD (F1): a release queued since the suspension meets it.
    if (C11ForwardAcquire(dma_pusher, pending_acquire->address, pending_acquire->value,
                          pending_acquire->operation)) {
        ++VideoCommon::MT::C11FrontStats::Get().acq_resumed;
        return true;
    }
    // Title 1 polled ~13,000 times a second, ~330 of them draining a busy back, for semaphores
    // the CPU or another channel releases. Without a queued release for the address the back
    // can't meet it soon: check memory and the waiter latch without draining, and drain only
    // every 8th poll (writes nobody noted, e.g. shaders). EDEN_NX2_RESUME_LAZY=0 drains every
    // poll (=a alternates). On by default under EDEN_NX2_MT_LITE (EDEN_NX2_MT_LITE_RESUME): each
    // drain there also idles the sequencer until the front sends again.
    static const char lazy_mode = Tegra::Nx2EnvMode("EDEN_NX2_RESUME_LAZY");
    if (auto& split = VideoCommon::GpuSplit::Get();
        split.Active() && Tegra::Nx2Switch(lazy_mode, VideoCommon::MT::LiteAcquireNoDrain()) &&
        !split.HasPendingRelease(pending_acquire->address)) {
        // A queued release could still change a word that matches now (equal acquires): only
        // without one is memory the answer.
        if (Nx2SemaphoreWaiters::Get().Latched(this) ||
            pending_acquire->Matches(dma_pusher.memory_manager.Read<u32>(pending_acquire->address))) {
            return true;
        }
        // EDEN_NX2_MT_LITE_FOREIGNACQ: nothing queued can write the word, so a drain can
        // only find an un-noted GPU write: every 64th poll.
        const u32 period = VideoCommon::MT::LiteForeignAcquire() &&
                                   NothingQueuedWrites(dma_pusher, pending_acquire->address)
                               ? 63
                               : 7;
        if ((++lazy_polls & period) != 0) {
            return false;
        }
    } else if (VideoCommon::MT::Lite6PendingPoll() && split.Active() &&
               split.HasPendingRelease(pending_acquire->address) &&
               PendingValuesFail(dma_pusher, pending_acquire->address,
                                 [this](u32 value) { return pending_acquire->Matches(value); })) {
        // EDEN_NX2_MT_LITE_PENDPOLL (D6): a release is still queued, but none of the queued
        // values meets the acquire: only the latch (a write that met it) answers between drains,
        // which come every 8th poll (memory may still change to a queued value).
        const bool latched = Nx2SemaphoreWaiters::Get().Latched(this);
        if (latched) {
            return true;
        }
        if ((++lazy_polls & 7) != 0) {
            if (VideoCommon::MT::Lite6PendingCheck()) [[unlikely]] {
                DrainBack();
                if (Nx2SemaphoreWaiters::Get().Latched(this) ||
                    pending_acquire->Matches(
                        ReadSemaphoreWord(dma_pusher, pending_acquire->address))) {
                    NotePullerSite(PullerSite::PendCheckMet);
                    return true;
                }
            }
            NotePullerSite(PullerSite::PendPollLazy);
            return false;
        }
    }
    // The release may still be queued for the split GPU thread's back half (and the caches
    // ReadSemaphoreWord asks are its state).
    NotePullerSite(PullerSite::ResumeDrain);
    DrainBack();
    const u32 current = ReadSemaphoreWord(dma_pusher, pending_acquire->address);
    if (Nx2SemaphoreWaiters::Get().Latched(this)) {
        if (static std::atomic<u32> rescued{};
            !pending_acquire->Matches(current) && rescued.fetch_add(1) < 64) {
            LOG_INFO(HW_GPU, "Channel {} met semaphore {:#x} value {:#x} at its release (now {:#x})",
                     dma_pusher.channel_state.bind_id, pending_acquire->address,
                     pending_acquire->value, current);
        }
        return true;
    }
    if (pending_acquire->Matches(current)) {
        return true;
    }
    // EDEN_NX2_SUSPEND_LOG_MS: report suspensions after that many ms instead of 2 s.
    static const auto report_after = [] {
        const char* value = std::getenv("EDEN_NX2_SUSPEND_LOG_MS");
        return std::chrono::milliseconds{value != nullptr ? std::atoll(value) : 2000};
    }();
    // With delayed fence operations (Balanced or Accurate fences, accurate GPU), a release
    // queued after the acquire released the fences waits in the caches for the next fence. With
    // every channel waiting none comes: release them again now and then while waiting.
    const bool delayed = Settings::IsGPUFenceBehaviorDefault()
                             ? Settings::IsGPULevelHigh()
                             : Settings::IsGPUFenceBehaviorBalanced() ||
                                   Settings::IsGPUFenceBehaviorAccurate();
    if (const auto now = std::chrono::steady_clock::now();
        delayed && now - pending_acquire->last_fences >= std::chrono::milliseconds{1}) {
        pending_acquire->last_fences = now;
        ReleaseFences(dma_pusher);
        if (Nx2SemaphoreWaiters::Get().Latched(this) ||
            pending_acquire->Matches(ReadSemaphoreWord(dma_pusher, pending_acquire->address))) {
            return true;
        }
    }
    if (!pending_acquire->reported &&
        std::chrono::steady_clock::now() - pending_acquire->since > report_after) {
        pending_acquire->reported = true;
        const auto device = dma_pusher.memory_manager.GpuToCpuAddress(pending_acquire->address);
        LOG_WARNING(HW_GPU, "Channel {} waits on semaphore {:#x} (op {}, {:#x}, now {:#x}) for {} ms; "
                    "device {:#x}, buffer cache holds GPU data: {}, words {:08x} {:08x} {:08x} {:08x}",
                    dma_pusher.channel_state.bind_id, pending_acquire->address,
                    pending_acquire->operation, pending_acquire->value, current,
                    report_after.count(), device.value_or(0),
                    device && dma_pusher.rasterizer->MustFlushRegion(*device, 16, VideoCommon::CacheType::All),
                    dma_pusher.memory_manager.Read<u32>(pending_acquire->address & ~GPUVAddr{0xf}),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 4),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 8),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 12));
    }
    return false;
}

bool Puller::ResumeAcquire(DmaPusher& dma_pusher) {
    if (!CanResumeAcquire(dma_pusher)) {
        return false;
    }
    // Only a resume from a suspension counts: DmaPusher::Step calls this before every entry, and
    // counting those made the count odd ("suspended") at random, so submit-time snapshots were
    // thrown away for about half of all entries (CommandList::snapshot_suspensions).
    if (pending_acquire) {
        pending_acquire.reset();
        dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
        Nx2SemaphoreWaiters::Get().Remove(this);
    }
    return true;
}

void Puller::ProcessBindMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    LOG_DEBUG(HW_GPU, "Binding subchannel {} to engine {:#x}", method_call.subchannel, method_call.argument);
    u32 engine = method_call.argument;
    if ((engine & ~Gf100BindClassMask) != 0 && (engine & ~Gf100BindValidMask) == 0) {
        engine &= Gf100BindClassMask;
    }
    // Switch 2 binds Ampere classes; run them on the Maxwell engines (same method layout for
    // most state). Class 0 unbinds the subchannel.
    switch (engine & 0xFFFF) {
    case 0xC797: // AMPERE_B
        engine = static_cast<u32>(EngineID::MAXWELL_B);
        break;
    case 0xC7C0: // AMPERE_COMPUTE_B
        engine = static_cast<u32>(EngineID::KEPLER_COMPUTE_B);
        break;
    case 0xC7B5: // AMPERE_DMA_COPY_B
        engine = static_cast<u32>(EngineID::MAXWELL_DMA_COPY_A);
        break;
    case 0x0:
        if (g_nx2_ampere) {
            LOG_DEBUG(HW_GPU, "Switch 2 unbinds subchannel {}", method_call.subchannel);
            return;
        }
        break;
    default:
        break;
    }

    const auto engine_id = static_cast<EngineID>(engine);
    bound_engines[method_call.subchannel] = engine_id;
    switch (engine_id) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->fermi_2d, method_call.subchannel, EngineTypes::Fermi2D);
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->maxwell_3d, method_call.subchannel, EngineTypes::Maxwell3D);
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->kepler_compute, method_call.subchannel, EngineTypes::KeplerCompute);
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->maxwell_dma, method_call.subchannel, EngineTypes::MaxwellDMA);
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->kepler_memory, method_call.subchannel, EngineTypes::KeplerMemory);
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->nv01_timer, method_call.subchannel, EngineTypes::Nv01Timer);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine {:04X}", engine_id);
        break;
    }
}

void Puller::ProcessFenceActionMethod(DmaPusher& dma_pusher) {
    switch (regs.fence_action.op) {
    case Puller::FenceOperation::Acquire:
        // UNIMPLEMENTED_MSG("Channel Scheduling pending.");
        // WaitFence(regs.fence_action.syncpoint_id, regs.fence_value);
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); });
        break;
    case Puller::FenceOperation::Increment:
        OnBack([rasterizer = dma_pusher.rasterizer, id = static_cast<u32>(regs.fence_action.syncpoint_id.Value())] {
            rasterizer->SignalSyncPoint(id);
        });
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented operation {}", regs.fence_action.op.Value());
        break;
    }
}

void Puller::ProcessSemaphoreTriggerMethod(DmaPusher& dma_pusher) {
    const auto semaphoreOperationMask = 0xF;
    const auto op = GpuSemaphoreOperation(regs.semaphore_trigger & semaphoreOperationMask);
    // EDEN_NX2_SEM_WATCH=<gpu address>: log every release/acquire on the 16 bytes there.
    static const GPUVAddr watch = [] {
        const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
        return value ? GPUVAddr{std::strtoull(value, nullptr, 16)} : GPUVAddr{};
    }();
    if (watch != 0 && (regs.semaphore_address.SemaphoreAddress() & ~GPUVAddr{0xf}) == watch) {
        LOG_INFO(HW_GPU, "Semaphore watch: channel {} as {} device {:#x} op {} {:#x} value {:#x} (now {:#x})",
                 dma_pusher.channel_state.bind_id, dma_pusher.memory_manager.GetID(),
                 dma_pusher.memory_manager.GpuToCpuAddress(regs.semaphore_address.SemaphoreAddress()).value_or(0),
                 static_cast<u32>(op),
                 regs.semaphore_address.SemaphoreAddress(), regs.semaphore_sequence,
                 dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress()));
    }
    if (op == GpuSemaphoreOperation::WriteLong) {
        const GPUVAddr sequence_address{regs.semaphore_address.SemaphoreAddress()};
        const u32 payload = regs.semaphore_sequence;
        // RELEASE_SIZE (bit 24) set means a 4-byte release without the timestamp; writing 16
        // bytes there clobbers the words next to it (Switch 2 packs semaphores tightly).
        const auto properties = g_nx2_ampere && (regs.semaphore_trigger & (1U << 24)) != 0
                                    ? VideoCommon::QueryPropertiesFlags::IsAFence
                                    : VideoCommon::QueryPropertiesFlags::HasTimeout;
        const GPUVAddr mirror = g_nx2_ampere ? Nx2SemaphoreMirror(sequence_address) : 0;
        if (mirror != 0) {
            LOG_DEBUG(HW_GPU, "Switch 2 semaphore {:#x}={:#x} mirrored to {:#x}", sequence_address, payload, mirror);
        }
        OnBack([rasterizer = dma_pusher.rasterizer, sequence_address, mirror, properties, payload] {
            rasterizer->Query(sequence_address, VideoCommon::QueryType::Payload, properties, payload, 0);
            if (mirror != 0) {
                rasterizer->Query(mirror, VideoCommon::QueryType::Payload, properties, payload, 0);
            }
        });
        const u64 release_bytes =
            properties == VideoCommon::QueryPropertiesFlags::IsAFence ? 4 : 16;
        NoteRelease(sequence_address, payload, release_bytes);
        if (mirror != 0) {
            NoteRelease(mirror, payload, release_bytes);
        }
    } else if (g_nx2_ampere) {
        // Switch 2 waits on semaphores the CPU (or another channel) releases, e.g. once it has
        // written the commands this channel runs next: suspend the channel until the value is
        // met instead of reading ahead (approach from changes.patch).
        PendingAcquire acquire{
            .address = regs.semaphore_address.SemaphoreAddress(),
            .value = regs.semaphore_sequence,
            .operation = static_cast<u32>(op),
            .since = std::chrono::steady_clock::now(),
            .last_fences = std::chrono::steady_clock::now(),
            .reported = false,
        };
        if (op != GpuSemaphoreOperation::AcquireEqual && op != GpuSemaphoreOperation::AcquireGequal &&
            op != GpuSemaphoreOperation::AcquireMask) {
            LOG_ERROR(HW_GPU, "Invalid semaphore operation {}", static_cast<u32>(op));
            return;
        }
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireChecks, 1);
        if (op == GpuSemaphoreOperation::AcquireGequal &&
            MetInMemory(dma_pusher, acquire.address, acquire.value, false)) {
            VideoCommon::GpuSplit::Get().NotePredictedAcquire();
            return;
        }
        if (const auto predicted = PredictedRelease(dma_pusher, acquire.address);
            predicted && (!predicted->at_least || op == GpuSemaphoreOperation::AcquireGequal) &&
            acquire.Matches(predicted->value)) {
            VideoCommon::GpuSplit::Get().NotePredictedAcquire();
            return;
        }
        if (op == GpuSemaphoreOperation::AcquireGequal &&
            LedgerMeets(dma_pusher, acquire.address, acquire.value)) {
            NotePullerSite(PullerSite::AcquireLedger);
            VideoCommon::GpuSplit::Get().NotePredictedAcquire();
            return;
        }
        // EDEN_NX2_MT_LITE_FOREIGNACQ: nothing queued can write the word: suspend without the
        // drain (as the old path did after it for these: ~690 in 10 s stayed unmet in title 1).
        // The waiter latch goes first, then the fences go to the back without a wait (a fence
        // callback may be what lets the CPU release the word), then one look at memory.
        const auto suspend_without_drain = [&](int site) {
            auto& waiters = Nx2SemaphoreWaiters::Get();
            if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address)) {
                waiters.Add(this, *device, acquire.value, acquire.operation);
            }
            SendFences(dma_pusher);
            const bool met_now =
                waiters.Latched(this) ||
                acquire.Matches(dma_pusher.memory_manager.Read<u32>(acquire.address));
            NoteAcquireDrain(dma_pusher, site, static_cast<u32>(op), acquire.address,
                             acquire.value, met_now);
            if (!met_now) {
                pending_acquire = acquire;
                dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
                VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireWaits, 1);
            } else {
                waiters.Remove(this);
            }
        };
        if (VideoCommon::MT::LiteForeignAcquire() &&
            NothingQueuedWrites(dma_pusher, acquire.address)) {
            NotePullerSite(PullerSite::AcquireNoDrain);
            suspend_without_drain(3);
            return;
        }
        // EDEN_NX2_MT_LITE_PENDACQ (D5): releases are queued for the word, but none of their
        // (known) values meets the acquire, nor does memory now: the drain could only find it met
        // through a write nobody noted. Suspend as above (ACQUIRE_STATS site 4).
        if (VideoCommon::MT::Lite6PendingAcquire() &&
            PendingValuesFail(dma_pusher, acquire.address,
                              [&acquire](u32 value) { return acquire.Matches(value); }) &&
            !acquire.Matches(dma_pusher.memory_manager.Read<u32>(acquire.address))) {
            if (VideoCommon::MT::Lite6PendingCheck()) [[unlikely]] {
                DrainBack();
                if (acquire.Matches(ReadSemaphoreWord(dma_pusher, acquire.address))) {
                    NotePullerSite(PullerSite::PendCheckMet);
                    return;
                }
            }
            NotePullerSite(PullerSite::PendAcquire);
            suspend_without_drain(4);
            return;
        }
        // EDEN_NX2_C11_ACQFWD (F1): the satisfying release is queued before it: no wait at all.
        if (C11ForwardAcquire(dma_pusher, acquire.address, acquire.value,
                              static_cast<u32>(op))) {
            NoteAcquireDrain(dma_pusher, 6, static_cast<u32>(op), acquire.address, acquire.value,
                             true);
            return;
        }
        // EDEN_NX2_MT_LITE_QMDLEDGER: a word QMD max releases target, met after the drain without
        // a release the front noted (~50/s in title 1): the QMD is probably still queued. Wait
        // for the sequencer only (it records the release in the ledger), then look again.
        if (op == GpuSemaphoreOperation::AcquireGequal && VideoCommon::MT::LiteQmdLedger() &&
            VideoCommon::GpuSplit::Get().MtActive() &&
            Tegra::Nx2QmdReleaseKind(acquire.address) == 3) {
            NotePullerSite(PullerSite::AcquireSeqWait);
            VideoCommon::GpuSplit::Get().WaitSequencer(VideoCommon::GpuSplit::DrainReason::Puller);
            if (LedgerMeets(dma_pusher, acquire.address, acquire.value) ||
                MetInMemory(dma_pusher, acquire.address, acquire.value, false)) {
                NotePullerSite(PullerSite::AcquireLedger);
                VideoCommon::GpuSplit::Get().NotePredictedAcquire();
                return;
            }
        }
        // EDEN_NX2_C10_ACQ_SEQ (B1): wait only as far as the noted writes over the word.
        if (op == GpuSemaphoreOperation::AcquireGequal && VideoCommon::MT::C10AcqSeq() &&
            C10AcquireWithoutDrain(dma_pusher, acquire.address, acquire.value)) {
            NoteAcquireDrain(dma_pusher, 5, static_cast<u32>(op), acquire.address, acquire.value,
                             true);
            return;
        }
        const bool had_pending =
            VideoCommon::GpuSplit::Get().HasPendingRelease(acquire.address);
        static const bool acquire_stats = std::getenv("EDEN_NX2_ACQUIRE_STATS") != nullptr;
        if (acquire_stats) [[unlikely]] {
            const u32 before = dma_pusher.memory_manager.Read<u32>(acquire.address);
            const u64 adds = VideoCommon::GpuSplit::Get().PendingAddSum(acquire.address);
            static u32 logged{};
            if ((logged++ % 3000) == 2999) {
                LOG_INFO(HW_GPU, "Acquire drain: {:#x} wants >= {:#x}, memory {:#x}, queued adds {} "
                         "(adds noted so far {}, last at {:#x})",
                         acquire.address, acquire.value, before, adds,
                         VideoCommon::GpuSplit::Get().noted_adds,
                         VideoCommon::GpuSplit::Get().last_add_address);
            }
        }
        NotePullerSite(PullerSite::TriggerDrain);
        DrainBack();
        const bool met_now = acquire.Matches(ReadSemaphoreWord(dma_pusher, acquire.address));
        NoteAcquireDrain(dma_pusher, had_pending ? 1 : 2, static_cast<u32>(op), acquire.address,
                         acquire.value, met_now);
        if (met_now) {
            return;
        }
        // Registered before the fences run, so a release among them that meets the value counts
        // even if a later one overwrites it.
        auto& waiters = Nx2SemaphoreWaiters::Get();
        if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address)) {
            waiters.Add(this, *device, acquire.value, acquire.operation);
        }
        // EDEN_NX2_MT_LITE_ASYNCFENCES: sent, not waited for (a second round trip right after
        // the drain); the resume poll sees what the callbacks write (memory, the latch).
        const bool async_fences = VideoCommon::MT::LiteAsyncFences();
        NotePullerSite(PullerSite::TriggerFences, !async_fences);
        if (async_fences) {
            SendFences(dma_pusher);
        } else {
            ReleaseFences(dma_pusher);
        }
        if (!waiters.Latched(this) && !acquire.Matches(ReadSemaphoreWord(dma_pusher, acquire.address))) {
            pending_acquire = acquire;
            dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireWaits, 1);
        } else {
            waiters.Remove(this);
        }
    } else {
        NotePullerSite(PullerSite::Legacy);
        DrainBack();
        do {
            const u32 word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
            regs.acquire_source = true;
            regs.acquire_value = regs.semaphore_sequence;
            if (op == GpuSemaphoreOperation::AcquireEqual) {
                regs.acquire_active = true;
                regs.acquire_mode = false;
                if (word != regs.acquire_value) {
                    ReleaseFences(dma_pusher);
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireGequal) {
                regs.acquire_active = true;
                regs.acquire_mode = true;
                if (word < regs.acquire_value) {
                    ReleaseFences(dma_pusher);
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireMask) {
                if (word && regs.semaphore_sequence == 0) {
                    ReleaseFences(dma_pusher);
                    continue;
                }
            } else {
                LOG_ERROR(HW_GPU, "Invalid semaphore operation");
            }
        } while (false);
    }
}

void Puller::ProcessSemaphoreRelease(DmaPusher& dma_pusher) {
    const GPUVAddr sequence_address{regs.semaphore_address.SemaphoreAddress()};
    const u32 payload = regs.semaphore_release;
    OnBack([rasterizer = dma_pusher.rasterizer, sequence_address, payload] {
        rasterizer->Query(sequence_address, VideoCommon::QueryType::Payload,
                          VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
    });
    NoteRelease(sequence_address, payload, 4);
}

void Puller::ProcessSemaphoreAcquire(DmaPusher& dma_pusher) {
    DrainBack();
    u32 word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
    const auto value = regs.semaphore_acquire;
    while (word != value) {
        regs.acquire_active = true;
        regs.acquire_value = value;
        ReleaseFences(dma_pusher);
        word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
        // TODO(kemathe73) figure out how to do the acquire_timeout
        regs.acquire_mode = false;
        regs.acquire_source = false;
    }
}

// Ampere SEM_EXECUTE (method 0x1B): operation in bits 2:0 on the semaphore at
// SEM_ADDR_HI:SEM_ADDR_LO (methods 0x18/0x17) with payload SEM_PAYLOAD_LO (0x19).
void Puller::ProcessSemaphoreExecute(DmaPusher& dma_pusher, u32 argument) {
    const GPUVAddr address = (GPUVAddr{regs.reg_array[0x18] & 0x1FFFFFF} << 32) |
                             (regs.reg_array[0x17] & ~3U);
    const u32 payload = regs.reg_array[0x19];
    const u32 operation = argument & 7;
    if (operation == 1) { // RELEASE
        OnBack([rasterizer = dma_pusher.rasterizer, address, payload] {
            rasterizer->Query(address, VideoCommon::QueryType::Payload,
                              VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
        });
        NoteRelease(address, payload, 4);
        return;
    }
    // Acquires: check once after flushing pending fences; never block the GPU thread on them.
    if ((operation == 2 || operation == 3) && MetInMemory(dma_pusher, address, payload, operation == 3)) {
        VideoCommon::GpuSplit::Get().NotePredictedAcquire();
        return;
    }
    if (const auto predicted = PredictedRelease(dma_pusher, address);
        predicted && (!predicted->at_least || operation == 2) &&
        (operation == 0   ? predicted->value == payload
         : operation == 2 ? predicted->value >= payload
         : operation == 3 ? static_cast<s32>(predicted->value - payload) >= 0
         : operation == 4 ? (predicted->value & payload) != 0
         : operation == 5 ? (predicted->value & payload) == 0
                          : true)) {
        VideoCommon::GpuSplit::Get().NotePredictedAcquire();
        return;
    }
    // These acquires never block (see below: an unmet one only logs and the channel goes on),
    // so the drain and the fence release that follow change nothing the front does next: they
    // only made it wait for the back (and the back submit) at every acquire no queued release
    // predicts. EDEN_NX2_SEM_EXECUTE_NODRAIN=1 goes on without them (=a alternates; off by
    // default). The back still runs every release in order, before anything sent after this.
    // On by default under EDEN_NX2_MT_LITE (EDEN_NX2_MT_LITE_EXECNODRAIN): there each drain also
    // idles the sequencer until the front sends again.
    static const char nodrain_mode = Tegra::Nx2EnvMode("EDEN_NX2_SEM_EXECUTE_NODRAIN");
    if (VideoCommon::GpuSplit::Get().Active() &&
        Tegra::Nx2Switch(nodrain_mode, VideoCommon::MT::LiteExecNoDrain())) {
        NotePullerSite(PullerSite::ExecSkipped);
        return;
    }
    const bool had_pending = VideoCommon::GpuSplit::Get().HasPendingRelease(address);
    NotePullerSite(PullerSite::ExecDrain);
    DrainBack();
    NotePullerSite(PullerSite::ExecFences, true);
    ReleaseFences(dma_pusher);
    const u32 word = dma_pusher.memory_manager.Read<u32>(address);
    NoteAcquireDrain(dma_pusher, had_pending ? 3 : 4, 100 + operation, address, payload,
                     operation == 0   ? word == payload
                     : operation == 2 ? word >= payload
                     : operation == 3 ? static_cast<s32>(word - payload) >= 0
                     : operation == 4 ? (word & payload) != 0
                     : operation == 5 ? (word & payload) == 0
                                      : true);
    const bool ready = operation == 0   ? word == payload
                       : operation == 2 ? word >= payload
                       : operation == 3 ? static_cast<s32>(word - payload) >= 0
                       : operation == 4 ? (word & payload) != 0
                       : operation == 5 ? (word & payload) == 0
                                        : true;
    if (!ready) {
        static std::atomic<u32> reported{};
        if (reported.fetch_add(1) < 8) {
            LOG_WARNING(HW_GPU, "SEM_EXECUTE op {} at {:#x}: {:#x} vs {:#x} not met, continuing",
                        operation, address, word, payload);
        }
    }
}

/// Calls a GPU puller method.
void Puller::CallPullerMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    static const bool nx2_trace = std::getenv("EDEN_NX2_PULLER_TRACE") != nullptr;
    if (nx2_trace) {
        LOG_INFO(HW_GPU, "nx2 puller sub={} method={:#x} arg={:#x}", method_call.subchannel,
                 method_call.method, method_call.argument);
    }
    regs.reg_array[method_call.method] = method_call.argument;
    const auto method = static_cast<BufferMethods>(method_call.method);

    switch (method) {
    case BufferMethods::BindObject: {
        ProcessBindMethod(dma_pusher, method_call);
        break;
    }
    case BufferMethods::Nop:
    case BufferMethods::SemaphoreAddressHigh:
    case BufferMethods::SemaphoreAddressLow:
    case BufferMethods::SemaphoreSequencePayload:
    case BufferMethods::SyncpointPayload:
    case BufferMethods::WrcacheFlush:
        break;
    case BufferMethods::RefCnt:
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->SignalReference(); });
        break;
    case BufferMethods::SyncpointOperation:
        ProcessFenceActionMethod(dma_pusher);
        break;
    case BufferMethods::WaitForIdle:
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->WaitForIdle(); });
        break;
    case BufferMethods::SemaphoreOperation: {
        ProcessSemaphoreTriggerMethod(dma_pusher);
        break;
    }
    case BufferMethods::NonStallInterrupt: {
        LOG_ERROR(HW_GPU, "Special puller engine method NonStallInterrupt not implemented");
        break;
    }
    case BufferMethods::MemOpA: {
        LOG_TRACE(HW_GPU, "Memory Operation A");
        break;
    }
    case BufferMethods::MemOpB: {
        // Implement this better.
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->InvalidateGPUCache(); });
        break;
    }
    case BufferMethods::MemOpC:
    case BufferMethods::MemOpD: {
        LOG_TRACE(HW_GPU, "Memory Operation C,D");
        break;
    }
    case BufferMethods::SemaphoreAcquire: {
        // On Ampere host classes 0x17..0x1A are SEM_ADDR_LO/HI and SEM_PAYLOAD_LO/HI.
        if (!g_nx2_ampere) {
            ProcessSemaphoreAcquire(dma_pusher);
        }
        break;
    }
    case BufferMethods::SemaphoreRelease: {
        if (g_nx2_ampere) {
            ProcessSemaphoreExecute(dma_pusher, method_call.argument);
        } else {
            ProcessSemaphoreRelease(dma_pusher);
        }
        break;
    }
    case BufferMethods::Yield: {
        // TODO(Kmather73): Research and implement this method.
        LOG_ERROR(HW_GPU, "Special puller engine method Yield not implemented");
        break;
    }
    default:
        LOG_ERROR(HW_GPU, "Special puller engine method {:X} not implemented", method);
        if (static bool dumped = false; g_nx2_ampere && !dumped) {
            dumped = true;
            dma_pusher.DumpCurrentEntry("unknown host method");
        }
        break;
    }
}

/// Calls a GPU engine method.
static bool Nx2MethodTrace() {
    static const bool trace = std::getenv("EDEN_NX2_METHOD_TRACE") != nullptr;
    return trace;
}

void Puller::CallEngineMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    const EngineID engine = bound_engines[method_call.subchannel];
    if (Nx2MethodTrace()) {
        LOG_INFO(HW_GPU, "nx2 method sub={} engine={:04X} method={:#x} arg={:#x}",
                 method_call.subchannel, static_cast<u32>(engine), method_call.method,
                 method_call.argument);
    }
    switch (engine) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.channel_state.payload->fermi_2d.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.channel_state.payload->maxwell_3d.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.channel_state.payload->kepler_compute.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.channel_state.payload->maxwell_dma.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.channel_state.payload->kepler_memory.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.channel_state.payload->nv01_timer.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine");
        break;
    }
}

/// Calls a GPU engine multivalue method.
void Puller::CallEngineMultiMethod(DmaPusher& dma_pusher, u32 method, u32 subchannel, const u32* base_start, u32 amount, u32 methods_pending) {
    const EngineID engine = bound_engines[subchannel];
    if (Nx2MethodTrace()) {
        LOG_INFO(HW_GPU, "nx2 multimethod sub={} engine={:04X} method={:#x} amount={} first={:#x}",
                 subchannel, static_cast<u32>(engine), method, amount, base_start[0]);
    }
    switch (engine) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.channel_state.payload->fermi_2d.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.channel_state.payload->maxwell_3d.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.channel_state.payload->kepler_compute.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.channel_state.payload->maxwell_dma.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.channel_state.payload->kepler_memory.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.channel_state.payload->nv01_timer.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine");
        break;
    }
}

/// Calls a GPU method.
void Puller::CallMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    LOG_TRACE(HW_GPU, "Processing method {:08X} on subchannel {}", method_call.method, method_call.subchannel);
    ASSERT(method_call.subchannel < bound_engines.size());

    if (ExecuteMethodOnEngine(dma_pusher, method_call.method)) {
        CallEngineMethod(dma_pusher, method_call);
    } else {
        CallPullerMethod(dma_pusher, method_call);
    }
}

/// Calls a GPU multivalue method.
void Puller::CallMultiMethod(DmaPusher& dma_pusher, u32 method, u32 subchannel, const u32* base_start, u32 amount, u32 methods_pending) {
    LOG_TRACE(HW_GPU, "Processing method {:08X} on subchannel {}", method, subchannel);
    ASSERT(subchannel < bound_engines.size());
    if (ExecuteMethodOnEngine(dma_pusher, method)) {
        CallEngineMultiMethod(dma_pusher, method, subchannel, base_start, amount, methods_pending);
    } else {
        for (u32 i = 0; i < amount; i++) {
            CallPullerMethod(dma_pusher, MethodCall{
                method,
                base_start[i],
                subchannel,
                methods_pending - i,
            });
        }
    }
}

/// Determines where the method should be executed.
[[nodiscard]] bool Puller::ExecuteMethodOnEngine(DmaPusher& dma_pusher, u32 method) {
    const auto buffer_method = BufferMethods(method);
    return buffer_method >= BufferMethods::NonPullerMethods;
}

} // namespace Tegra::Engines
