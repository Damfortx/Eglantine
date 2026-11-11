// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/mt_switches.h"
#include "video_core/nx2.h"

/// Cycle 13, workstream W2 of docs/cycle13-plan.md: the native uniform bank re-upload
/// (EDEN_NX2_BANK_REUPLOAD, buffer_cache.h BindHostGraphicsUniformBuffer) under the
/// multithreaded back, and the MT_VERIFY regression it caused.
///
/// The merged path marks a 64 KiB bank CPU-modified once per frame from the bind. Under MT the
/// bind runs on the committer, whose SynchronizeBuffer is a no-op (the sequencer captured the
/// bytes), so the mark is never uploaded: the bank stays CPU-modified, VERIFY counts an
/// uncaptured sync on every later draw that syncs it, and every native UBO bind paid a
/// TrackerScope + IsRegionGpuModified before the per-frame set test.
///
/// B1, EDEN_NX2_C13_BANK:
///  - '1' (default): the per-frame set test and the size test come first, the tracker walk only
///    for a bank not yet seen this frame. Under MT (committer recording a captured packet) the
///    committer never marks: the sequencer, in its uniform capture (vk_mt_pipeline.h, hook in
///    docs/cycle13-hooks-W2.md), marks the bank once per frame as a guest write would
///    (MarkRegionAsCpuModified + NoteGuestWrite + BumpWriteGeneration) right before it takes the
///    bound range's uploads, so the bytes go into the packet and the committer applies them
///    like any captured upload (MtApplyUploads).
///  - '0': the merged path (committer marks under MT).
///  - 's': no re-upload under MT at all (the c11 behaviour); single-thread binds as '1'.
/// EDEN_NX2_C13_BANK_CHECK=1: VERIFY's uncaptured syncs by address (top 4) and how many hit a
///  bank; banks still CPU-modified when the committer binds them; rate-limited ERROR
///  "C13 bank check" for an uncaptured sync on a bank re-uploaded this frame.
///
/// B2, EDEN_NX2_C13_CLAIM (default '0', contingent: only if the B1 census shows uncaptured
/// syncs left elsewhere): the MT sequencer's cache invalidations claim the CPU writes first
/// (ClaimCpuWrites), as the single-thread OnCacheInvalidations does since upstream fe2cd8be2b.
/// EDEN_NX2_C13_CLAIM_CHECK=1: count of calls and ranges claimed.
///
/// EDEN_NX2_C13_STATS=1 (or any C13 W2 CHECK): one "C13 bank (10 s)" line.
namespace VideoCommon::MT::C13Bank {

/// EDEN_NX2_BANK_REUPLOAD (the merged switch): '0' off, '1' once per frame, '2' every bind of a
/// 64 KiB bank, '3' every bind of any native bank.
[[nodiscard]] inline char ReuploadMode() {
    static const char mode = [] {
        const char* v = std::getenv("EDEN_NX2_BANK_REUPLOAD");
        return v != nullptr ? v[0] : '1';
    }();
    return mode;
}

[[nodiscard]] inline bool CheckOn() {
    static const bool on = Tegra::Nx2EnvMode("EDEN_NX2_C13_BANK_CHECK") == '1';
    return on;
}

[[nodiscard]] inline bool ClaimCheckOn() {
    static const bool on = Tegra::Nx2EnvMode("EDEN_NX2_C13_CLAIM_CHECK") == '1';
    return on;
}

[[nodiscard]] inline bool StatsOn() {
    static const bool on =
        Tegra::Nx2EnvMode("EDEN_NX2_C13_STATS") == '1' || CheckOn() || ClaimCheckOn();
    return on;
}

namespace Detail {
[[nodiscard]] inline char ReadMode() {
    const char mode = Tegra::Nx2EnvMode("EDEN_NX2_C13_BANK");
    if (mode == '0' || mode == '1' || mode == 's') {
        return mode;
    }
    return '1';
}
[[nodiscard]] inline bool ReadClaim() {
    const char mode = Tegra::Nx2EnvMode("EDEN_NX2_C13_CLAIM");
    return Enabled() && mode == '1';
}
inline void LogStartup(char mode, bool claim) {
    LOG_INFO(HW_GPU,
             "C13 bank: mode {} (EDEN_NX2_C13_BANK), reupload {} (EDEN_NX2_BANK_REUPLOAD), "
             "MT {}, check {}, claim {} (check {}), stats {}",
             mode == '1' ? "sequencer" : (mode == 's' ? "skip under MT" : "merged path"),
             ReuploadMode(), Enabled() ? "on" : "off", CheckOn() ? "on" : "off",
             claim ? "on" : "off", ClaimCheckOn() ? "on" : "off", StatsOn() ? "on" : "off");
}
} // namespace Detail

/// '1', '0' or 's' (read once; logs the startup state).
[[nodiscard]] inline char Mode() {
    static const char mode = [] {
        const char m = Detail::ReadMode();
        Detail::LogStartup(m, Detail::ReadClaim());
        return m;
    }();
    return mode;
}

/// B2: the MT sequencer claims CPU writes before its invalidations' WriteMemory.
[[nodiscard]] inline bool ClaimOn() {
    static const bool on = [] {
        (void)Mode(); // startup line
        return Detail::ReadClaim();
    }();
    return on;
}

/// Counters (relaxed; only touched when StatsOn(), except the cheap per-bank ones).
struct Counters {
    std::atomic<u64> frames{};
    std::atomic<u64> seq_banks{};       ///< banks marked by the sequencer (captured uploads)
    std::atomic<u64> seq_bytes{};
    std::atomic<u64> seq_gpu_refused{}; ///< sequencer: bank GPU-modified, not marked
    std::atomic<u64> seq_tests{};       ///< sequencer: 64 KiB bank candidates seen
    std::atomic<u64> commit_banks{};    ///< banks marked by the committer (single thread / '0')
    std::atomic<u64> commit_bytes{};
    std::atomic<u64> commit_tests{};    ///< committer: native bank binds reaching the block
    std::atomic<u64> commit_set_skips{};///< committer: refused by the per-frame set (no walk)
    std::atomic<u64> commit_walks{};    ///< committer: TrackerScope + IsRegionGpuModified
    std::atomic<u64> commit_mt_skips{}; ///< committer: under MT, left to the sequencer / 's'
    std::atomic<u64> check_dirty_at_commit{}; ///< CHECK: bank still CPU-modified at the bind
    std::atomic<u64> uncaptured{};      ///< CHECK: VERIFY uncaptured syncs seen
    std::atomic<u64> uncaptured_bank{}; ///< CHECK: of which on a bank re-uploaded this frame
    std::atomic<u64> claim_calls{};
    std::atomic<u64> claim_ranges{};
};

[[nodiscard]] inline Counters& Stats() {
    static Counters counters;
    return counters;
}

/// The sequencer's per-frame set of banks already marked (sequencer thread only). The frame is
/// the committer's TickFrame epoch (g_hot_page_epoch), so a bank is re-uploaded at most once per
/// committer frame, as the single-thread path does.
class SeqFrameSet {
public:
    /// Whether the bank was already marked in frame `epoch`.
    [[nodiscard]] bool Contains(u64 addr, u32 epoch) {
        Roll(epoch);
        for (const u64 recent_addr : recent) {
            if (recent_addr == addr) {
                return true;
            }
        }
        return seen.contains(addr);
    }
    void Insert(u64 addr, u32 epoch) {
        Roll(epoch);
        seen.insert(addr);
        recent[next_recent] = addr;
        next_recent = (next_recent + 1) % recent.size();
    }

private:
    void Roll(u32 epoch) {
        if (epoch != frame) {
            frame = epoch;
            seen.clear();
            recent.fill(~u64{0});
        }
    }
    std::unordered_set<u64> seen;
    std::array<u64, 4> recent{~u64{0}, ~u64{0}, ~u64{0}, ~u64{0}};
    size_t next_recent = 0;
    u32 frame = ~u32{0};
};

/// CHECK census: VERIFY's uncaptured syncs by address (committer thread; logged from TickFrame).
class UncapturedCensus {
public:
    void Note(u64 addr, u32 size, bool on_bank) {
        Stats().uncaptured.fetch_add(1, std::memory_order_relaxed);
        if (on_bank) {
            Stats().uncaptured_bank.fetch_add(1, std::memory_order_relaxed);
        }
        std::scoped_lock lock{mutex};
        for (Entry& entry : entries) {
            if (entry.count != 0 && entry.addr == addr && entry.size == size) {
                ++entry.count;
                return;
            }
        }
        // Replace the least counted (a new address starts at 1).
        Entry* least = &entries[0];
        for (Entry& entry : entries) {
            if (entry.count < least->count) {
                least = &entry;
            }
        }
        *least = Entry{addr, size, 1};
    }
    /// "addr+size xN, ..." for the top 4, then cleared.
    [[nodiscard]] std::string TakeTop() {
        std::scoped_lock lock{mutex};
        std::array<Entry, NumEntries> sorted = entries;
        std::sort(sorted.begin(), sorted.end(),
                  [](const Entry& a, const Entry& b) { return a.count > b.count; });
        std::string text;
        for (size_t i = 0; i < 4 && sorted[i].count != 0; ++i) {
            text += fmt::format("{}{:#x}+{} x{}", i == 0 ? "" : ", ", sorted[i].addr,
                                sorted[i].size, sorted[i].count);
        }
        entries.fill(Entry{});
        return text.empty() ? std::string{"none"} : text;
    }

private:
    struct Entry {
        u64 addr = 0;
        u32 size = 0;
        u64 count = 0;
    };
    static constexpr size_t NumEntries = 16;
    std::mutex mutex;
    std::array<Entry, NumEntries> entries{};
};

[[nodiscard]] inline UncapturedCensus& Census() {
    static UncapturedCensus census;
    return census;
}

/// Banks re-uploaded in the current committer frame (for the CHECK's "on a bank" test; written by
/// either thread, read by the committer under CHECK only).
class RecentBanks {
public:
    void Note(u64 addr, u64 size) {
        const size_t slot = next.fetch_add(1, std::memory_order_relaxed) % banks.size();
        banks[slot].addr.store(addr, std::memory_order_relaxed);
        banks[slot].size.store(size, std::memory_order_relaxed);
    }
    [[nodiscard]] bool Overlaps(u64 addr, u64 size) const {
        for (const Bank& bank : banks) {
            const u64 bank_addr = bank.addr.load(std::memory_order_relaxed);
            const u64 bank_size = bank.size.load(std::memory_order_relaxed);
            if (bank_size != 0 && addr < bank_addr + bank_size && bank_addr < addr + size) {
                return true;
            }
        }
        return false;
    }

private:
    struct Bank {
        std::atomic<u64> addr{};
        std::atomic<u64> size{};
    };
    std::array<Bank, 8> banks{};
    std::atomic<size_t> next{};
};

[[nodiscard]] inline RecentBanks& Recent() {
    static RecentBanks recent;
    return recent;
}

/// Rate-limited ERROR for the CHECK (at most one per second).
inline void CheckError(u64 addr, u32 size) {
    static std::atomic<s64> last_ms{-1000000};
    const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 last = last_ms.load(std::memory_order_relaxed);
    if (now - last >= 1000 && last_ms.compare_exchange_strong(last, now)) {
        LOG_ERROR(HW_GPU, "C13 bank check: uncaptured sync on a re-uploaded bank {:#x}+{}", addr,
                  size);
    }
}

/// Called by SynchronizeBuffer's VERIFY branch for an uncaptured sync (committer).
inline void NoteUncapturedSync(u64 addr, u32 size) {
    if (!CheckOn()) {
        return;
    }
    const bool on_bank = Recent().Overlaps(addr, size);
    Census().Note(addr, size, on_bank);
    if (on_bank) {
        CheckError(addr, size);
    }
}

/// A bank marked for re-upload (either thread).
inline void NoteMarked(bool sequencer, u64 addr, u64 bytes) {
    Counters& c = Stats();
    if (sequencer) {
        c.seq_banks.fetch_add(1, std::memory_order_relaxed);
        c.seq_bytes.fetch_add(bytes, std::memory_order_relaxed);
    } else {
        c.commit_banks.fetch_add(1, std::memory_order_relaxed);
        c.commit_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    if (CheckOn()) {
        Recent().Note(addr, bytes);
    }
}

/// One "C13 bank (10 s)" line; called from BufferCache::TickFrame (committer).
inline void TickFrame() {
    if (!StatsOn()) {
        return;
    }
    Counters& c = Stats();
    c.frames.fetch_add(1, std::memory_order_relaxed);
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    const Clock::time_point now = Clock::now();
    if (now - last < std::chrono::seconds(10)) {
        return;
    }
    last = now;
    const auto take = [](std::atomic<u64>& counter) {
        return counter.exchange(0, std::memory_order_relaxed);
    };
    const u64 frames = take(c.frames);
    const u64 seq_banks = take(c.seq_banks);
    const u64 seq_bytes = take(c.seq_bytes);
    const u64 commit_banks = take(c.commit_banks);
    const u64 commit_bytes = take(c.commit_bytes);
    const u64 per_frame_x100 = frames != 0 ? (seq_banks + commit_banks) * 100 / frames : 0;
    const std::string top = CheckOn() ? Census().TakeTop() : std::string{"-"};
    LOG_INFO(HW_GPU,
             "C13 bank (10 s): mode {} reupload {}; frames {}; sequencer banks {} ({} MiB, "
             "tests {}, gpu-modified {}); committer banks {} ({} MiB, tests {}, set skips {}, "
             "tracker walks {}, left to MT {}); banks/frame {}.{:02}; check: dirty at commit {}, "
             "uncaptured syncs {} (on a bank {}), top {}; claim: calls {} ranges {}",
             Mode(), ReuploadMode(), frames, seq_banks, seq_bytes >> 20, take(c.seq_tests),
             take(c.seq_gpu_refused), commit_banks, commit_bytes >> 20, take(c.commit_tests),
             take(c.commit_set_skips), take(c.commit_walks), take(c.commit_mt_skips),
             per_frame_x100 / 100, per_frame_x100 % 100, take(c.check_dirty_at_commit),
             take(c.uncaptured), take(c.uncaptured_bank), top, take(c.claim_calls),
             take(c.claim_ranges));
}

} // namespace VideoCommon::MT::C13Bank
