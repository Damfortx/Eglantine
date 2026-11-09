// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/spmc_stream.h"

namespace VideoCommon::MT::Selftest {

namespace LazyWake {

[[nodiscard]] inline u64 Mix(u64 x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

/// Payload words of entry `seq` (header excluded): mostly small, now and then large.
[[nodiscard]] inline u32 PayloadWords(u64 seq, u32 max_payload) {
    const u64 h = Mix(seq * 3 + 1);
    const u32 limit = (h & 63) == 0 ? max_payload : (max_payload < 48 ? max_payload : 48);
    return static_cast<u32>((h >> 8) % (limit + 1));
}

[[nodiscard]] inline u64 PayloadWord(u64 seq, u32 index) {
    return Mix(seq ^ (u64{index} << 40) ^ 0x5bd1e995ULL);
}

/// One run: a producer and 3 consumers (fast, slow, often asleep) over a ring of
/// `capacity_words`. Returns the number of failures; aborts the process on a hang.
inline u32 Run(bool lazy, size_t capacity_words, u32 num_entries, u32 max_payload, u64 seed) {
    enum Kind : u8 { Data = 1, End = 2 };
    constexpr u32 NumConsumers = 3;
    SpmcStream stream(capacity_words);
    stream.SetLazyWake(lazy);
    std::array<u32, NumConsumers> ids{};
    for (u32& id : ids) {
        id = stream.AddConsumer();
    }
    std::atomic<u32> failures{0};
    std::atomic<bool> done{false};
    std::atomic<bool> timed_out{false};
    std::atomic<u32> producer_phase{0}; // for the hang report
    const auto stop = [&] { return timed_out.load(std::memory_order_relaxed); };

    const auto consume = [&](u32 index) {
        const u32 consumer = ids[index];
        std::mt19937_64 rng{seed * 7 + index};
        u64 expected = 0;
        u64 sink = 0;
        for (;;) {
            const u64* entry = stream.Peek(consumer);
            if (entry == nullptr) {
                if (stop()) {
                    return;
                }
                stream.WaitForData(consumer, stop);
                continue;
            }
            const u64 header = entry[0];
            const u8 kind = SpmcStream::HeaderKind(header);
            if (kind == End) {
                if (expected != num_entries) {
                    failures.fetch_add(1);
                    LOG_ERROR(HW_GPU, "T11: consumer {} saw End after {} of {} entries", index,
                              expected, num_entries);
                }
                stream.Advance(consumer, entry);
                return;
            }
            const u32 payload = PayloadWords(expected, max_payload);
            bool ok = kind == Data && SpmcStream::HeaderAux(header) == static_cast<u32>(expected) &&
                      SpmcStream::HeaderWords(header) == payload + 1;
            for (u32 i = 0; ok && i < payload; ++i) {
                ok = entry[i + 1] == PayloadWord(expected, i);
            }
            if (!ok) {
                if (failures.fetch_add(1) < 4) {
                    LOG_ERROR(HW_GPU, "T11: consumer {} entry {} bad (kind={} aux={} words={})",
                              index, expected, kind, SpmcStream::HeaderAux(header),
                              SpmcStream::HeaderWords(header));
                }
                // Resynchronizing is hopeless; stop checking but keep draining.
                expected = SpmcStream::HeaderAux(header);
            }
            if (index == 1) {
                // Slow: extra work per entry, a yield now and then.
                for (u32 r = 0; r < 4; ++r) {
                    for (u32 i = 0; i < payload; ++i) {
                        sink += Mix(entry[i + 1] + r);
                    }
                }
                if (rng() % 64 == 0) {
                    std::this_thread::yield();
                }
            } else if (index == 2 && rng() % 256 == 0) {
                // Often asleep: falls behind, then catches up and sleeps in WaitForData.
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 400));
            }
            ++expected;
            stream.Advance(consumer, entry);
        }
        (void)sink;
    };

    std::array<std::thread, NumConsumers> consumers;
    for (u32 i = 0; i < NumConsumers; ++i) {
        consumers[i] = std::thread(consume, i);
    }

    std::thread watchdog([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!done.load()) {
            if (std::chrono::steady_clock::now() > deadline) {
                timed_out.store(true);
                std::fprintf(stderr,
                             "T11: HANG (lazy=%d cap=%zu) producer_phase=%u published=%llu "
                             "positions=%llu,%llu,%llu\n",
                             lazy ? 1 : 0, capacity_words, producer_phase.load(),
                             static_cast<unsigned long long>(stream.PublishedPosition()),
                             static_cast<unsigned long long>(stream.ConsumerPosition(ids[0])),
                             static_cast<unsigned long long>(stream.ConsumerPosition(ids[1])),
                             static_cast<unsigned long long>(stream.ConsumerPosition(ids[2])));
                std::fflush(stderr);
                // The producer may be stuck in Reserve, which cannot be interrupted.
                std::_Exit(3);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Producer.
    std::mt19937_64 rng{seed};
    u32 no_flush_left = 0;
    u32 waits = 0;
    u32 liveness_checks = 0;
    const auto spin_until = [&](auto&& reached) {
        for (u32 spins = 0; !reached(); ++spins) {
            if (stop()) {
                return;
            }
            if (spins > 64) {
                std::this_thread::yield();
            }
        }
    };
    for (u64 seq = 0; seq < num_entries; ++seq) {
        producer_phase.store(1, std::memory_order_relaxed);
        const u32 payload = PayloadWords(seq, max_payload);
        u64* const out = stream.Reserve(payload + 1);
        out[0] = SpmcStream::MakeHeader(Data, payload + 1, static_cast<u32>(seq));
        for (u32 i = 0; i < payload; ++i) {
            out[i + 1] = PayloadWord(seq, i);
        }
        stream.Commit();
        producer_phase.store(2, std::memory_order_relaxed);

        if (no_flush_left > 0) {
            // A long stretch without explicit flushes: only the automatic ones (and ring full).
            if (--no_flush_left == 0 && rng() % 2 == 0) {
                // Liveness of the automatic flush: everything up to the last flush must reach
                // every consumer without a further flush.
                producer_phase.store(3, std::memory_order_relaxed);
                const u64 target = stream.FlushedPosition();
                if (stream.WritePosition() - target >= SpmcStream::LazyFlushWords) {
                    failures.fetch_add(1);
                    LOG_ERROR(HW_GPU, "T11: {} words unflushed", stream.WritePosition() - target);
                }
                spin_until([&] {
                    for (const u32 id : ids) {
                        if (stream.ConsumerPosition(id) < target) {
                            return false;
                        }
                    }
                    return true;
                });
                ++liveness_checks;
            }
            continue;
        }
        const u64 roll = rng() % 1024;
        if (roll == 0) {
            no_flush_left = 500 + static_cast<u32>(rng() % 5000);
        } else if (roll < 3) {
            // WaitCommitted-like: flush, then wait for one consumer to pass this entry.
            producer_phase.store(4, std::memory_order_relaxed);
            stream.Flush();
            const u32 id = ids[rng() % NumConsumers];
            const u64 target = stream.WritePosition();
            spin_until([&] { return stream.ConsumerPosition(id) >= target; });
            ++waits;
        } else if (roll < 6) {
            // Producer goes idle for a moment (after a flush, as the real one does).
            stream.Flush();
            std::this_thread::sleep_for(std::chrono::microseconds(rng() % 300));
        } else if (roll < 8) {
            // A pause without a flush: sleepers stay asleep, spinners proceed.
            std::this_thread::sleep_for(std::chrono::microseconds(rng() % 200));
        } else if (rng() % 4 == 0) {
            stream.Flush(); // batch end
        }
    }
    producer_phase.store(5, std::memory_order_relaxed);
    {
        u64* const out = stream.Reserve(1);
        out[0] = SpmcStream::MakeHeader(End, 1, 0);
        stream.Commit();
        stream.Flush();
    }
    for (std::thread& consumer : consumers) {
        consumer.join();
    }
    done.store(true);
    watchdog.join();
    if (timed_out.load()) {
        failures.fetch_add(1);
    }
    LOG_INFO(HW_GPU, "T11: lazy={} cap={} waits={} liveness_checks={} space_wait_ms={}", lazy,
             capacity_words, waits, liveness_checks, stream.space_wait_ns / 1000000);
    return failures.load();
}

} // namespace LazyWake

/// T11: lazy wake in SpmcStream. One producer, 3 consumers (fast, slow, often asleep), random
/// entry sizes; the producer flushes at random batch ends, sometimes not for long stretches
/// (only the automatic 32 KiB flush and the ring-full flush), waits for consumers after a flush,
/// pauses with and without one; a small ring forces the ring-full path. Every consumer must see
/// every entry in order with its payload, and nothing may hang (20 s watchdog per run). Also run
/// with lazy wake off.
inline bool LazyWakeStressTest() {
    u32 failures = 0;
    for (const bool lazy : {true, false}) {
        // Small ring (the auto flush threshold is the whole ring: ring-full path) and a large
        // one (automatic flushes inside no-flush stretches).
        failures += LazyWake::Run(lazy, size_t{1} << 12, 150000, 1024, lazy ? 11 : 12);
        failures += LazyWake::Run(lazy, size_t{1} << 16, 150000, 1024, lazy ? 13 : 14);
    }
    if (failures != 0) {
        LOG_ERROR(HW_GPU, "T11: failures={}", failures);
    }
    return failures == 0;
}

} // namespace VideoCommon::MT::Selftest
