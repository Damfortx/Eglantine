// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <optional>
#include <vector>

#include "common/common_types.h"

/// A resolver's uniform ring (docs/mt-phase5-spec.md §9.3, EDEN_NX2_MT_RBUF): streamed uniform
/// buffer bytes written by one resolver into host-visible memory, bound by the committer's draws.
/// Free of Vulkan types so the standalone stress tests exercise it as is.
///
/// Allocations bump within the resolver's batches (local index = batch / K, dense and increasing).
/// The committer stamps each batch, once it recorded the batch's draws, with the command buffer
/// tick they went into. A batch's region is freed once the batch `ReuseWindow - 1` later is
/// stamped and its tick is complete on the GPU: a later batch reuses an allocation (same bytes for
/// the same slot) only within that window, so every draw that reads a region completed first.
/// Never blocks: a full ring makes the caller fall back.
namespace VideoCommon::MT {

class UniformRingCore {
public:
    static constexpr u64 ReuseWindow = 8;
    static constexpr u64 Records = 1024;

    UniformRingCore() = default;

    /// `capacity` bytes, `alignment` a power of two.
    void Reset(size_t capacity_, u32 alignment_) {
        capacity = capacity_;
        alignment = alignment_ == 0 ? 1 : alignment_;
        head = 0;
        tail = 0;
        open_local = ~u64{0};
        oldest = 0;
        newest_ended = ~u64{0};
        for (auto& record : records) {
            record.end = 0;
            record.stamped.store(0, std::memory_order_relaxed);
            record.tick.store(0, std::memory_order_relaxed);
        }
    }

    // ---- resolver ----

    /// Before the batch's first allocation.
    void BeginBatch(u64 local) noexcept {
        if (open_local == local) {
            return;
        }
        if (open_local != ~u64{0}) {
            EndBatch(open_local);
        }
        // Batches the resolver skipped entirely (none of their draws allocated) end empty.
        if (newest_ended != ~u64{0}) {
            for (u64 skipped = newest_ended + 1; skipped < local; ++skipped) {
                records[skipped % Records].end = head;
                newest_ended = skipped;
            }
        } else {
            oldest = local;
        }
        open_local = local;
    }

    /// At the batch's end: everything allocated so far belongs to it.
    void EndBatch(u64 local) noexcept {
        if (open_local != local) {
            return;
        }
        records[local % Records].end = head;
        newest_ended = local;
        open_local = ~u64{0};
    }

    /// The offset of `size` bytes, or nullopt when the ring is full (nothing waits). `is_free`:
    /// whether a stamped tick completed on the GPU.
    template <typename IsFree>
    [[nodiscard]] std::optional<u64> Allocate(u32 size, IsFree&& is_free) {
        if (size > capacity / 4) {
            return std::nullopt;
        }
        for (u32 attempt = 0; attempt < 2; ++attempt) {
            u64 start = (head + alignment - 1) & ~u64{alignment - 1};
            const u64 offset = start % capacity;
            if (offset + size > capacity) {
                start += capacity - offset; // never wraps inside an allocation
            }
            const u64 end = start + size;
            if (end - tail <= capacity && open_local - oldest < Records) {
                head = end;
                return start % capacity;
            }
            if (!Reclaim(is_free)) {
                break;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] u64 InFlight() const noexcept {
        return head - tail;
    }

    // ---- committer ----

    /// The batch's draws were recorded into command buffers up to `tick`.
    void Stamp(u64 local, u64 tick) noexcept {
        Record& record = records[local % Records];
        record.tick.store(tick, std::memory_order_relaxed);
        record.stamped.store(local + 1, std::memory_order_release);
    }

private:
    struct Record {
        u64 end = 0;                ///< ring position after the batch (resolver)
        std::atomic<u64> stamped{}; ///< local + 1 once stamped (committer)
        std::atomic<u64> tick{};
    };

    /// Frees the oldest batches whose window is complete. False when nothing could be freed.
    template <typename IsFree>
    bool Reclaim(IsFree&& is_free) {
        bool freed = false;
        while (newest_ended != ~u64{0} && oldest + ReuseWindow - 1 <= newest_ended) {
            const u64 guard = oldest + ReuseWindow - 1;
            const Record& record = records[guard % Records];
            if (record.stamped.load(std::memory_order_acquire) != guard + 1 ||
                !is_free(record.tick.load(std::memory_order_relaxed))) {
                break;
            }
            tail = records[oldest % Records].end;
            ++oldest;
            freed = true;
        }
        return freed;
    }

    size_t capacity = 0;
    u64 alignment = 1;
    u64 head = 0;
    u64 tail = 0;
    u64 open_local = ~u64{0};
    u64 oldest = 0;
    u64 newest_ended = ~u64{0};
    std::array<Record, Records> records{};
};

/// The resolver's per-slot memo: reuse the allocation of the slot's last bind when the bytes are
/// the same and it is still inside the reuse window.
class UniformReuseMemo {
public:
    static constexpr u32 Stages = 5;
    static constexpr u32 Slots = 18;
    static constexpr u32 MaxBytes = 4096;

    struct Entry {
        u64 local = ~u64{0};
        u64 offset = 0;
        u64 source = 0; ///< identity of the bytes' source (0: none), compared before the bytes
        u32 size = 0;
        std::vector<u8> bytes;
    };

    /// The offset to reuse, or nullopt.
    [[nodiscard]] std::optional<u64> Find(u32 stage, u32 slot, u64 local, u64 source,
                                          const u8* data, u32 size) {
        if (stage >= Stages || slot >= Slots || size > MaxBytes) {
            return std::nullopt;
        }
        Entry& entry = entries[stage * Slots + slot];
        if (entry.local == ~u64{0} || entry.size != size || local < entry.local ||
            local - entry.local >= UniformRingCore::ReuseWindow) {
            return std::nullopt;
        }
        if (source != 0 && entry.source == source) {
            ++source_hits;
            return entry.offset;
        }
        if (std::memcmp(entry.bytes.data(), data, size) != 0) {
            return std::nullopt;
        }
        entry.source = source;
        ++byte_hits;
        return entry.offset;
    }

    void Store(u32 stage, u32 slot, u64 local, u64 source, u64 offset, const u8* data, u32 size) {
        if (stage >= Stages || slot >= Slots || size > MaxBytes) {
            return;
        }
        Entry& entry = entries[stage * Slots + slot];
        entry.local = local;
        entry.offset = offset;
        entry.source = source;
        entry.size = size;
        entry.bytes.assign(data, data + size);
    }

    void Clear() {
        for (Entry& entry : entries) {
            entry.local = ~u64{0};
        }
    }

    u64 source_hits = 0;
    u64 byte_hits = 0;

private:
    std::array<Entry, Stages * Slots> entries{};
};

} // namespace VideoCommon::MT
