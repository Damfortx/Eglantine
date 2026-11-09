// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/nx2.h"

/// MT lite round 6, W4 (docs/mt-lite-plan6.md, "W4 FRONT2"): a two-stage front (one thread
/// decoding GPFIFO entries, another executing puller/macro/3D methods) was studied and NOT built:
/// every parse decision that matters (GPU-written lists, GPU-param calls, semaphore acquires that
/// suspend the channel mid-list, dma_state carried through those) depends on the execution
/// thread, and the decode that does not is a few ns per header (the CPU lists' words are already
/// copied on the guest thread at submit, CommandList::submitted_words).
///
/// EDEN_NX2_MT_LITE_FRONT2 is reserved and stays OFF: there is no two-stage implementation, a '1'
/// only logs that once. EDEN_NX2_FRONT2_STATS=1 runs a read-only census instead: at
/// DispatchCalls' entry each newly queued CommandList's submitted words are decoded once (a dry
/// pass, no engine touched, nothing written outside DmaPusher::front2_census) and every 10 s the
/// channel logs what a parse thread would have had to hand over (records/s), how often it would
/// have had to stop for the execution thread (barriers/s, words between barriers) and what the
/// dry decode itself cost (the most a parse thread could take off the front).
namespace VideoCommon::MT {

namespace Front2Detail {
[[nodiscard]] inline bool StatsOn() {
    static const bool on = [] {
        if (Tegra::Nx2EnvMode("EDEN_NX2_MT_LITE_FRONT2") == '1') {
            LOG_WARNING(HW_GPU, "EDEN_NX2_MT_LITE_FRONT2=1: no two-stage front exists (see "
                                "docs/mt-lite-plan6.md, W4 FRONT2); ignored. "
                                "EDEN_NX2_FRONT2_STATS=1 measures the parse/execute split.");
        }
        return Tegra::Nx2EnvMode("EDEN_NX2_FRONT2_STATS") == '1';
    }();
    return on;
}
} // namespace Front2Detail

/// EDEN_NX2_MT_LITE_FRONT2: always false (reserved; see above).
[[nodiscard]] inline bool LiteFront2() {
    return false;
}

/// DmaPusher member (front2_census): the census' carried decode state and its counters.
struct Front2Census {
    // Bookkeeping: lists still queued when DispatchCalls last returned (already counted).
    size_t remaining_at_exit{};

    // Decode state carried across entries (as dma_state; parked split methods not modelled).
    u32 method{};
    u32 subchannel{};
    u32 count{};
    bool non_incrementing{};
    bool increment_once{};

    // Counters of the current window.
    u64 lists{};
    u64 prefetch_lists{};
    u64 entries{};
    u64 words{};
    u64 headers{};
    u64 records{};          ///< Data spans (one header's words within one entry) + inline headers
    u64 data_words{};
    u64 macro_words{};      ///< Data words to 3D methods >= 0xE00 (subchannel types at census)
    u64 puller_words{};
    u64 bind_words{};
    u64 semaphore_words{};  ///< Puller words that can suspend the channel (SEM_* trigger/execute)
    u64 gpu_entries{};      ///< Entries on GPU-written pages at census time
    u64 gpu_after_macro{};  ///< ... of those, while a 3D macro call's parameters were pending
    u64 no_snapshot_entries{}; ///< Entries without submitted words (read from memory on exec)
    u64 continued_entries{};   ///< Entries starting inside the previous entry's method
    u64 barriers{};
    u64 words_since_barrier{};
    std::array<u64, 8> barrier_gap{}; ///< Words between barriers: <16,<64,<256,<1k,<4k,<16k,<64k,more
    u64 census_ns{};
    std::chrono::steady_clock::time_point window{};
    bool window_started{};

    void Barrier() {
        ++barriers;
        size_t bucket = 0;
        for (u64 limit = 16; bucket < barrier_gap.size() - 1 && words_since_barrier >= limit;
             limit *= 4) {
            ++bucket;
        }
        ++barrier_gap[bucket];
        words_since_barrier = 0;
    }

    /// Unknown words (GPU-written / memory entries are decoded on the exec side): drop the carry.
    void LoseCarry() {
        count = 0;
        increment_once = false;
    }

    template <typename Pusher>
    void Decode(const Pusher& pusher, const u32* words_ptr, size_t size) {
        words += size;
        words_since_barrier += size;
        if (count != 0) {
            ++continued_entries;
        }
        for (size_t index = 0; index < size;) {
            if (count != 0) {
                if (method < 0x40) {
                    // Puller methods: one word at a time (as ProcessCommands under Ampere).
                    Puller(method);
                    if (!non_incrementing) {
                        ++method;
                    }
                    non_incrementing |= increment_once;
                    --count;
                    ++index;
                    ++records;
                    continue;
                }
                size_t take = std::min<size_t>(count, size - index);
                if (increment_once && !non_incrementing) {
                    take = 1;
                }
                if (method >= 0xE00 && subchannel < 8 &&
                    pusher.subchannels[subchannel] != nullptr &&
                    pusher.subchannel_type[subchannel] == Tegra::Engines::EngineTypes::Maxwell3D) {
                    macro_words += take;
                }
                ++records;
                data_words += take;
                if (!non_incrementing) {
                    method += static_cast<u32>(take);
                }
                non_incrementing |= increment_once;
                count -= static_cast<u32>(take);
                index += take;
                continue;
            }
            const u32 header = words_ptr[index++];
            ++headers;
            const u32 mode = header >> 29;
            const u32 header_method = header & 0x1fff;
            const u32 header_sub = (header >> 13) & 0x7;
            const u32 header_count = (header >> 16) & 0x1fff;
            switch (mode) {
            case 1: // Increasing
            case 3: // NonIncreasing
            case 5: // IncreaseOnce
                method = header_method;
                subchannel = header_sub;
                count = header_count;
                non_incrementing = mode == 3;
                increment_once = mode == 5;
                break;
            case 4: // Inline: one method, the argument in the header
                method = header_method;
                subchannel = header_sub;
                non_incrementing = true;
                increment_once = false;
                ++records;
                if (header_method < 0x40) {
                    Puller(header_method);
                }
                break;
            default:
                break;
            }
        }
    }

    void Puller(u32 puller_method) {
        ++puller_words;
        if (puller_method == 0x0) {
            ++bind_words;
        } else if (puller_method == 0x7 || puller_method == 0x1A || puller_method == 0x1B) {
            // SemaphoreOperation / Acquire / Release (SEM_EXECUTE on Ampere): may suspend.
            ++semaphore_words;
            Barrier();
        }
    }

    template <typename Pusher>
    void List(Pusher& pusher, const auto& list) {
        ++lists;
        if (!list.prefetch_command_list.empty()) {
            ++prefetch_lists;
            Decode(pusher, &list.prefetch_command_list.data()->argument,
                   list.prefetch_command_list.size());
            return;
        }
        const bool snapshot_ok = (list.snapshot_suspensions & 1) == 0;
        for (size_t i = 0; i < list.command_lists.size(); ++i) {
            const auto& entry = list.command_lists[i];
            const size_t size = entry.size;
            ++entries;
            if (size == 0) {
                continue;
            }
            const u64 address = entry.addr;
            if (pusher.IsGpuWrittenCommands(address, size * sizeof(u32))) {
                ++gpu_entries;
                if (count != 0 && method >= 0xE00) {
                    ++gpu_after_macro;
                }
                words += size;
                Barrier();
                LoseCarry();
                continue;
            }
            if (!snapshot_ok || i >= list.submitted_offsets.size() ||
                list.submitted_offsets[i] + size > list.submitted_words.size()) {
                ++no_snapshot_entries;
                words += size;
                Barrier();
                LoseCarry();
                continue;
            }
            Decode(pusher, &list.submitted_words[list.submitted_offsets[i]].argument, size);
        }
    }

    void MaybeLog(s32 channel) {
        const auto now = std::chrono::steady_clock::now();
        if (!window_started) {
            window_started = true;
            window = now;
            return;
        }
        const auto elapsed = now - window;
        if (elapsed < std::chrono::seconds{10}) {
            return;
        }
        const double seconds = std::chrono::duration<double>(elapsed).count();
        const auto per_s = [seconds](u64 value) { return static_cast<u64>(value / seconds); };
        LOG_INFO(HW_GPU,
                 "FRONT2 census ch {} per s: lists {} (prefetch {}), entries {}, words {}, "
                 "headers {}, records {}, data words {} (3D macro {}), puller words {} (bind {}, "
                 "semaphore {}), GPU-written entries {} ({} inside a macro call), no-snapshot "
                 "entries {}, continued entries {}, barriers {} (mean {} words apart), dry "
                 "decode {} us",
                 channel, per_s(lists), per_s(prefetch_lists), per_s(entries), per_s(words),
                 per_s(headers), per_s(records), per_s(data_words), per_s(macro_words),
                 per_s(puller_words), per_s(bind_words), per_s(semaphore_words),
                 per_s(gpu_entries), per_s(gpu_after_macro), per_s(no_snapshot_entries),
                 per_s(continued_entries), per_s(barriers),
                 barriers != 0 ? words / barriers : words, per_s(census_ns) / 1000);
        LOG_INFO(HW_GPU,
                 "FRONT2 census ch {} words between barriers (10 s): <16 {}, <64 {}, <256 {}, "
                 "<1k {}, <4k {}, <16k {}, <64k {}, more {}",
                 channel, barrier_gap[0], barrier_gap[1], barrier_gap[2], barrier_gap[3],
                 barrier_gap[4], barrier_gap[5], barrier_gap[6], barrier_gap[7]);
        lists = prefetch_lists = entries = words = headers = records = data_words = 0;
        macro_words = puller_words = bind_words = semaphore_words = 0;
        gpu_entries = gpu_after_macro = no_snapshot_entries = continued_entries = 0;
        barriers = census_ns = 0;
        barrier_gap = {};
        window = now;
    }
};

/// Returned by Front2Entry: records how many lists DispatchCalls left queued (already counted).
template <typename Pusher>
class Front2Guard {
public:
    explicit Front2Guard(Pusher* pusher_) : pusher{pusher_} {}
    ~Front2Guard() {
        if (pusher != nullptr) {
            pusher->front2_census.remaining_at_exit = pusher->dma_pushbuffer.size();
        }
    }
    Front2Guard(const Front2Guard&) = delete;
    Front2Guard& operator=(const Front2Guard&) = delete;

private:
    Pusher* pusher;
};

/// The single hook, first statement of DmaPusher::DispatchCalls. Off (EDEN_NX2_FRONT2_STATS
/// unset): one static bool test. On: decodes the lists queued since the last call (Push only
/// appends, only Step pops, both on this thread) and keeps nothing but counters.
template <typename Pusher>
[[nodiscard]] Front2Guard<Pusher> Front2Entry(Pusher& pusher) {
    if (!Front2Detail::StatsOn()) [[likely]] {
        return Front2Guard<Pusher>{nullptr};
    }
    auto& census = pusher.front2_census;
    const auto begin = std::chrono::steady_clock::now();
    const size_t queued = pusher.dma_pushbuffer.size();
    const size_t fresh = queued - std::min(queued, census.remaining_at_exit);
    for (size_t i = queued - fresh; i < queued; ++i) {
        census.List(pusher, pusher.dma_pushbuffer[i]);
    }
    census.census_ns += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - begin)
                                             .count());
    census.MaybeLog(pusher.channel_state.bind_id);
    return Front2Guard<Pusher>{&pusher};
}

} // namespace VideoCommon::MT
