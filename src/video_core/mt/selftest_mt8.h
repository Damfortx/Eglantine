// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Stress tests of the steps after phase 5 (docs/mt-pending-runtime-checks.md): T8 the resolver
// uniform ring (EDEN_NX2_MT_RBUF), T9 the uniform side arena of compact packets
// (EDEN_NX2_MT_COMPACT), T10 the replay op arena (EDEN_NX2_MT_REPLAY). Only mt/*.h, so they
// build outside CMake under TSan/ASan (mt/selftest_standalone.sh).

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/resolve_sync.h"
#include "video_core/mt/selftest_phase5.h"
#include "video_core/mt/uniform_ring.h"

namespace VideoCommon::MT::Selftest {

/// T8: a resolver streams uniform bytes into a small ring (wraps, reclaims, runs full) with
/// slot reuse; a committer waits for each batch, "records" its draws into command buffers and
/// stamps the batch with the buffer's tick; a GPU thread executes submitted buffers, checks every
/// allocation still holds its draw's bytes, then completes the tick. A region overwritten before
/// the GPU read it fails the check (and is a TSan race: no happens-before from the GPU's read to
/// the resolver's rewrite).
inline bool UniformRingStressTest() {
    constexpr u64 NumBatches = 20000;
    constexpr u32 MaxDraws = 6;
    constexpr size_t Capacity = 64 * 1024;
    constexpr u32 Alignment = 64;
    constexpr u32 Slots = 4;
    constexpr u64 Window = 64; // batches in flight between resolver and committer

    struct Draw {
        u64 offset;
        u32 size;
        u32 slot;
        u64 version;
        bool valid;
    };
    struct Batch {
        u32 count = 0;
        std::array<Draw, MaxDraws> draws{};
    };
    struct CommandBuffer {
        u64 tick;
        std::vector<Draw> draws;
    };
    const auto pattern = [](u32 slot, u64 version, u32 i) -> u8 {
        return static_cast<u8>(P5::Mix((u64{slot} << 40) ^ version ^ (u64{i} << 20)));
    };

    std::vector<u8> memory(Capacity);
    UniformRingCore core;
    core.Reset(Capacity, Alignment);
    UniformReuseMemo memo;
    SeqSlotRing<Batch, 128> batches;
    BatchCursor cursor;
    std::atomic<u64> committed{0};
    std::atomic<u64> gpu_tick{0};
    std::atomic<bool> stop{false};
    std::atomic<u32> failures{0};
    std::atomic<u32> finished{0};
    std::mutex queue_mutex;
    std::deque<CommandBuffer> queue;
    bool committer_done = false;
    u64 ring_full = 0;
    u64 reuses = 0;
    u64 allocations = 0;

    std::thread resolver([&] {
        std::mt19937_64 rng{8};
        std::array<u64, Slots> versions{};
        std::vector<u8> bytes;
        for (u64 b = 0; b < NumBatches; ++b) {
            while (b - committed.load(std::memory_order_acquire) >= Window) {
                if (stop.load()) {
                    finished.fetch_add(1);
                    return;
                }
                CpuPause();
            }
            Batch& batch = batches.Slot(b);
            batch.count = static_cast<u32>(1 + rng() % MaxDraws);
            core.BeginBatch(b);
            for (u32 d = 0; d < batch.count; ++d) {
                Draw& draw = batch.draws[d];
                draw.slot = static_cast<u32>(rng() % Slots);
                if (rng() % 3 == 0) {
                    ++versions[draw.slot];
                }
                draw.version = versions[draw.slot];
                draw.size = 16 * static_cast<u32>(1 + (P5::Mix(draw.slot ^ draw.version) % 64));
                bytes.resize(draw.size);
                for (u32 i = 0; i < draw.size; ++i) {
                    bytes[i] = pattern(draw.slot, draw.version, i);
                }
                std::optional<u64> offset =
                    memo.Find(0, draw.slot, b, 0, bytes.data(), draw.size);
                if (offset) {
                    ++reuses;
                } else {
                    offset = core.Allocate(draw.size, [&](u64 tick) {
                        return gpu_tick.load(std::memory_order_acquire) >= tick;
                    });
                    if (offset) {
                        ++allocations;
                        std::memcpy(memory.data() + *offset, bytes.data(), draw.size);
                        memo.Store(0, draw.slot, b, 0, *offset, bytes.data(), draw.size);
                    } else {
                        ++ring_full;
                    }
                }
                draw.valid = offset.has_value();
                draw.offset = offset.value_or(0);
            }
            core.EndBatch(b);
            cursor.Publish(b);
        }
        finished.fetch_add(1);
    });

    std::thread committer([&] {
        std::mt19937_64 rng{9};
        u64 tick = 1;
        CommandBuffer current{tick, {}};
        const auto submit = [&] {
            {
                std::scoped_lock lock{queue_mutex};
                queue.push_back(std::move(current));
            }
            ++tick;
            current = CommandBuffer{tick, {}};
        };
        for (u64 b = 0; b < NumBatches; ++b) {
            if (!cursor.Wait(b, [] {}, [&] { return stop.load(); })) {
                break;
            }
            const Batch& batch = batches.Slot(b);
            for (u32 d = 0; d < batch.count; ++d) {
                if (batch.draws[d].valid) {
                    current.draws.push_back(batch.draws[d]);
                }
            }
            core.Stamp(b, tick);
            committed.store(b + 1, std::memory_order_release);
            if (rng() % 5 == 0) {
                submit();
            }
        }
        submit();
        {
            std::scoped_lock lock{queue_mutex};
            committer_done = true;
        }
        finished.fetch_add(1);
    });

    std::thread gpu([&] {
        while (!stop.load()) {
            CommandBuffer buffer;
            bool have = false;
            bool done = false;
            {
                std::scoped_lock lock{queue_mutex};
                if (!queue.empty()) {
                    buffer = std::move(queue.front());
                    queue.pop_front();
                    have = true;
                } else {
                    done = committer_done;
                }
            }
            if (!have) {
                if (done) {
                    break;
                }
                std::this_thread::yield();
                continue;
            }
            for (const Draw& draw : buffer.draws) {
                for (u32 i = 0; i < draw.size; ++i) {
                    if (memory[draw.offset + i] != pattern(draw.slot, draw.version, i)) {
                        failures.fetch_add(1);
                        break;
                    }
                }
            }
            gpu_tick.store(buffer.tick, std::memory_order_release);
        }
        finished.fetch_add(1);
    });

    const bool in_time = P5::WaitFinished(finished, 3, std::chrono::seconds(60));
    if (!in_time) {
        stop.store(true);
        cursor.Wake();
    }
    resolver.join();
    committer.join();
    gpu.join();
    const bool ok = in_time && failures.load() == 0 && reuses > 0 && allocations > 0;
    if (!ok) {
        LOG_ERROR(HW_GPU, "T8: in_time={} failures={} reuses={} allocations={} ring_full={}",
                  in_time, failures.load(), reuses, allocations, ring_full);
    }
    return ok;
}

} // namespace VideoCommon::MT::Selftest
