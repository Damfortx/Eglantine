// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "common/common_types.h"

namespace VideoCommon::MT {

/// EDEN_NX2_MT_LITE_XLATE: GPU -> device address translations of one thread (the sequencer's
/// captures), cached per 4 KiB GPU page. A translation is linear inside a 4 KiB page (GPU pages
/// are 4 KiB or 64 KiB, device pages 4 KiB). As the sequencer's other layout memos, only an odd
/// (stable) layout generation read before the walk is kept, and a lookup matches only while the
/// manager's generation is still that one; the manager id tells channels apart. Unmapped
/// addresses are not cached.
class TranslationCache {
public:
    static constexpr u64 PageBits = 12;
    static constexpr u64 PageMask = (u64{1} << PageBits) - 1;

    template <typename Memory>
    [[nodiscard]] std::optional<DAddr> Translate(const Memory& memory, GPUVAddr gpu_addr) {
        const u64 generation = memory.LayoutGeneration();
        const u64 page = gpu_addr >> PageBits;
        const size_t id = memory.GetID();
        Entry& entry = entries[(page ^ (page >> 9)) & (entries.size() - 1)];
        if ((generation & 1) != 0 && entry.generation == generation && entry.page == page &&
            entry.manager_id == id) {
            return entry.base + (gpu_addr & PageMask);
        }
        const std::optional<DAddr> translated = memory.GpuToCpuAddress(gpu_addr);
        if (translated) {
            entry = Entry{page, *translated - (gpu_addr & PageMask), generation, id};
        }
        return translated;
    }

private:
    struct Entry {
        u64 page = 0;
        DAddr base = 0;
        u64 generation = 0; ///< even: empty
        size_t manager_id = 0;
    };
    std::array<Entry, 128> entries{};
};

} // namespace VideoCommon::MT
