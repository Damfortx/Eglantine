// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <span>

#include "common/common_types.h"

/// What the multithreaded back's sequencer read from guest memory for a draw (docs/mt-plan.md,
/// phase 4), as the committer sees it while it records the draw: the caches' guest reads on the
/// draw path consult it first, so the committer records what the guest memory held at the draw
/// even though the sequencer has written it since.
namespace VideoCommon::MT {

/// Guest bytes at a GPU address (texture handles, TIC/TSC entries, storage buffer descriptors).
struct OverlayEntry {
    u64 gpu_addr;
    u32 size;
    u32 offset; ///< into the blob
};
static_assert(sizeof(OverlayEntry) == 16);

/// A uniform buffer bind: streamed from these bytes (staged) or bound from the cache (cached;
/// its CPU-modified bytes travel as uploads). EDEN_NX2_MT_UBO_GEN: staged == 2 streams the bytes
/// the committer kept for the slot; `memo` != 0 on a staged == 1 entry makes it keep these.
/// EDEN_NX2_MT_COMPACT: staged == 3 is a patch of the kept bytes (a u64 mask of changed
/// UboPatchChunk-byte chunks, then those chunks, at `offset`); the committer applies it before
/// anything reads the packet, so it streams the kept bytes like staged == 2.
struct UboEntry {
    u8 stage;
    u8 index;
    u8 staged;
    u8 memo;
    u32 size;
    u64 device_addr;
    u32 offset; ///< into the blob, staged only
    u32 pad2;
};
static_assert(sizeof(UboEntry) == 24);

/// CPU-modified bytes of a range the draw binds (taken from the buffer tracker at capture), to
/// be copied into the buffer that holds [range_addr, range_addr + range_size) before the draw.
struct UploadEntry {
    u64 range_addr;
    u32 range_size;
    u32 size;
    u64 device_addr;
    u64 data; ///< offset into the blob, or a heap pointer (heap != 0) the committer frees
    u32 heap;
    u32 pad;
};
static_assert(sizeof(UploadEntry) == 40);

struct CaptureView {
    std::span<const OverlayEntry> overlay;
    std::span<const UboEntry> ubos;
    const u8* blob = nullptr;
    bool verify = false;
    /// EDEN_NX2_MT_UBO_GEN: the committer's per-slot copies (UboMemoBytes each).
    const u8* ubo_memo = nullptr;
    mutable u64 overlay_misses = 0;
    mutable u64 ubo_misses = 0;
    mutable u64 decision_mismatches = 0;
    mutable u64 uncaptured_syncs = 0;
    mutable u64 first_uncaptured_addr = 0;
    mutable u64 first_uncaptured_size = 0;
    /// EDEN_NX2_MT_VERIFY: GuestCpuWrites() at the capture (writes after it are races, not
    /// capture misses), and the syncs it explained.
    u64 cpu_write_since = 0;
    mutable u64 concurrent_cpu_syncs = 0;
    /// EDEN_NX2_MT_COMPACT: the overlay's bytes when they are not in `blob` (a decoded packet).
    const u8* overlay_blob = nullptr;

    /// Copies [gpu_addr, +size) when one entry holds all of it.
    [[nodiscard]] bool Read(u64 gpu_addr, void* out, size_t size) const noexcept {
        const auto it = std::upper_bound(
            overlay.begin(), overlay.end(), gpu_addr,
            [](u64 addr, const OverlayEntry& entry) { return addr < entry.gpu_addr; });
        if (it == overlay.begin()) {
            return false;
        }
        const OverlayEntry& entry = *(it - 1);
        if (gpu_addr + size > entry.gpu_addr + entry.size) {
            return false;
        }
        const u8* const base = overlay_blob != nullptr ? overlay_blob : blob;
        std::memcpy(out, base + entry.offset + (gpu_addr - entry.gpu_addr), size);
        return true;
    }

    /// A staged uniform buffer's bytes.
    [[nodiscard]] const u8* UboBytes(const UboEntry& entry) const noexcept {
        if (entry.staged >= 2) {
            return ubo_memo + (size_t{entry.stage} * UboMemoSlots + entry.index) * UboMemoBytes;
        }
        return blob + entry.offset;
    }

    static constexpr size_t UboMemoStages = 6; ///< 5 graphics stages and compute
    static constexpr size_t UboMemoSlots = 18;
    static constexpr size_t UboMemoBytes = 4096;
    /// EDEN_NX2_MT_COMPACT patches: 64 chunks (one u64 mask) cover UboMemoBytes.
    static constexpr size_t UboPatchChunk = 64;
    static_assert(UboMemoBytes / UboPatchChunk <= 64);

    [[nodiscard]] const UboEntry* FindUbo(u32 stage, u32 index) const noexcept {
        for (const UboEntry& entry : ubos) {
            if (entry.stage == stage && entry.index == index) {
                return &entry;
            }
        }
        return nullptr;
    }
};

/// Set on the committer while it records a packet.
inline thread_local const CaptureView* t_capture = nullptr;

/// A guest read of the draw path: the captured bytes while a packet is recorded.
template <typename T, typename Memory>
[[nodiscard]] inline T ReadCaptured(const Memory& memory, u64 gpu_addr) {
    if (const CaptureView* const capture = t_capture) [[unlikely]] {
        T value;
        if (capture->Read(gpu_addr, &value, sizeof(T))) {
            return value;
        }
        ++capture->overlay_misses;
    }
    return memory.template Read<T>(gpu_addr);
}

/// Device-memory stand-in over a uniform buffer's captured bytes (uniform_stream_reuse.h).
struct CapturedBytes {
    const u8* data;
    u64 base;
    [[nodiscard]] const u8* GetSpan(u64 addr, size_t) const noexcept {
        return data + (addr - base);
    }
    void ReadBlockUnsafe(u64 addr, void* out, size_t size) const noexcept {
        std::memcpy(out, data + (addr - base), size);
    }
};

} // namespace VideoCommon::MT
