// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "common/common_types.h"
#include "video_core/mt/lite3_front_switches.h"
#include "video_core/mt/lite_switches.h"

/// MT lite iteration 4 (docs/mt-lite-plan4.md): the front's drains. On by default under
/// EDEN_NX2_MT_LITE=1 (with EDEN_NX2_MT=1); EDEN_NX2_MT_LITE_<X>=0 turns one off. The front pieces
/// (L19-L22) also turn on with '1' without the MT back (plain split); L23 needs EDEN_NX2_MT.
/// Read once.
namespace VideoCommon::MT {

/// EDEN_NX2_MT_LITE_OVERLAY (L19): Maxwell3D::FrontDmaRead lays the known payloads of queued
/// releases over memory before it considers the partial (position) wait, which since
/// EDEN_NX2_MT_LITE_DMAPARTIAL took every such read first; wider noted writes keep the value of
/// their first word (16-byte releases), and 3D payload reports note their payload.
[[nodiscard]] inline bool LiteOverlayFirst() {
    static const bool on = Lite3Detail::Read("EDEN_NX2_MT_LITE_OVERLAY");
    return on;
}

/// EDEN_NX2_MT_LITE_FOREIGNACQ (L20): an unpredicted semaphore acquire on a word nothing queued
/// can write (no release, add or noted write pending, never a QMD/DMA release target, page never
/// GPU-written) suspends the channel without draining the back; fences are sent, not waited for.
/// Its resume polls drain every 64th poll instead of every 8th.
[[nodiscard]] inline bool LiteForeignAcquire() {
    static const bool on = Lite3Detail::Read("EDEN_NX2_MT_LITE_FOREIGNACQ");
    return on;
}

/// EDEN_NX2_MT_LITE_ASYNCFENCES (L21): an acquire still unmet after its drain sends the fence
/// release to the back without waiting for it (the resume poll sees what it writes).
[[nodiscard]] inline bool LiteAsyncFences() {
    static const bool on = Lite3Detail::Read("EDEN_NX2_MT_LITE_ASYNCFENCES");
    return on;
}

/// EDEN_NX2_MT_LITE_QMDLEDGER (L22): the back records the QMD max releases it ran whose fence
/// has not written memory yet (QmdMaxLedger); a GEQ acquire one of them meets passes without a
/// drain. On a word QMD max releases target, an unpredicted acquire first waits for the
/// sequencer only (no committer settle) and looks again.
[[nodiscard]] inline bool LiteQmdLedger() {
    static const bool on = Lite3Detail::Read("EDEN_NX2_MT_LITE_QMDLEDGER");
    return on;
}

/// EDEN_NX2_MT_LITE_SEQTARGET (L23): the sequencer clears the front's wait_target once it stored
/// `consumed` past it, instead of a Mark + seq_cst consumed store after every following entry
/// until the front (waiting for the committer meanwhile) clears it.
[[nodiscard]] inline bool LiteSeqTarget() {
    static const bool on = LiteDetail::Read("EDEN_NX2_MT_LITE_SEQTARGET");
    return on;
}

/// L20: GPU words a back-only release (QMD release, DMA engine semaphore) ever targeted, by GPU
/// address (4-byte granules hashed into 1M bits). Bits are only set, never cleared: a set bit may
/// be a collision (the acquire then drains as before), a clear bit is never a back target.
class BackReleaseTargets {
public:
    static BackReleaseTargets& Get() {
        static BackReleaseTargets instance;
        return instance;
    }
    /// Back: a release to `gpu_address` was run.
    void Note(u64 gpu_address) {
        const u64 bit = Hash(gpu_address);
        auto& word = bits[bit / 64];
        const u64 mask = u64{1} << (bit % 64);
        if ((word.load(std::memory_order_relaxed) & mask) == 0) {
            word.fetch_or(mask, std::memory_order_relaxed);
        }
    }
    /// Front: whether a back-only release may ever have targeted the word.
    [[nodiscard]] bool MayBeTarget(u64 gpu_address) const {
        const u64 bit = Hash(gpu_address);
        return ((bits[bit / 64].load(std::memory_order_relaxed) >> (bit % 64)) & 1) != 0;
    }

private:
    static constexpr u64 Bits = u64{1} << 20;
    [[nodiscard]] static u64 Hash(u64 gpu_address) {
        return ((gpu_address >> 2) * 0x9E3779B97F4A7C15ULL) >> (64 - 20);
    }
    std::array<std::atomic<u64>, Bits / 64> bits{};
};

/// L22: QMD max releases the back ran whose fence callback has not written memory yet, by device
/// address: the largest such payload. Max releases only raise the word, so once the back ran one
/// (it is ahead of everything the front sends next in the ring), the word ends >= its payload.
/// An entry is dropped by the callback of a release whose payload is >= it (in order, the larger
/// ones still in flight keep theirs); dropping too early only costs a drain.
class QmdMaxLedger {
public:
    static QmdMaxLedger& Get() {
        static QmdMaxLedger instance;
        return instance;
    }
    /// Back, when the QMD runs (before its fence is signalled).
    void Add(u64 device_address, u32 payload) {
        std::scoped_lock lock{mutex};
        if (entries.size() >= 4096) {
            entries.clear();
        }
        auto [it, inserted] = entries.try_emplace(device_address, payload);
        if (!inserted && it->second < payload) {
            it->second = payload;
        }
        count.store(entries.size(), std::memory_order_relaxed);
    }
    /// The fence callback, after it wrote memory.
    void Landed(u64 device_address, u32 payload) {
        if (count.load(std::memory_order_relaxed) == 0) {
            return;
        }
        std::scoped_lock lock{mutex};
        if (const auto it = entries.find(device_address);
            it != entries.end() && it->second <= payload) {
            entries.erase(it);
        }
        count.store(entries.size(), std::memory_order_relaxed);
    }
    /// Front: whether an in-flight max release leaves the word >= `value`.
    [[nodiscard]] bool Meets(u64 device_address, u32 value) {
        if (count.load(std::memory_order_relaxed) == 0) {
            return false;
        }
        std::scoped_lock lock{mutex};
        const auto it = entries.find(device_address);
        return it != entries.end() && it->second >= value;
    }

private:
    std::mutex mutex;
    std::unordered_map<u64, u32> entries;
    std::atomic<size_t> count{};
};

} // namespace VideoCommon::MT
