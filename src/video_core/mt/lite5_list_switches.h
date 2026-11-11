// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/c10_front.h"
#include "video_core/mt/c11_front.h"
#include "video_core/mt/lite_switches.h"

/// MT lite iteration 5 (docs/mt-lite-plan5.md): the front's writer waits before command lists on
/// GPU-written pages (DmaPusher::ReadGpuWrittenCommands, "writers (commands)"). On by default
/// under EDEN_NX2_MT_LITE=1 (with EDEN_NX2_MT=1); EDEN_NX2_MT_LITE_<X>=0 turns one off. They need
/// the MT back (the sequencer records the positions). Read once.
namespace VideoCommon::MT {

/// EDEN_NX2_MT_LITE_LISTPOS (L24): the writer wait before a command list on GPU-written pages
/// still waits for the sequencer to run every writer sent, but the committer only up to the last
/// writer the sequencer saw write one of the list's pages (Tegra::Nx2ListWriterPosition) and its
/// queued guest memory writes (committer_write_position), not up to every writer.
[[nodiscard]] inline bool LiteListPos() {
    static const bool on = LiteDetail::Read("EDEN_NX2_MT_LITE_LISTPOS");
    return on;
}

/// EDEN_NX2_MT_LITE_LISTKNOWN (L25): pages the front read CPU command lists from although the GPU
/// wrote them once (Synced: no GPU write since their flush) ListTrust::Streak times in a row over
/// at least ListTrust::MinSpan, with no sequencer capture writing them meanwhile, skip the writer wait like the known CPU list
/// pages that were never GPU-written (same policy: EDEN_NX2_WRITER_WAIT, title default). A page
/// whose list is not Synced any more is never trusted again. Needs the MT back.
[[nodiscard]] inline bool LiteListKnown() {
    static const bool on = LiteDetail::Read("EDEN_NX2_MT_LITE_LISTKNOWN");
    return on;
}

/// EDEN_NX2_MT_LITE_LISTCHECK=1 (diagnostic, off; not part of LITE): a list read with a shorter
/// wait (L24) or none (L25) is read again after the full writer wait and compared; differences
/// are counted as stale and the second read is used. Logged every 10 s ("List check:").
[[nodiscard]] inline bool LiteListCheck() {
    static const bool on = Enabled() && Tegra::Nx2EnvMode("EDEN_NX2_MT_LITE_LISTCHECK") == '1';
    return on;
}

/// EDEN_NX2_LIST_STATS=1 (diagnostic, any mode): why command list fetches on GPU-written pages
/// wait (page state before the wait, whose writers were pending, last writer kind, results, top
/// pages). Counted and logged every 10 s also with EDEN_NX2_GPU_PROFILE (without the page peek
/// and the top pages).
[[nodiscard]] inline bool ListStatsFull() {
    static const bool on = Tegra::Nx2EnvMode("EDEN_NX2_LIST_STATS") == '1';
    return on;
}

/// L25: per channel (DmaPusher), device pages (index = address >> 12) of command lists on
/// GPU-written pages and how far they are trusted as CPU lists.
class ListTrust {
public:
    /// Synced reads in a row (no capture writing the page between them), spread over at least
    /// MinSpan (a few frames: a page the GPU fills in every frame shows it within that time).
    static constexpr u8 Streak = 8;
    static constexpr std::chrono::milliseconds MinSpan{100};

    struct Page {
        std::chrono::steady_clock::time_point first{}; ///< First read of the current streak
        u64 slot_seen{};  ///< Nx2ListWriterPosition at the last confirmation
        u8 streak{};      ///< Synced reads in a row with the slot unchanged
        bool trusted{};
        bool synced_once{};
        bool banned{};    ///< Seen written by the GPU after a synced read: never trusted
    };

    [[nodiscard]] bool Trusted(u64 page) const {
        const auto it = pages.find(page);
        return it != pages.end() && it->second.trusted && !it->second.banned;
    }
    /// A read after the full (or L24) writer wait found the page Synced.
    /// Returns true when this confirmation promoted it.
    bool Confirm(u64 page, u64 slot) {
        Page& entry = pages[page];
        if (entry.banned || entry.trusted) {
            return false;
        }
        entry.synced_once = true;
        const auto now = std::chrono::steady_clock::now();
        if (entry.streak == 0 || entry.slot_seen != slot) {
            entry.streak = 0;
            entry.first = now;
        }
        entry.slot_seen = slot;
        if (entry.streak < Streak) {
            ++entry.streak;
        }
        if (entry.streak >= Streak && now - entry.first >= MinSpan) {
            entry.trusted = true;
            return true;
        }
        return false;
    }
    /// A read found the page Copied or Unknown: written by the GPU since its last sync.
    /// Returns true when the page was banned now.
    bool Written(u64 page, bool copied) {
        Page& entry = pages[page];
        if (entry.banned) {
            return false;
        }
        if (copied || entry.synced_once) {
            entry.banned = true;
            entry.trusted = false;
            return true;
        }
        entry.streak = 0;
        return false;
    }

private:
    std::unordered_map<u64, Page> pages;
};

/// Front-only counters for EDEN_NX2_LIST_STATS / EDEN_NX2_GPU_PROFILE and the L24/L25 check.
struct ListStats {
    // Page state before the wait (EDEN_NX2_LIST_STATS=1 only), by BufferCache::Nx2PeekListPage.
    static constexpr std::array<const char*, 5> StateNames{"no entry", "synced", "copy current",
                                                           "copy stale", "draw-written"};
    // Why a fetch waited: a writer of one of its pages was not yet run by the sequencer, run but
    // not yet committed, or only other writers were pending.
    static constexpr std::array<const char*, 4> ReasonNames{"page writer queued",
                                                            "page writer uncommitted",
                                                            "other writers", "first list of page"};
    static constexpr std::array<const char*, 3> ResultNames{"synced", "copied", "unknown"};
    // Tegra::Engines::EngineTypes order (Nv01Timer, KeplerCompute, Maxwell3D, Fermi2D,
    // MaxwellDMA, KeplerMemory), then none (no writer noted yet).
    static constexpr std::array<const char*, 7> KindNames{"timer", "compute", "3d", "2d",
                                                          "dma",   "i2m",     "none"};

    u64 fetches{};
    u64 waited{};
    u64 new_pages{};
    u64 multi_page{};
    std::array<u64, 5> state_before{};
    std::array<u64, 5> state_before_waited{};
    std::array<u64, 4> reasons{};
    std::array<u64, 3> results{};
    std::array<u64, 7> writer_kind_waited{};
    u64 l24_short{};
    u64 l25_skipped{};
    u64 l25_fallback{};
    u64 l25_promoted{};
    u64 l25_banned{};
    u64 check_compared{};
    u64 check_stale{};
    u64 check_unverifiable{};
    // EDEN_NX2_C10_LIST_CHECK (cycle 10 B2): fetches B2 shortened, read again after every writer.
    u64 c10_compared{};
    u64 c10_stale{};
    u64 c10_unverifiable{};
    // EDEN_NX2_C11_LISTRANGE_CHECK (cycle 11 F2): fetches F2 shortened, read again after every writer.
    u64 c11_compared{};
    u64 c11_stale{};
    u64 c11_unverifiable{};
    std::unordered_map<u64, u32> waited_pages;
    std::chrono::steady_clock::time_point window{std::chrono::steady_clock::now()};

    void NoteWaitedPage(u64 page) {
        if (waited_pages.size() < 4096 || waited_pages.contains(page)) {
            ++waited_pages[page];
        }
    }

    void MaybeLog(bool log) {
        const auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::seconds{10}) {
            return;
        }
        const double seconds =
            std::chrono::duration_cast<std::chrono::duration<double>>(now - window).count();
        window = now;
        if (log) {
            const auto per_second = [seconds](u64 value) {
                return static_cast<double>(value) / seconds;
            };
            std::string line = fmt::format(
                "GPU-written list fetches per second: {:.0f} ({:.0f} waited, {:.1f} new pages, "
                "{:.1f} multi-page) | waited because:",
                per_second(fetches), per_second(waited), per_second(new_pages),
                per_second(multi_page));
            for (size_t i = 0; i < reasons.size(); ++i) {
                line += fmt::format(" {} {:.0f},", ReasonNames[i], per_second(reasons[i]));
            }
            line += " | last writer kind (waited):";
            for (size_t i = 0; i < writer_kind_waited.size(); ++i) {
                if (writer_kind_waited[i] != 0) {
                    line += fmt::format(" {} {:.0f},", KindNames[i],
                                        per_second(writer_kind_waited[i]));
                }
            }
            line += " | results:";
            for (size_t i = 0; i < results.size(); ++i) {
                line += fmt::format(" {} {:.0f},", ResultNames[i], per_second(results[i]));
            }
            if (ListStatsFull()) {
                line += " | page state before (all/waited):";
                for (size_t i = 0; i < state_before.size(); ++i) {
                    line += fmt::format(" {} {:.0f}/{:.0f},", StateNames[i],
                                        per_second(state_before[i]),
                                        per_second(state_before_waited[i]));
                }
            }
            line += fmt::format(" | L24 short {:.0f}, L25 skipped {:.0f} (fallback {:.1f}, "
                                "promoted {}, banned {})",
                                per_second(l24_short), per_second(l25_skipped),
                                per_second(l25_fallback), l25_promoted, l25_banned);
            LOG_INFO(HW_GPU, "{}", line);
            if (ListStatsFull() && !waited_pages.empty()) {
                std::vector<std::pair<u64, u32>> top(waited_pages.begin(), waited_pages.end());
                const size_t count = std::min<size_t>(top.size(), 6);
                std::partial_sort(top.begin(), top.begin() + count, top.end(),
                                  [](const auto& a, const auto& b) { return a.second > b.second; });
                std::string pages_line;
                for (size_t i = 0; i < count; ++i) {
                    pages_line += fmt::format(" {:#x} ({}),", top[i].first << 12, top[i].second);
                }
                LOG_INFO(HW_GPU, "GPU-written list pages that waited most (10 s, {} pages):{}",
                         waited_pages.size(), pages_line);
            }
        }
        if (LiteListCheck()) {
            LOG_INFO(HW_GPU,
                     "List check: {} shortened fetches compared, {} stale, {} unverifiable; "
                     "L25 fallbacks {}",
                     check_compared, check_stale, check_unverifiable, l25_fallback);
        }
        if (C10ListCheck()) {
            LOG_INFO(HW_GPU,
                     "C10 list check: {} B2-shortened fetches compared, {} stale, {} "
                     "unverifiable",
                     c10_compared, c10_stale, c10_unverifiable);
        }
        c10_compared = c10_stale = c10_unverifiable = 0;
        if (C11ListRangeCheck()) {
            LOG_INFO(HW_GPU,
                     "C11 list range check: {} F2-shortened fetches compared, {} stale, {} "
                     "unverifiable",
                     c11_compared, c11_stale, c11_unverifiable);
        }
        c11_compared = c11_stale = c11_unverifiable = 0;
        fetches = waited = new_pages = multi_page = 0;
        state_before = {};
        state_before_waited = {};
        reasons = {};
        results = {};
        writer_kind_waited = {};
        l24_short = l25_skipped = l25_fallback = 0;
        check_compared = check_stale = check_unverifiable = 0;
        waited_pages.clear();
    }
};

} // namespace VideoCommon::MT
