// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>

#include "common/common_types.h"
#include "video_core/mt/mt_switches.h"

/// Phase 7 of docs/mt-plan.md ("sequencer diet"): switches, and the uniform buffer write
/// generations (EDEN_NX2_MT_UBO_GEN).
namespace VideoCommon::MT {

namespace SeqDietDetail {
[[nodiscard]] inline bool Switch(const char* name, bool default_on) {
    const char mode = Tegra::Nx2EnvMode(name);
    return Enabled() && Tegra::Nx2Switch(mode, default_on);
}
} // namespace SeqDietDetail

/// EDEN_NX2_MT_COND (on): conditional rendering as a Command when the sequencer knows the answer.
[[nodiscard]] inline bool CondCommands() {
    static const bool on = SeqDietDetail::Switch("EDEN_NX2_MT_COND", true);
    return on;
}
/// EDEN_NX2_MT_INDIRECT (on): indirect draws as draw packets.
[[nodiscard]] inline bool IndirectPackets() {
    static const bool on = SeqDietDetail::Switch("EDEN_NX2_MT_INDIRECT", true);
    return on;
}
/// EDEN_NX2_MT_DRAWTEX (on): DrawTexture as a Command with its TIC/TSC entries.
[[nodiscard]] inline bool DrawTextureCommands() {
    static const bool on = SeqDietDetail::Switch("EDEN_NX2_MT_DRAWTEX", true);
    return on;
}
/// EDEN_NX2_MT_DMA=packet: DMA buffer copies/clears as Commands (default serial).
[[nodiscard]] inline bool DmaPackets() {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_NX2_MT_DMA");
        return Enabled() && value != nullptr && value[0] == 'p';
    }();
    return on;
}
/// EDEN_NX2_MT_FLUSHCMD (on): cache flushes and invalidations on the sequencer skip the Serial
/// when nothing can need them, run as ordered Commands where the sequencer reads no result, and
/// stay Serial otherwise (vk_mt_seq.h, SeqFlushRegion / SeqInvalidateRegion).
[[nodiscard]] inline bool FlushCommands() {
    static const bool on = SeqDietDetail::Switch("EDEN_NX2_MT_FLUSHCMD", true);
    return on;
}

/// Sequencer: set while a function the split's front deferred and then drains for runs
/// (GpuSplit::RunFlush). The front reads the flushed memory only once the committer passed the
/// drain's mark, and the sequencer gets nothing else to run before that: a flush may be queued
/// as a Command instead of waited for.
inline thread_local bool t_flush_async = false;

class AsyncFlushScope {
public:
    AsyncFlushScope() noexcept : saved{t_flush_async} {
        t_flush_async = true;
    }
    ~AsyncFlushScope() {
        t_flush_async = saved;
    }
    AsyncFlushScope(const AsyncFlushScope&) = delete;
    AsyncFlushScope& operator=(const AsyncFlushScope&) = delete;

private:
    bool saved;
};

/// EDEN_NX2_MT_UBO_GEN (off): reuse a uniform buffer's last staging while its pages are unchanged.
[[nodiscard]] inline bool UboGen() {
    static const bool on = SeqDietDetail::Switch("EDEN_NX2_MT_UBO_GEN", false);
    return on;
}

/// Write generations of 4 KiB device pages, hashed into a fixed table (an aliased page only
/// makes a reuse miss). Bumped by every guest write the GPU side can see (sequencer and
/// committer writes through the memory manager, guest CPU writes that reach the buffer tracker,
/// and the tracker dropping a page's protection) and read by the sequencer's uniform capture.
class WriteGenerations {
public:
    static constexpr u32 PageBits = 12;
    static constexpr size_t TableSize = size_t{1} << 20;

    void Bump(u64 device_addr, u64 size) noexcept {
        if (size == 0) {
            return;
        }
        const u64 first = device_addr >> PageBits;
        const u64 last = (device_addr + size - 1) >> PageBits;
        for (u64 page = first; page <= last && page - first < TableSize; ++page) {
            table[page & (TableSize - 1)].fetch_add(1, std::memory_order_release);
        }
    }

    /// Sum of the range's generations (changes whenever any of them does).
    [[nodiscard]] u64 Read(u64 device_addr, u64 size) const noexcept {
        if (size == 0) {
            return 0;
        }
        const u64 first = device_addr >> PageBits;
        const u64 last = (device_addr + size - 1) >> PageBits;
        u64 sum = 0;
        for (u64 page = first; page <= last && page - first < TableSize; ++page) {
            sum += u64{table[page & (TableSize - 1)].load(std::memory_order_acquire)} << 1;
        }
        return sum | 1; // never 0 (0 = no memo)
    }

private:
    std::unique_ptr<std::atomic<u32>[]> table{new std::atomic<u32>[TableSize]{}};
};

inline WriteGenerations& UboWriteGenerations() {
    static WriteGenerations generations;
    return generations;
}

/// Set while EDEN_NX2_MT_UBO_GEN runs (the committer is up): write paths bump generations.
inline std::atomic<bool> g_ubo_gen_on{};

/// EDEN_NX2_MT_VERIFY: the guest CPU writes the GPU side heard of (traps, invalidations), so the
/// verifier can tell a write made by a game thread between a draw's capture and its commit (a
/// race the old single-threaded back had as well, at another point in time) from a capture bug.
class CpuWriteLog {
public:
    static constexpr u64 Entries = 4096;
    static constexpr u64 PageMask = ~u64{0xfff};

    void Record(u64 device_addr, u64 size) noexcept {
        const u64 n = count.fetch_add(1, std::memory_order_acq_rel) + 1;
        Entry& entry = ring[n & (Entries - 1)];
        entry.n.store(0, std::memory_order_relaxed);
        entry.begin.store(device_addr & PageMask, std::memory_order_relaxed);
        entry.end.store((device_addr + size + 0xfff) & PageMask, std::memory_order_relaxed);
        entry.n.store(n, std::memory_order_release);
    }

    [[nodiscard]] u64 Snapshot() const noexcept {
        return count.load(std::memory_order_acquire);
    }

    /// Whether a write recorded after `since` may overlap [device_addr, +size) (true when the
    /// log wrapped or an entry is being written).
    [[nodiscard]] bool WrittenSince(u64 since, u64 device_addr, u64 size) const noexcept {
        const u64 current = count.load(std::memory_order_acquire);
        if (current == since) {
            return false;
        }
        if (current - since >= Entries) {
            return true;
        }
        const u64 begin = device_addr & PageMask;
        const u64 end = (device_addr + size + 0xfff) & PageMask;
        for (u64 n = since + 1; n <= current; ++n) {
            const Entry& entry = ring[n & (Entries - 1)];
            if (entry.n.load(std::memory_order_acquire) != n) {
                return true;
            }
            if (entry.begin.load(std::memory_order_relaxed) < end &&
                begin < entry.end.load(std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

private:
    struct Entry {
        std::atomic<u64> n{0};
        std::atomic<u64> begin{0};
        std::atomic<u64> end{0};
    };
    alignas(64) std::atomic<u64> count{0};
    std::array<Entry, Entries> ring{};
};

inline CpuWriteLog& GuestCpuWrites() {
    static CpuWriteLog log;
    return log;
}

/// Set while the committer runs with EDEN_NX2_MT_VERIFY.
inline std::atomic<bool> g_cpu_write_log_on{};

/// `guest`: a write a draw's capture may race with: guest CPU writes the GPU side heard of
/// (traps, WriteMemory, invalidations), asynchronous downloads of GPU data into guest memory,
/// and a deleted buffer's range handed back to guest memory (DeleteBuffer marks it
/// CPU-modified). Not the ordered writes the GPU side makes (copies, inline uploads,
/// semaphores).
/// EDEN_NX2_MT_VERIFY: a guest CPU write marked CPU-modified in the tracker (recorded again at
/// the marking: the sequencer's capture may hold the tracker lock between the trap's start and
/// its marking).
inline void NoteGuestWrite(u64 device_addr, u64 size) {
    if (g_cpu_write_log_on.load(std::memory_order_relaxed)) [[unlikely]] {
        GuestCpuWrites().Record(device_addr, size);
    }
}

inline void BumpWriteGeneration(u64 device_addr, u64 size, bool guest = false) {
    if (g_ubo_gen_on.load(std::memory_order_relaxed)) {
        UboWriteGenerations().Bump(device_addr, size);
    }
    if (guest && g_cpu_write_log_on.load(std::memory_order_relaxed)) [[unlikely]] {
        GuestCpuWrites().Record(device_addr, size);
    }
}

} // namespace VideoCommon::MT
