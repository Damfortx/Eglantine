// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bit>
#include <cstring>

#include "common/common_types.h"

namespace VideoCommon::MT {

/// Registers written since the last delta was emitted (4096 methods covers Maxwell3D's 0xE00
/// and the compute engine's register files).
class RegWriteSet {
public:
    static constexpr u32 NumBits = 4096;
    static constexpr u32 NumWords = NumBits / 64;
    static constexpr u32 NumSummaryBits = NumWords; // one summary bit per word

    void Mark(u32 method) noexcept {
        const u32 word = (method / 64) & (NumWords - 1);
        words[word] |= u64{1} << (method % 64);
        summary |= u64{1} << word;
    }

    void MarkRange(u32 method, u32 count) noexcept {
        for (u32 i = 0; i < count; ++i) {
            Mark(method + i);
        }
    }

    [[nodiscard]] bool Test(u32 method) const noexcept {
        return ((words[(method / 64) & (NumWords - 1)] >> (method % 64)) & 1) != 0;
    }

    [[nodiscard]] bool Any() const noexcept {
        return summary != 0;
    }

    /// Calls func(method) for each marked method, ascending.
    template <typename Func>
    void ForEach(Func&& func) const {
        u64 pending_words = summary;
        while (pending_words != 0) {
            const u32 word = static_cast<u32>(std::countr_zero(pending_words));
            pending_words &= pending_words - 1;
            u64 bits = words[word];
            while (bits != 0) {
                const u32 bit = static_cast<u32>(std::countr_zero(bits));
                bits &= bits - 1;
                func(word * 64 + bit);
            }
        }
    }

    [[nodiscard]] u32 Count() const noexcept {
        u32 count = 0;
        u64 pending_words = summary;
        while (pending_words != 0) {
            const u32 word = static_cast<u32>(std::countr_zero(pending_words));
            pending_words &= pending_words - 1;
            count += static_cast<u32>(std::popcount(words[word]));
        }
        return count;
    }

    void Clear() noexcept {
        u64 pending_words = summary;
        while (pending_words != 0) {
            const u32 word = static_cast<u32>(std::countr_zero(pending_words));
            pending_words &= pending_words - 1;
            words[word] = 0;
        }
        summary = 0;
    }

    /// Writes (method | value << 32) pairs of the marked registers from `reg_array` into `out`
    /// (room for Count() entries), clears the set, returns how many were written.
    u32 Drain(const u32* reg_array, u32 num_regs, u64* out) noexcept {
        u32 count = 0;
        ForEach([&](u32 method) {
            if (method < num_regs) {
                out[count++] = method | (u64{reg_array[method]} << 32);
            }
        });
        Clear();
        return count;
    }

private:
    std::array<u64, NumWords> words{};
    u64 summary = 0;
};
static_assert(RegWriteSet::NumWords <= 64);

/// Applies register deltas to a mirror engine the way the real engine stores them: the value,
/// then the dirty flags of both dirty tables (Maxwell3D::ApplyState without the side effects).
/// Engines without dirty tables (compute) only store.
template <class Engine>
struct MirrorApplier {
    static void Apply(Engine& mirror, u32 method, u32 value) noexcept {
        mirror.regs.reg_array[method] = value;
        if constexpr (requires { mirror.dirty.tables; }) {
            for (const auto& table : mirror.dirty.tables) {
                mirror.dirty.flags[table[method]] = true;
            }
        }
    }

    static void ApplyPairs(Engine& mirror, const u64* pairs, u32 count) noexcept {
        for (u32 i = 0; i < count; ++i) {
            Apply(mirror, static_cast<u32>(pairs[i]), static_cast<u32>(pairs[i] >> 32));
        }
    }
};

/// Pseudo-delta for a constant buffer bind (Maxwell3D::ProcessCBBind's state change).
struct CbBindDelta {
    u8 stage;
    u8 slot;
    u8 enabled;
    u8 pad;
    u32 size;
    u64 gpu_addr;
};

/// Pseudo-delta for a root table word (methods 0x504/0x508 keep their state outside regs).
struct RootTableDelta {
    u8 table;
    u8 index; // word index
    u16 pad;
    u32 value;
};

} // namespace VideoCommon::MT
