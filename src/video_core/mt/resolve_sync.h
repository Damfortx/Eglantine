// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <thread>

#include "common/common_types.h"

/// Synchronization between the sequencer, the resolvers and the committer of the multithreaded
/// back (docs/mt-phase5-spec.md §3.3, §4). Header-only and free of emulator types, so the
/// standalone stress tests (mt/selftest_phase5.h) exercise the same code.
namespace VideoCommon::MT {

inline void CpuPause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

/// One resolver's progress: batches are resolved in increasing order and published with one
/// release store each. `wake` also moves when the resolver starves (it waits for space the
/// committer frees), so a waiting committer can relieve the pressure.
class BatchCursor {
public:
    /// Resolver: every batch up to and including `batch` is complete.
    void Publish(u64 batch) noexcept {
        published.store(batch + 1, std::memory_order_release);
        Wake();
    }

    /// Resolver: something the committer may act on changed (starved flags).
    void Wake() noexcept {
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_all();
    }

    [[nodiscard]] bool Done(u64 batch) const noexcept {
        return published.load(std::memory_order_acquire) > batch;
    }

    /// Committer: blocks until `batch` is published; `idle()` runs between waits (pressure
    /// relief), `stop()` ends the wait early (returns false).
    template <typename Idle, typename Stop>
    bool Wait(u64 batch, Idle&& idle, Stop&& stop) {
        for (u32 spins = 0; spins < 2048; ++spins) {
            if (Done(batch)) {
                return true;
            }
            CpuPause();
        }
        while (true) {
            const u32 seen = wake.load(std::memory_order_seq_cst);
            if (Done(batch)) {
                return true;
            }
            idle();
            if (Done(batch)) {
                return true;
            }
            if (stop()) {
                return false;
            }
            wake.wait(seen, std::memory_order_seq_cst);
        }
    }

    [[nodiscard]] u64 PublishedCount() const noexcept {
        return published.load(std::memory_order_acquire);
    }

private:
    alignas(64) std::atomic<u64> published{0};
    alignas(64) std::atomic<u32> wake{0};
};

/// Results indexed by draw sequence number. The producer side is guarded by back-pressure: the
/// sequencer never emits draw `seq` while `seq - committed >= Slots - margin`, so a slot is
/// rewritten only after its previous occupant was committed.
template <class T, u32 SlotCount>
class SeqSlotRing {
    static_assert((SlotCount & (SlotCount - 1)) == 0);

public:
    static constexpr u32 Slots = SlotCount;

    [[nodiscard]] T& Slot(u64 seq) noexcept {
        return slots[seq & (Slots - 1)];
    }

    /// Sequencer: whether draw `seq` may be emitted with `committed` the last committed seq.
    [[nodiscard]] static constexpr bool HasRoom(u64 seq, u64 committed, u32 margin) noexcept {
        return seq - committed < Slots - margin;
    }

private:
    std::unique_ptr<T[]> slots{std::make_unique<T[]>(Slots)};
};

/// A Serial entry that also stops the resolvers: the committer runs it and stores `done`, each
/// resolver then copies what it needs from the real engines and acks. The sequencer waits for
/// both, so it does not touch the engines before every resolver copied them.
struct BarrierAcks {
    std::atomic<u32> done{0};
    std::atomic<u32> acks{0};

    /// Sequencer.
    template <typename Idle>
    void WaitAll(u32 resolvers, Idle&& idle) const {
        for (u32 spins = 0;; ++spins) {
            if (done.load(std::memory_order_acquire) != 0 &&
                acks.load(std::memory_order_acquire) == resolvers) {
                return;
            }
            if (spins < 4096) {
                CpuPause();
            } else {
                idle();
            }
        }
    }

    /// Resolver: spins until the committer ran the Serial.
    template <typename Stop>
    bool WaitDone(Stop&& stop) const {
        for (u32 spins = 0; done.load(std::memory_order_acquire) == 0; ++spins) {
            if (stop()) {
                return false;
            }
            if (spins < 4096) {
                CpuPause();
            } else {
                std::this_thread::yield();
            }
        }
        return true;
    }

    void Ack() noexcept {
        acks.fetch_add(1, std::memory_order_acq_rel);
    }
};

} // namespace VideoCommon::MT
