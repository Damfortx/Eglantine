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

inline void BumpWriteGeneration(u64 device_addr, u64 size) {
    if (g_ubo_gen_on.load(std::memory_order_relaxed)) {
        UboWriteGenerations().Bump(device_addr, size);
    }
}

} // namespace VideoCommon::MT
