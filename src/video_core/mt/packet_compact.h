// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "common/assert.h"
#include "common/common_types.h"
#include "video_core/mt/capture.h"
#include "video_core/mt/c12_capture.h"

/// EDEN_NX2_MT_COMPACT: the stream-ordered state behind compact draw packets.
///
/// TIC/TSC entries: the sequencer keeps a direct-mapped table of the 32-byte entries it sent,
/// and every reader of draw packets (the committer and each resolver) keeps a replica of it.
/// A compact draw packet names an entry the table holds unchanged by its slot (2 bytes instead
/// of 32 bytes plus a 16-byte overlay entry), and lists the entries it carries that replace a
/// slot ("inserts"). Every reader applies every compact draw packet's inserts in stream order
/// (resolvers also those of the batches they do not resolve), after resolving the packet's own
/// references against the table as it was before the packet: the sequencer decides the same
/// way, so all replicas equal the sequencer's table at each packet.
namespace VideoCommon::MT {

struct DescriptorReplica {
    static constexpr u32 SlotBits = 12;
    static constexpr u32 Slots = 1u << SlotBits;
    static constexpr u32 EntryBytes = 32;
    struct Slot {
        u64 gpu_addr;
        std::array<u8, EntryBytes> bytes;
    };
    std::unique_ptr<Slot[]> slots{new Slot[Slots]{}};

    [[nodiscard]] static u32 SlotOf(u64 gpu_addr) noexcept {
        return static_cast<u32>(((gpu_addr >> 5) * 0x9E3779B97F4A7C15ULL) >> (64 - SlotBits));
    }
    /// Only while no packet is in flight (Start): every copy must start equal.
    void Reset() noexcept {
        std::fill_n(slots.get(), Slots, Slot{});
    }
};

/// A packet reader's replica and the decoded overlay of the last packet it parsed (valid until
/// its next Decode on the same object; one object per reading thread).
struct PacketDecoder {
    DescriptorReplica table;
    std::vector<OverlayEntry> overlay;
    std::vector<u8> bytes;

    // ---- Uniform replica (a resolver with EDEN_NX2_MT_RBUF) ----
    // The bytes the committer keeps per graphics uniform slot (staged == 1 with `memo`, patched
    // by staged == 3, read by staged == 2/3), kept by a reader that streams uniforms itself. It
    // applies every draw packet's entries in stream order, those it resolves and those it skips,
    // so it equals the committer's at each packet. `version` names the bytes (bumped on every
    // change): the same version of a slot is the same bytes (the resolver's ring memo).
    static constexpr u32 UniformStages = 5;
    std::unique_ptr<u8[]> uniform_kept;
    std::array<u64, UniformStages * CaptureView::UboMemoSlots> uniform_version{};

    void EnableUniforms() {
        if (!uniform_kept) {
            uniform_kept = std::make_unique<u8[]>(size_t{UniformStages} *
                                                  CaptureView::UboMemoSlots *
                                                  CaptureView::UboMemoBytes);
        }
    }

    [[nodiscard]] static bool UniformSlot(const UboEntry& entry) noexcept {
        return entry.stage < UniformStages && entry.index < CaptureView::UboMemoSlots &&
               entry.size <= CaptureView::UboMemoBytes;
    }

    [[nodiscard]] u8* UniformKept(const UboEntry& entry) noexcept {
        return uniform_kept.get() +
               (size_t{entry.stage} * CaptureView::UboMemoSlots + entry.index) *
                   CaptureView::UboMemoBytes;
    }

    /// A draw packet's uniform entries (what KeepUniformMemos does on the committer).
    void ApplyUniforms(std::span<const UboEntry> ubos, const u8* blob);

    /// The bytes of a streamed entry (after ApplyUniforms of its packet) and their identity
    /// (0: none, compare the bytes). Null when this reader keeps no replica for it.
    [[nodiscard]] const u8* UniformBytes(const UboEntry& entry, const u8* blob, u64& source) {
        source = 0;
        if (entry.staged == 1) {
            if (entry.memo != 0 && uniform_kept && UniformSlot(entry)) {
                source = uniform_version[entry.stage * CaptureView::UboMemoSlots + entry.index];
            }
            return blob + entry.offset;
        }
        if ((entry.staged == 2 || entry.staged == 3) && uniform_kept && UniformSlot(entry)) {
            source = uniform_version[entry.stage * CaptureView::UboMemoSlots + entry.index];
            return UniformKept(entry);
        }
        return nullptr;
    }

    /// The overlay of a compact packet: its own entries (bytes in `blob`) and the slots it
    /// references (sorted by address, disjoint from them), merged by address.
    void Decode(std::span<const OverlayEntry> wire, const u8* blob, std::span<const u16> refs) {
        overlay.clear();
        bytes.clear();
        overlay.reserve(wire.size() + refs.size());
        const auto push = [&](u64 gpu_addr, u32 size, const u8* src) {
            const u32 offset = static_cast<u32>(bytes.size());
            bytes.insert(bytes.end(), src, src + size);
            overlay.push_back({gpu_addr, size, offset});
        };
        size_t ref = 0;
        const auto push_ref = [&] {
            const DescriptorReplica::Slot& slot = table.slots[refs[ref++]];
            push(slot.gpu_addr, DescriptorReplica::EntryBytes, slot.bytes.data());
        };
        for (const OverlayEntry& entry : wire) {
            while (ref < refs.size() && table.slots[refs[ref]].gpu_addr < entry.gpu_addr) {
                push_ref();
            }
            push(entry.gpu_addr, entry.size, blob + entry.offset);
        }
        while (ref < refs.size()) {
            push_ref();
        }
    }

    // ---- EDEN_NX2_C12_CAPTURE_REF (contingent, c12_capture.h) ----
    // The decoded overlay of the last packet that stored it, per sequencer memo slot. Every
    // reader of every draw packet applies the operation in stream order (ParsePacket, after
    // ApplyInserts), so the slots equal what the sequencer assumes. Never sent with resolvers.
    struct C12RefSlot {
        std::vector<OverlayEntry> overlay;
        std::vector<u8> bytes;
    };
    std::unique_ptr<C12RefSlot[]> c12_ref_slots;

    /// After Decode/ApplyInserts of a compact packet: Store keeps `overlay`/`bytes` in the slot,
    /// Use replaces them (the packet carried none) with the slot's.
    void C12ApplyRef(u8 op, u32 slot) {
        if (op == static_cast<u8>(C12RefOp::None)) {
            return;
        }
        if (!c12_ref_slots) {
            c12_ref_slots = std::make_unique<C12RefSlot[]>(C12RefSlotCount);
        }
        C12RefSlot& kept = c12_ref_slots[slot & (C12RefSlotCount - 1)];
        if (op == static_cast<u8>(C12RefOp::Store)) {
            kept.overlay.assign(overlay.begin(), overlay.end());
            kept.bytes.assign(bytes.begin(), bytes.end());
        } else if (op == static_cast<u8>(C12RefOp::Use)) {
            overlay.assign(kept.overlay.begin(), kept.overlay.end());
            bytes.assign(kept.bytes.begin(), kept.bytes.end());
        }
    }

    /// The packet's inserts into the replica (after Decode: references see the old slots).
    void ApplyInserts(std::span<const OverlayEntry> wire, const u8* blob,
                      std::span<const u64> inserts) {
        if (inserts.empty()) {
            return;
        }
        const CaptureView view{.overlay = wire, .ubos = {}, .blob = blob};
        for (const u64 gpu_addr : inserts) {
            DescriptorReplica::Slot& slot = table.slots[DescriptorReplica::SlotOf(gpu_addr)];
            const bool found = view.Read(gpu_addr, slot.bytes.data(), DescriptorReplica::EntryBytes);
            ASSERT_MSG(found, "MT compact: insert {:#x} not in the packet", gpu_addr);
            // Not found would be a sequencer bug: an address no read matches turns later
            // references into overlay misses (the old guest read / the resolver's slow path).
            slot.gpu_addr = found ? gpu_addr : ~u64{0};
        }
    }
};

/// Sequencer side of the uniform buffer patches (capture.h, staged == 3): the mask of the
/// UboPatchChunk-byte chunks of [0, size) where `now` differs from `kept`.
[[nodiscard]] inline u64 UniformPatchMask(const u8* now, const u8* kept, u32 size,
                                          u32& changed_bytes) noexcept {
    constexpr u32 Chunk = static_cast<u32>(CaptureView::UboPatchChunk);
    u64 mask = 0;
    changed_bytes = 0;
    for (u32 begin = 0, chunk = 0; begin < size; begin += Chunk, ++chunk) {
        const u32 length = (std::min)(Chunk, size - begin);
        if (std::memcmp(now + begin, kept + begin, length) != 0) {
            mask |= u64{1} << chunk;
            changed_bytes += length;
        }
    }
    return mask;
}

/// Writes the patch (mask, then the changed chunks of `now`) at `out`; returns its size.
inline u32 WriteUniformPatch(u8* out, const u8* now, u32 size, u64 mask) noexcept {
    constexpr u32 Chunk = static_cast<u32>(CaptureView::UboPatchChunk);
    std::memcpy(out, &mask, sizeof(mask));
    u32 cursor = sizeof(mask);
    for (u64 bits = mask; bits != 0; bits &= bits - 1) {
        const u32 begin = static_cast<u32>(std::countr_zero(bits)) * Chunk;
        const u32 length = (std::min)(Chunk, size - begin);
        std::memcpy(out + cursor, now + begin, length);
        cursor += length;
    }
    return cursor;
}

inline void ApplyUniformPatch(u8* kept, const u8* patch, u32 size) noexcept;

inline void PacketDecoder::ApplyUniforms(std::span<const UboEntry> ubos, const u8* blob) {
    if (!uniform_kept) {
        return;
    }
    for (const UboEntry& entry : ubos) {
        if (!UniformSlot(entry)) {
            continue;
        }
        const size_t slot = size_t{entry.stage} * CaptureView::UboMemoSlots + entry.index;
        if (entry.staged == 1 && entry.memo != 0) {
            std::memcpy(UniformKept(entry), blob + entry.offset, entry.size);
            ++uniform_version[slot];
        } else if (entry.staged == 3) {
            ApplyUniformPatch(UniformKept(entry), blob + entry.offset, entry.size);
            ++uniform_version[slot];
        }
    }
}

/// Committer side: applies a patch to the kept bytes of a slot.
inline void ApplyUniformPatch(u8* kept, const u8* patch, u32 size) noexcept {
    constexpr u32 Chunk = static_cast<u32>(CaptureView::UboPatchChunk);
    u64 mask;
    std::memcpy(&mask, patch, sizeof(mask));
    u32 cursor = sizeof(mask);
    for (u64 bits = mask; bits != 0; bits &= bits - 1) {
        const u32 begin = static_cast<u32>(std::countr_zero(bits)) * Chunk;
        const u32 length = (std::min)(Chunk, size - begin);
        std::memcpy(kept + begin, patch + cursor, length);
        cursor += length;
    }
}

} // namespace VideoCommon::MT
