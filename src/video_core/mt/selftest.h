// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "video_core/mt/arena.h"
#include "video_core/mt/atomic_flags.h"
#include "video_core/mt/concurrent_map.h"
#include "video_core/mt/engine_ptr.h"
#include "video_core/mt/epoch.h"
#include "video_core/mt/reg_delta.h"
#include "video_core/mt/selftest_phase5.h"
#include "video_core/mt/selftest_mt8.h"
#include "video_core/mt/selftest_mt8b.h"
#include "video_core/mt/spmc_stream.h"
#include "video_core/mt/stable_slot_vector.h"

/// EDEN_NX2_MT_SELFTEST: multithreaded stress of the MT primitives, run once at startup.
namespace VideoCommon::MT::Selftest {

inline bool StreamTest() {
    SpmcStream stream(4096);
    constexpr u32 NumConsumers = 3;
    constexpr u64 NumEntries = 400000;
    std::array<u32, NumConsumers> ids{};
    for (auto& id : ids) {
        id = stream.AddConsumer();
    }
    std::atomic<bool> done{false};
    std::atomic<u32> failures{0};
    std::vector<std::jthread> consumers;
    for (u32 c = 0; c < NumConsumers; ++c) {
        consumers.emplace_back([&, c] {
            u64 expected = 0;
            while (expected < NumEntries) {
                const u64* entry = stream.Peek(ids[c]);
                if (entry == nullptr) {
                    stream.WaitForData(ids[c], [&] { return done.load(); });
                    continue;
                }
                const u32 total = SpmcStream::HeaderWords(entry[0]);
                if (SpmcStream::HeaderAux(entry[0]) != static_cast<u32>(expected) ||
                    total != 1 + (expected % 37)) {
                    failures.fetch_add(1);
                    return;
                }
                for (u32 i = 1; i < total; ++i) {
                    if (entry[i] != expected * 1000 + i) {
                        failures.fetch_add(1);
                        return;
                    }
                }
                stream.Advance(ids[c], entry);
                ++expected;
            }
        });
    }
    std::array<u64, 64> payload{};
    for (u64 n = 0; n < NumEntries; ++n) {
        const u32 words = static_cast<u32>(n % 37);
        for (u32 i = 0; i < words; ++i) {
            payload[i] = n * 1000 + i + 1;
        }
        stream.Push(1, static_cast<u32>(n), payload.data(), words);
    }
    for (auto& consumer : consumers) {
        consumer.join();
    }
    done = true;
    return failures.load() == 0;
}

inline bool MapTest() {
    struct Value {
        u64 key_echo;
        u64 version;
    };
    EpochManager epoch;
    SingleWriterMap<u64, Value> map(8);
    map.SetEpochManager(&epoch);
    std::atomic<bool> stop{false};
    std::atomic<u32> failures{0};
    std::atomic<u64> hits{0};
    std::vector<std::jthread> readers;
    for (u32 r = 0; r < 3; ++r) {
        readers.emplace_back([&, r] {
            const int reader = epoch.RegisterReader();
            std::mt19937_64 rng{r};
            while (!stop.load(std::memory_order_relaxed)) {
                EpochGuard guard{epoch, reader};
                for (u32 i = 0; i < 64; ++i) {
                    const u64 key = rng() % 4096;
                    if (const auto value = map.Find(key)) {
                        hits.fetch_add(1, std::memory_order_relaxed);
                        if (value->key_echo != key * 7 + 1) {
                            failures.fetch_add(1);
                        }
                    }
                }
            }
            epoch.UnregisterReader(reader);
        });
    }
    std::mt19937_64 rng{99};
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds{400};
    u64 version = 0;
    while (std::chrono::steady_clock::now() < end) {
        for (u32 i = 0; i < 256; ++i) {
            const u64 key = rng() % 4096;
            if (rng() % 3 == 0) {
                map.Erase(key);
            } else {
                map.InsertOrAssign(key, Value{key * 7 + 1, ++version});
            }
        }
        if (rng() % 64 == 0) {
            map.Clear();
        }
        epoch.Collect();
    }
    stop = true;
    readers.clear();
    // Writer-side consistency after the run.
    map.ForEach([&](u64 key, const Value& value) {
        if (value.key_echo != key * 7 + 1) {
            failures.fetch_add(1);
        }
    });
    epoch.Collect();
    return failures.load() == 0 && hits.load() != 0 && epoch.PendingCount() == 0;
}

inline bool SlotVectorTest() {
    static constexpr u64 Alive = 0xA11CEA11CEull;
    static constexpr u64 Destroyed = 0xDEADDEADDEADull;
    struct Object {
        explicit Object(u64 tag_) : tag{tag_}, magic{Alive} {}
        ~Object() {
            magic = Destroyed;
        }
        Object(Object&&) = delete;
        u64 tag;
        volatile u64 magic;
    };
    EpochManager epoch;
    StableSlotVector<Object> slots;
    slots.SetEpochManager(&epoch);
    constexpr u32 NumPublished = 512;
    std::array<std::atomic<u32>, NumPublished> published{};
    for (auto& entry : published) {
        entry.store(Common::SlotId::INVALID_INDEX);
    }
    std::atomic<bool> stop{false};
    std::atomic<u32> failures{0};
    std::atomic<u64> checks{0};
    std::vector<std::jthread> readers;
    for (u32 r = 0; r < 3; ++r) {
        readers.emplace_back([&, r] {
            const int reader = epoch.RegisterReader();
            std::mt19937 rng{r + 7};
            while (!stop.load(std::memory_order_relaxed)) {
                EpochGuard guard{epoch, reader};
                for (u32 i = 0; i < 64; ++i) {
                    const u32 index = published[rng() % NumPublished].load(std::memory_order_acquire);
                    if (index == Common::SlotId::INVALID_INDEX) {
                        continue;
                    }
                    const Common::SlotId id{index};
                    const u32 generation = slots.Generation(id);
                    if ((generation & 1) == 0) {
                        continue;
                    }
                    // Erase defers the destructor past our guard: whatever the slot holds now
                    // must be a live object.
                    if (slots.UncheckedAt(id).magic != Alive) {
                        failures.fetch_add(1);
                    }
                    checks.fetch_add(1, std::memory_order_relaxed);
                }
            }
            epoch.UnregisterReader(reader);
        });
    }
    std::mt19937 rng{5};
    std::vector<Common::SlotId> live;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds{400};
    u64 tag = 0;
    while (std::chrono::steady_clock::now() < end) {
        for (u32 i = 0; i < 128; ++i) {
            if (live.size() < 3000 && (live.empty() || rng() % 2 == 0)) {
                const Common::SlotId id = slots.insert(++tag);
                live.push_back(id);
                published[rng() % NumPublished].store(id.index, std::memory_order_release);
            } else {
                const size_t pick = rng() % live.size();
                slots.erase(live[pick]);
                live[pick] = live.back();
                live.pop_back();
            }
        }
        epoch.Collect();
    }
    stop = true;
    readers.clear();
    size_t iterated = 0;
    for (auto [id, object] : slots) {
        iterated += object->magic == Alive ? 1 : 0;
    }
    return failures.load() == 0 && checks.load() != 0 && iterated == live.size() &&
           slots.size() == live.size();
}

inline bool FlagsTest() {
    enum class Bits : u32 {};
    AtomicEnumFlags<Bits> flags;
    std::vector<std::jthread> threads;
    for (u32 t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            const Bits bit = static_cast<Bits>(1u << t);
            const Bits keep = static_cast<Bits>(~(1u << t));
            for (u32 i = 0; i < 200000; ++i) {
                flags |= bit;
                flags &= keep;
            }
            flags |= bit;
        });
    }
    threads.clear();
    return static_cast<u32>(flags.Load()) == 0xf;
}

inline bool ArenaTest() {
    RingArena<u64> arena(1024);
    struct Span {
        u64* data;
        u32 count;
        u64 end;
        u64 tag;
    };
    std::array<Span, 256> queue{};
    std::atomic<u64> produced{0};
    std::atomic<u64> consumed{0};
    std::atomic<u32> failures{0};
    constexpr u64 Total = 200000;
    std::jthread consumer([&] {
        for (u64 n = 0; n < Total; ++n) {
            while (produced.load(std::memory_order_acquire) <= n) {
            }
            const Span span = queue[n % queue.size()];
            for (u32 i = 0; i < span.count; ++i) {
                if (span.data[i] != span.tag + i) {
                    failures.fetch_add(1);
                }
            }
            // Poison before release: a span handed out again too early shows up as a mismatch.
            for (u32 i = 0; i < span.count; ++i) {
                span.data[i] = ~u64{0};
            }
            arena.Release(span.end);
            consumed.store(n + 1, std::memory_order_release);
        }
    });
    for (u64 n = 0; n < Total; ++n) {
        while (n - consumed.load(std::memory_order_acquire) >= queue.size()) {
        }
        const u32 count = 1 + static_cast<u32>(n % 61);
        u64 end_position = 0;
        const auto span = arena.Allocate(count, end_position);
        for (u32 i = 0; i < count; ++i) {
            span[i] = n * 100 + i;
        }
        queue[n % queue.size()] = Span{span.data(), count, end_position, n * 100};
        produced.store(n + 1, std::memory_order_release);
    }
    consumer.join();
    return failures.load() == 0;
}

inline bool EnginePtrTest() {
    struct Engine {
        int value;
    };
    Engine real{1};
    Engine mirror{2};
    ThreadEnginePtr<Engine> ptr{&real};
    std::atomic<u32> failures{0};
    std::jthread other([&] {
        for (u32 i = 0; i < 100000; ++i) {
            if (ptr->value != 1) {
                failures.fetch_add(1);
            }
        }
    });
    {
        ScopedEngineOverride<Engine> scope{&real, &mirror};
        for (u32 i = 0; i < 100000; ++i) {
            if (ptr->value != 2) {
                failures.fetch_add(1);
            }
        }
    }
    other.join();
    return failures.load() == 0 && ptr->value == 1;
}

inline bool RegSetTest() {
    RegWriteSet set;
    std::array<u32, 4096> regs{};
    std::vector<bool> expected(4096);
    std::mt19937 rng{3};
    for (u32 i = 0; i < 500; ++i) {
        const u32 method = rng() % 0xE00;
        regs[method] = rng();
        set.Mark(method);
        expected[method] = true;
    }
    std::vector<u64> pairs(set.Count());
    const u32 count = set.Drain(regs.data(), 0xE00, pairs.data());
    u32 matched = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 method = static_cast<u32>(pairs[i]);
        if (!expected[method] || static_cast<u32>(pairs[i] >> 32) != regs[method]) {
            return false;
        }
        ++matched;
    }
    u32 total = 0;
    for (bool bit : expected) {
        total += bit ? 1 : 0;
    }
    return matched == total && !set.Any();
}

inline void Run() {
    struct Test {
        const char* name;
        bool (*fn)();
    };
    static constexpr std::array tests{
        Test{"spmc_stream", StreamTest}, Test{"single_writer_map+epoch", MapTest},
        Test{"stable_slot_vector+epoch", SlotVectorTest}, Test{"atomic_flags", FlagsTest},
        Test{"ring_arena", ArenaTest}, Test{"engine_ptr", EnginePtrTest},
        Test{"reg_write_set", RegSetTest},
        Test{"p5_t1_slot_ebr", SlotEbrStressTest},
        Test{"p5_t2_multi_writer_map", MultiWriterMapStressTest},
        Test{"p5_t3_buffer_model", BufferModelStressTest},
        Test{"p5_t4_arena_worker", ArenaWorkerStressTest},
        Test{"p5_t5_batch_publish", BatchPublishStressTest},
        Test{"p5_t6_variant_array", VariantArrayStressTest},
        Test{"p5_t7_barrier", BarrierStressTest},
        Test{"t8_uniform_ring", UniformRingStressTest},
        Test{"t9_packet_replica", PacketReplicaStressTest},
        Test{"t10_replay_arena", ReplayArenaStressTest},
    };
    bool all = true;
    for (const Test& test : tests) {
        const auto start = std::chrono::steady_clock::now();
        const bool ok = test.fn();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        all &= ok;
        LOG_INFO(HW_GPU, "MT selftest: {} {} ({} ms)", test.name, ok ? "PASS" : "FAIL", ms);
    }
    LOG_INFO(HW_GPU, "MT selftest: {}", all ? "PASS" : "FAIL");
}

} // namespace VideoCommon::MT::Selftest
