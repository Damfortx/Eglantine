// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <memory>

#include "common/common_types.h"

namespace VideoCommon::MT {

/// Count per 64 KiB device page (36-bit device address space: 1M pages), changed by one writer
/// and read by any thread. Ranges beyond the address space are ignored by Add and reported as
/// possibly used by Any.
class PageCounter {
public:
    static constexpr u32 PageBits = 16;
    static constexpr u64 NumPages = u64{1} << (36 - PageBits);

    PageCounter() : counts{std::make_unique<std::atomic<u32>[]>(NumPages)} {}

    void Add(u64 device_address, u64 size, s32 delta) noexcept {
        if (size == 0) {
            return;
        }
        const u64 first = device_address >> PageBits;
        if (first >= NumPages) {
            return;
        }
        const u64 last = (std::min)((device_address + size - 1) >> PageBits, NumPages - 1);
        for (u64 page = first; page <= last; ++page) {
            counts[page].fetch_add(static_cast<u32>(delta), std::memory_order_release);
        }
    }

    [[nodiscard]] bool Any(u64 device_address, u64 size) const noexcept {
        if (size == 0) {
            return false;
        }
        const u64 first = device_address >> PageBits;
        if (first >= NumPages) {
            return true;
        }
        const u64 last = (std::min)((device_address + size - 1) >> PageBits, NumPages - 1);
        for (u64 page = first; page <= last; ++page) {
            if (counts[page].load(std::memory_order_acquire) != 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] u32 Count(u64 device_address) const noexcept {
        const u64 page = device_address >> PageBits;
        return page < NumPages ? counts[page].load(std::memory_order_acquire) : 0;
    }

private:
    std::unique_ptr<std::atomic<u32>[]> counts;
};

} // namespace VideoCommon::MT
