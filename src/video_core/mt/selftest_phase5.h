// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Phase-5 stress tests T1-T7 (docs/mt-phase5-spec.md §14.2). Only mt/*.h and small fakes, so
// they also build outside CMake under TSan/ASan (mt/selftest_standalone.sh).

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/slot_vector.h"
#include "video_core/mt/arena.h"
#include "video_core/mt/concurrent_map.h"
#include "video_core/mt/epoch.h"
#include "video_core/mt/resolve_sync.h"
#include "video_core/mt/spmc_stream.h"
#include "video_core/mt/stable_slot_vector.h"

namespace VideoCommon::MT::Selftest {

namespace P5 {

using Clock = std::chrono::steady_clock;

constexpr u32 InvalidId = Common::SlotId::INVALID_INDEX;

[[nodiscard]] constexpr u64 Mix(u64 x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/// Polls `finished` until it reaches `count` or `timeout` passes (deadlock detection).
[[nodiscard]] inline bool WaitFinished(const std::atomic<u32>& finished, u32 count,
                                       std::chrono::milliseconds timeout) {
    const auto end = Clock::now() + timeout;
    while (finished.load(std::memory_order_acquire) < count) {
        if (Clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// ---- T1 ----

struct T1Obj {
    static constexpr u64 Magic = 0xC0FFEE0000000000ULL;
    static constexpr u64 MagicMask = 0xFFFFFF0000000000ULL;
    static constexpr u64 Poison = 0xDEADBEEFDEADBEEFULL;
    static constexpr u64 Dead = 0xDEADDEADDEADDEADULL;

    explicit T1Obj(u64 serial) : canary{Magic | serial}, heap{new u64{Mix(serial)}} {}
    T1Obj(T1Obj&& other) noexcept : canary{other.canary}, heap{other.heap} {
        other.canary = Poison;
        other.heap = nullptr;
    }
    T1Obj(const T1Obj&) = delete;
    T1Obj& operator=(const T1Obj&) = delete;
    T1Obj& operator=(T1Obj&&) = delete;
    ~T1Obj() {
        delete heap;
        heap = nullptr;
        canary = Dead;
    }

    /// Reader-side check of a live object: canary intact and the heap field readable.
    [[nodiscard]] static bool Valid(u64 canary_, const u64* heap_) noexcept {
        if ((canary_ & MagicMask) != Magic || heap_ == nullptr) {
            return false;
        }
        return *heap_ == Mix(canary_ & ~MagicMask);
    }

    u64 canary;
    u64* heap;
};

struct T1Record {
    u32 id;
    u32 gen;
    u64 canary;
};

// ---- T3 ----

struct FakeBuffer {
    u64 addr;
    u64 size;
};

// ---- T6 ----

/// Model of Sampler's variant array (vk_texture_cache.cpp, Sampler::Find/Emplace/HandleFor).
struct VariantModel {
    struct Key {
        u8 bits;    // reduce_anisotropy, force_nearest, drop_reduction, drop_custom_border, srgb
        u8 swizzle; // 0 = identity
        bool operator==(const Key&) const noexcept = default;
    };
    struct Variant {
        Key key;
        u64 handle;
    };
    static constexpr u32 MaxVariants = 32;
    static constexpr u32 Capacity = 64;
    static constexpr u8 SrgbBit = 1u << 4;

    [[nodiscard]] static u64 HandleOf(Key key) noexcept {
        return 0xAB00000000000000ULL | (u64{key.bits} << 8) | key.swizzle;
    }

    [[nodiscard]] u64 Find(Key key) const noexcept {
        const u32 count = num_variants.load(std::memory_order_acquire);
        for (u32 i = 0; i < count; ++i) {
            if (variants[i].key == key) {
                return variants[i].handle;
            }
        }
        return 0;
    }

    [[nodiscard]] Key Reduced(Key key) const noexcept {
        if (num_variants.load(std::memory_order_acquire) >= MaxVariants) {
            key.bits &= static_cast<u8>(~SrgbBit);
            key.swizzle = 0;
        }
        return key;
    }

    /// Caller holds `mutex`.
    u64 Emplace(Key key) {
        if (const u64 existing = Find(key); existing != 0) {
            return existing;
        }
        const u32 count = num_variants.load(std::memory_order_relaxed);
        if (count == Capacity) {
            overflowed = true;
            return variants[0].handle;
        }
        variants[count] = Variant{key, HandleOf(key)};
        num_variants.store(count + 1, std::memory_order_release);
        return variants[count].handle;
    }

    u64 HandleFor(Key key) {
        if (const u64 existing = Find(Reduced(key)); existing != 0) {
            return existing;
        }
        std::scoped_lock lock{mutex};
        return Emplace(Reduced(key));
    }

    std::array<Variant, Capacity> variants{};
    std::atomic<u32> num_variants{0};
    std::mutex mutex;
    bool overflowed = false; // under mutex
};

} // namespace P5

/// T1 slot+EBR: one writer inserts and erase_then()s (move-out into a writer-owned "sentenced
/// ring"), three readers follow the §7 protocol and validate their {id, gen} later.
inline bool SlotEbrStressTest() {
    using P5::T1Obj;
    using P5::T1Record;
    constexpr u32 TableSize = 64;
    constexpr u32 NumReaders = 3;
    constexpr size_t MaxRecords = 1u << 16;

    std::deque<T1Obj> sentenced; // writer-owned: only the writer Collects
    EpochManager epochs;
    epochs.SetAutoCollect(false);
    StableSlotVector<T1Obj> slots;
    slots.SetEpochManager(&epochs);
    if (slots.GetEpochManager() != &epochs) {
        return false;
    }

    std::array<std::atomic<u32>, TableSize> table;
    for (auto& entry : table) {
        entry.store(P5::InvalidId, std::memory_order_relaxed);
    }
    std::atomic<bool> stop{false};
    std::atomic<u32> failures{0};
    std::atomic<u64> reads{0};

    struct Truth {
        u32 gen;
        u64 serial;
    };
    std::vector<Truth> truth; // by slot index, writer-owned until joined
    std::array<std::vector<T1Record>, NumReaders> records;

    std::thread writer([&] {
        std::mt19937_64 rng{1};
        u64 serial = 0;
        std::array<u32, TableSize> mine;
        mine.fill(P5::InvalidId);
        u64 ops = 0;
        auto on_reclaim = [&sentenced](T1Obj&& object) {
            sentenced.push_back(std::move(object));
            while (sentenced.size() > 256) {
                sentenced.pop_front();
            }
        };
        while (!stop.load(std::memory_order_relaxed)) {
            const u32 k = static_cast<u32>(rng() % TableSize);
            if (mine[k] != P5::InvalidId) {
                const Common::SlotId id{mine[k]};
                table[k].store(P5::InvalidId, std::memory_order_release);
                mine[k] = P5::InvalidId;
                truth[id.index].gen = 0;
                slots.erase_then(id, on_reclaim);
            } else {
                const Common::SlotId id = slots.insert(++serial);
                if (truth.size() <= id.index) {
                    truth.resize(id.index + 1, Truth{0, 0});
                }
                truth[id.index] = Truth{slots.Generation(id), serial};
                mine[k] = id.index;
                table[k].store(id.index, std::memory_order_release);
            }
            if (++ops % 16 == 0) {
                epochs.Collect();
            }
            // Every reclaimed object went through the move: its source is poisoned.
            if (!sentenced.empty() && !T1Obj::Valid(sentenced.back().canary,
                                                    sentenced.back().heap)) {
                failures.fetch_add(1);
            }
        }
    });

    std::vector<std::thread> readers;
    for (u32 r = 0; r < NumReaders; ++r) {
        readers.emplace_back([&, r] {
            const int reader = epochs.RegisterReader();
            if (reader < 0) {
                failures.fetch_add(1);
                return;
            }
            std::mt19937_64 rng{100 + r};
            std::vector<T1Record>& mine = records[r];
            u64 local_reads = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                EpochGuard guard{epochs, reader};
                for (u32 j = 0; j < 8; ++j) {
                    const u32 raw = table[rng() % TableSize].load(std::memory_order_acquire);
                    if (raw == P5::InvalidId) {
                        continue;
                    }
                    const Common::SlotId id{raw};
                    const u32 gen = slots.Generation(id);
                    if ((gen & 1) == 0) {
                        continue;
                    }
                    const T1Obj& object = slots.UncheckedAt(id);
                    const u64 canary = object.canary;
                    const u64* const heap = object.heap;
                    if (!T1Obj::Valid(canary, heap)) {
                        failures.fetch_add(1);
                        continue;
                    }
                    ++local_reads;
                    if (slots.Generation(id) != gen) {
                        continue; // re-check: miss
                    }
                    if (mine.size() < MaxRecords) {
                        mine.push_back(T1Record{raw, gen, canary});
                    }
                }
                // "Commit" validation of older records, in this guard.
                for (u32 j = 0; j < 4 && !mine.empty(); ++j) {
                    const T1Record& rec = mine[rng() % mine.size()];
                    if (slots.IsCurrent(Common::SlotId{rec.id}, rec.gen)) {
                        const T1Obj& object = slots.UncheckedAt(Common::SlotId{rec.id});
                        if (object.canary != rec.canary || !T1Obj::Valid(object.canary, object.heap)) {
                            failures.fetch_add(1);
                        }
                    }
                }
            }
            reads.fetch_add(local_reads);
            epochs.UnregisterReader(reader);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    stop.store(true);
    writer.join();
    for (auto& thread : readers) {
        thread.join();
    }

    // Quiescent: every record validates iff the writer's truth says the object is still live.
    u64 stale = 0;
    for (const auto& list : records) {
        for (const T1Record& rec : list) {
            const Common::SlotId id{rec.id};
            const bool current = slots.IsCurrent(id, rec.gen);
            const bool expected = rec.id < truth.size() && truth[rec.id].gen == rec.gen;
            if (current != expected) {
                failures.fetch_add(1);
            }
            if (current && slots[id].canary != rec.canary) {
                failures.fetch_add(1);
            }
            stale += current ? 0 : 1;
        }
    }
    epochs.Collect();
    const size_t pending = epochs.PendingCount();
    const bool ok = failures.load() == 0 && pending == 0 && stale > 0 && reads.load() > 0;
    if (!ok) {
        LOG_ERROR(HW_GPU, "T1: failures={} pending={} stale={} reads={}", failures.load(),
                  pending, stale, reads.load());
    }
    return ok;
}

/// T2: SingleWriterMap with two writers serialized by a mutex (forced rebuilds and clears),
/// four readers inside guards.
inline bool MultiWriterMapStressTest() {
    constexpr u32 NumKeys = 256;
    EpochManager epochs;
    SingleWriterMap<u32, u64> map(16);
    map.SetEpochManager(&epochs);
    std::mutex writer_mutex;
    std::unordered_map<u32, u64> shadow; // under writer_mutex
    std::atomic<u32> issued{1};
    std::atomic<bool> stop{false};
    std::atomic<u32> failures{0};
    std::atomic<u64> hits{0};

    std::vector<std::thread> threads;
    for (u32 w = 0; w < 2; ++w) {
        threads.emplace_back([&, w] {
            std::mt19937_64 rng{10 + w};
            u64 ops = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const u32 key = static_cast<u32>(rng() % NumKeys);
                const u32 op = static_cast<u32>(rng() % 1000);
                std::scoped_lock lock{writer_mutex};
                if (op < 600) {
                    const u64 value = (u64{key} << 32) | issued.fetch_add(1);
                    map.InsertOrAssign(key, value);
                    shadow[key] = value;
                } else if (op < 998) {
                    map.Erase(key);
                    shadow.erase(key);
                } else {
                    map.Clear();
                    shadow.clear();
                }
                if (++ops % 32 == 0) {
                    epochs.Collect();
                }
            }
        });
    }
    for (u32 r = 0; r < 4; ++r) {
        threads.emplace_back([&, r] {
            const int reader = epochs.RegisterReader();
            if (reader < 0) {
                failures.fetch_add(1);
                return;
            }
            std::mt19937_64 rng{20 + r};
            u64 local_hits = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                EpochGuard guard{epochs, reader};
                for (u32 j = 0; j < 16; ++j) {
                    const u32 key = static_cast<u32>(rng() % NumKeys);
                    const std::optional<u64> value = map.Find(key);
                    if (!value) {
                        continue;
                    }
                    ++local_hits;
                    const u32 counter = static_cast<u32>(*value);
                    if ((*value >> 32) != key || counter == 0 ||
                        counter >= issued.load(std::memory_order_acquire)) {
                        failures.fetch_add(1);
                    }
                }
            }
            hits.fetch_add(local_hits);
            epochs.UnregisterReader(reader);
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    stop.store(true);
    for (auto& thread : threads) {
        thread.join();
    }
    for (u32 key = 0; key < NumKeys; ++key) {
        const std::optional<u64> value = map.Find(key);
        const auto it = shadow.find(key);
        if (value.has_value() != (it != shadow.end()) || (value && *value != it->second)) {
            failures.fetch_add(1);
        }
    }
    epochs.Collect();
    const size_t pending = epochs.PendingCount();
    const bool ok = failures.load() == 0 && pending == 0 && hits.load() > 0;
    if (!ok) {
        LOG_ERROR(HW_GPU, "T2: failures={} pending={} hits={}", failures.load(), pending,
                  hits.load());
    }
    return ok;
}

/// T3: buffer-cache model. Page table (atomic_ref), StableSlotVector<FakeBuffer>, writer
/// creates/joins/deletes and bumps buffer_epoch after each erase; readers run TryResolveRange
/// (§7.2) and validate against a mutex-protected shadow.
inline bool BufferModelStressTest() {
    using P5::FakeBuffer;
    constexpr u32 PageBits = 12;
    constexpr u32 NumPages = 512;
    constexpr u64 Space = u64{NumPages} << PageBits;

    EpochManager epochs;
    StableSlotVector<FakeBuffer> slots;
    slots.SetEpochManager(&epochs);
    std::vector<u32> page_table(NumPages, P5::InvalidId);
    std::atomic<u64> buffer_epoch{0};

    struct Shadow {
        u32 gen;
        u64 addr;
        u64 size;
    };
    std::mutex shadow_mutex;
    std::unordered_map<u32, Shadow> shadow; // under shadow_mutex
    std::atomic<bool> stop{false};
    std::atomic<u32> failures{0};
    std::atomic<u64> resolved{0}, invalidated{0}, joins{0}, fast_path{0};

    const auto load_page = [&](u32 page, std::memory_order order) {
        return std::atomic_ref<u32>{page_table[page]}.load(order);
    };
    const auto store_page = [&](u32 page, u32 id) {
        std::atomic_ref<u32>{page_table[page]}.store(id, std::memory_order_release);
    };

    std::thread writer([&] {
        std::mt19937_64 rng{3};
        const auto create = [&](u32 first, u32 count) {
            const Common::SlotId id =
                slots.insert(FakeBuffer{u64{first} << PageBits, u64{count} << PageBits});
            shadow[id.index] = Shadow{slots.Generation(id), u64{first} << PageBits,
                                      u64{count} << PageBits};
            for (u32 p = first; p < first + count; ++p) {
                store_page(p, id.index);
            }
            return id;
        };
        const auto random_buffer = [&]() -> u32 {
            for (u32 tries = 0; tries < 16; ++tries) {
                const u32 id = load_page(static_cast<u32>(rng() % NumPages),
                                         std::memory_order_relaxed);
                if (id != P5::InvalidId) {
                    return id;
                }
            }
            return P5::InvalidId;
        };
        while (!stop.load(std::memory_order_relaxed)) {
            const u32 op = static_cast<u32>(rng() % 100);
            if (op < 45) {
                const u32 first = static_cast<u32>(rng() % NumPages);
                const u32 count = std::min<u32>(1 + static_cast<u32>(rng() % 8), NumPages - first);
                bool empty = true;
                for (u32 p = first; p < first + count; ++p) {
                    empty &= load_page(p, std::memory_order_relaxed) == P5::InvalidId;
                }
                if (!empty) {
                    continue;
                }
                std::scoped_lock lock{shadow_mutex};
                create(first, count);
            } else if (op < 75) {
                const u32 a = random_buffer();
                if (a == P5::InvalidId) {
                    continue;
                }
                const FakeBuffer& buf_a = slots[Common::SlotId{a}];
                const u32 a_first = static_cast<u32>(buf_a.addr >> PageBits);
                const u32 a_end = static_cast<u32>((buf_a.addr + buf_a.size) >> PageBits);
                u32 b = P5::InvalidId;
                for (u32 p = a_end; p < std::min(a_end + 16, NumPages); ++p) {
                    b = load_page(p, std::memory_order_relaxed);
                    if (b != P5::InvalidId) {
                        break;
                    }
                }
                if (b == P5::InvalidId) {
                    continue;
                }
                const FakeBuffer& buf_b = slots[Common::SlotId{b}];
                const u32 b_end = static_cast<u32>((buf_b.addr + buf_b.size) >> PageBits);
                std::scoped_lock lock{shadow_mutex};
                const u32 gen_a = slots.Generation(Common::SlotId{a});
                const u32 gen_b = slots.Generation(Common::SlotId{b});
                create(a_first, b_end - a_first); // repoints every page, the gap included
                slots.erase(Common::SlotId{a});
                slots.erase(Common::SlotId{b});
                buffer_epoch.fetch_add(1, std::memory_order_release);
                shadow.erase(a);
                shadow.erase(b);
                if (slots.IsCurrent(Common::SlotId{a}, gen_a) ||
                    slots.IsCurrent(Common::SlotId{b}, gen_b)) {
                    failures.fetch_add(1); // a join must invalidate both
                }
                joins.fetch_add(1, std::memory_order_relaxed);
            } else {
                const u32 a = random_buffer();
                if (a == P5::InvalidId) {
                    continue;
                }
                std::scoped_lock lock{shadow_mutex};
                // The epoch bump must follow the page-table unmap: a reader whose epoch0
                // already includes the bump then sees the cleared pages and cannot resolve the
                // dead buffer (the commit fast path relies on it). The pause widens the window
                // so the wrong order shows up in the fast-path check.
                const FakeBuffer& buf = slots[Common::SlotId{a}];
                for (u64 p = buf.addr >> PageBits; p < (buf.addr + buf.size) >> PageBits; ++p) {
                    store_page(static_cast<u32>(p), P5::InvalidId);
                }
                for (u32 spin = 0; spin < 64; ++spin) {
                    CpuPause();
                }
                slots.erase(Common::SlotId{a});
                buffer_epoch.fetch_add(1, std::memory_order_release);
                shadow.erase(a);
            }
        }
    });

    struct Record {
        u32 id;
        u32 gen;
        u64 addr;
        u64 size;
        u64 epoch;
    };
    std::vector<std::thread> readers;
    for (u32 r = 0; r < 3; ++r) {
        readers.emplace_back([&, r] {
            const int reader = epochs.RegisterReader();
            if (reader < 0) {
                failures.fetch_add(1);
                return;
            }
            std::mt19937_64 rng{30 + r};
            std::vector<Record> batch;
            while (!stop.load(std::memory_order_relaxed)) {
                batch.clear();
                const u64 epoch0 = buffer_epoch.load(std::memory_order_acquire);
                {
                    EpochGuard guard{epochs, reader};
                    for (u32 j = 0; j < 8; ++j) {
                        const u64 size = 1 + rng() % 8192;
                        const u64 addr = rng() % (Space - size);
                        // TryResolveRange, §7.2.
                        const u32 page = static_cast<u32>(addr >> PageBits);
                        const u32 raw = load_page(page, std::memory_order_acquire);
                        if (raw == P5::InvalidId) {
                            continue;
                        }
                        const Common::SlotId id{raw};
                        const u32 gen = slots.Generation(id);
                        if ((gen & 1) == 0) {
                            continue;
                        }
                        const FakeBuffer& buf = slots.UncheckedAt(id);
                        const u64 buf_addr = buf.addr;
                        const u64 buf_size = buf.size;
                        if (buf_size == 0 || (buf_size & 0xfff) != 0 || (buf_addr & 0xfff) != 0 ||
                            buf_addr + buf_size > Space) {
                            failures.fetch_add(1); // torn or destroyed object
                            continue;
                        }
                        if (addr < buf_addr || addr + size > buf_addr + buf_size) {
                            continue; // IsInBounds miss
                        }
                        if (slots.Generation(id) != gen) {
                            continue;
                        }
                        batch.push_back(Record{raw, gen, addr, size, epoch0});
                    }
                }
                // Commit-time validation (I4) against the writer's truth.
                std::scoped_lock lock{shadow_mutex};
                const u64 epoch_now = buffer_epoch.load(std::memory_order_acquire);
                for (const Record& rec : batch) {
                    const bool current = slots.IsCurrent(Common::SlotId{rec.id}, rec.gen);
                    const auto it = shadow.find(rec.id);
                    const bool truth = it != shadow.end() && it->second.gen == rec.gen;
                    if (current != truth) {
                        failures.fetch_add(1);
                    }
                    if (current && (rec.addr < it->second.addr ||
                                    rec.addr + rec.size > it->second.addr + it->second.size)) {
                        failures.fetch_add(1); // validates but does not cover the range
                    }
                    if (epoch_now == rec.epoch) {
                        fast_path.fetch_add(1, std::memory_order_relaxed);
                        if (!current) {
                            failures.fetch_add(1); // epoch fast path would accept a dead buffer
                        }
                    }
                    (current ? resolved : invalidated).fetch_add(1, std::memory_order_relaxed);
                }
            }
            epochs.UnregisterReader(reader);
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    stop.store(true);
    writer.join();
    for (auto& thread : readers) {
        thread.join();
    }
    // Page table consistency: each live buffer's pages point to it.
    for (const auto& [id, buf] : shadow) {
        for (u64 p = buf.addr >> PageBits; p < (buf.addr + buf.size) >> PageBits; ++p) {
            if (page_table[p] != id) {
                failures.fetch_add(1);
            }
        }
    }
    epochs.Collect();
    const size_t pending = epochs.PendingCount();
    const bool ok = failures.load() == 0 && pending == 0 && resolved.load() > 0 &&
                    invalidated.load() > 0 && joins.load() > 0;
    if (!ok) {
        LOG_ERROR(HW_GPU, "T3: failures={} pending={} resolved={} invalidated={} joins={}",
                  failures.load(), pending, resolved.load(), invalidated.load(), joins.load());
    }
    return ok;
}

/// T4: resolver allocating from a tiny RingArena, committer recording release closures into a
/// chunk dispatched to a worker only when full or under pressure (§9.4).
inline bool ArenaWorkerStressTest() {
    constexpr u64 NumBatches = 50000;
    constexpr u32 AllocsPerBatch = 4;
    constexpr u32 ChunkLimit = 8;
    constexpr u64 Poison = 0xBADBADBADBADBADULL;

    struct Alloc {
        u64* ptr;
        u32 count;
        u64 end;
        u64 tag;
    };
    struct BatchRec {
        std::array<Alloc, AllocsPerBatch> allocs;
    };
    RingArena<u64> arena(512);
    SeqSlotRing<BatchRec, 64> batches;
    BatchCursor cursor;
    std::atomic<bool> starved{false};
    std::atomic<bool> stop{false};
    std::atomic<u64> committed_batches{0};
    std::atomic<u32> failures{0};
    std::atomic<u32> finished{0};
    std::atomic<u64> pressure_dispatches{0};
    std::atomic<u64> starvations{0};

    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<std::vector<std::function<void()>>> queue;
    bool committer_done = false; // under queue_mutex

    std::thread resolver([&] {
        std::mt19937_64 rng{4};
        for (u64 b = 0; b < NumBatches; ++b) {
            while (!SeqSlotRing<BatchRec, 64>::HasRoom(
                b, committed_batches.load(std::memory_order_acquire), 0)) {
                if (stop.load()) {
                    finished.fetch_add(1);
                    return;
                }
                std::this_thread::yield();
            }
            BatchRec& rec = batches.Slot(b);
            for (u32 i = 0; i < AllocsPerBatch; ++i) {
                const u32 count = 1 + static_cast<u32>(rng() % 32);
                u64 end = 0;
                std::span<u64> span;
                while ((span = arena.TryAllocate(count, end)).empty()) {
                    if (!starved.exchange(true, std::memory_order_seq_cst)) {
                        starvations.fetch_add(1, std::memory_order_relaxed);
                        cursor.Wake();
                    }
                    if (stop.load()) {
                        finished.fetch_add(1);
                        return;
                    }
                    std::this_thread::yield();
                }
                starved.store(false, std::memory_order_release);
                const u64 tag = b * AllocsPerBatch + i;
                for (u32 j = 0; j < count; ++j) {
                    span[j] = tag * 64 + j;
                }
                rec.allocs[i] = Alloc{span.data(), count, end, tag};
            }
            cursor.Publish(b);
        }
        finished.fetch_add(1);
    });

    std::thread worker([&] {
        while (true) {
            std::vector<std::function<void()>> chunk;
            {
                std::unique_lock lock{queue_mutex};
                queue_cv.wait(lock, [&] { return !queue.empty() || committer_done; });
                if (queue.empty()) {
                    break;
                }
                chunk = std::move(queue.front());
                queue.pop_front();
            }
            for (auto& closure : chunk) {
                closure();
            }
        }
        finished.fetch_add(1);
    });

    std::thread committer([&] {
        std::vector<std::function<void()>> chunk;
        const auto dispatch = [&] {
            if (chunk.empty()) {
                return;
            }
            {
                std::scoped_lock lock{queue_mutex};
                queue.push_back(std::move(chunk));
            }
            chunk.clear();
            queue_cv.notify_one();
        };
        const auto pressure_check = [&] {
            if (starved.load(std::memory_order_acquire)) {
                if (!chunk.empty()) {
                    pressure_dispatches.fetch_add(1, std::memory_order_relaxed);
                }
                dispatch();
            }
        };
        for (u64 b = 0; b < NumBatches; ++b) {
            if (!cursor.Wait(b, pressure_check, [&] { return stop.load(); })) {
                break;
            }
            const BatchRec rec = batches.Slot(b);
            committed_batches.store(b + 1, std::memory_order_release);
            for (const Alloc& alloc : rec.allocs) {
                chunk.push_back([alloc, &arena, &failures] {
                    for (u32 j = 0; j < alloc.count; ++j) {
                        if (alloc.ptr[j] != alloc.tag * 64 + j) {
                            failures.fetch_add(1);
                        }
                        alloc.ptr[j] = Poison;
                    }
                    arena.Release(alloc.end);
                });
            }
            if (chunk.size() >= ChunkLimit) {
                dispatch();
            }
        }
        dispatch();
        {
            std::scoped_lock lock{queue_mutex};
            committer_done = true;
        }
        queue_cv.notify_one();
        finished.fetch_add(1);
    });

    const bool in_time = P5::WaitFinished(finished, 3, std::chrono::seconds(30));
    if (!in_time) {
        stop.store(true);
        cursor.Wake();
        {
            std::scoped_lock lock{queue_mutex};
            committer_done = true;
        }
        queue_cv.notify_all();
    }
    resolver.join();
    committer.join();
    worker.join();
    const bool ok = in_time && failures.load() == 0 && arena.InFlight() == 0 &&
                    starvations.load() > 0;
    if (!ok) {
        LOG_ERROR(HW_GPU, "T4: in_time={} failures={} in_flight={} starvations={} pressure={}",
                  in_time, failures.load(), arena.InFlight(), starvations.load(),
                  pressure_dispatches.load());
    }
    return ok;
}

// GCC 13+ (seen on aarch64) reports a false -Wdangling-pointer for close_batch's captures.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
/// T5: SEQ → SpmcStream → committer + 2 resolvers; batches published with BatchCursor, slots in
/// a 64-entry SeqSlotRing with back-pressure; batches closed early at random serial points.
inline bool BatchPublishStressTest() {
    constexpr u64 NumDraws = 100000;
    constexpr u32 BatchSize = 4;
    constexpr u32 K = 2;
    enum Kind : u8 { Draw = 1, BatchEnd = 2, Serial = 3, End = 4 };
    struct Slot {
        u64 seq;
        u64 hash;
    };
    using Ring = SeqSlotRing<Slot, 64>;

    SpmcStream stream(1024);
    Ring ring;
    std::array<BatchCursor, K> cursors;
    std::atomic<u64> committed{0}; // draws committed (seq + 1 of the last one)
    std::atomic<bool> stop{false};
    std::atomic<u32> failures{0};
    std::atomic<u32> finished{0};
    const u32 committer_id = stream.AddConsumer();
    std::array<u32, K> resolver_ids;
    for (auto& id : resolver_ids) {
        id = stream.AddConsumer();
    }
    u64 short_batches = 0; // SEQ-owned until joined

    std::thread seq_thread([&] {
        std::mt19937_64 rng{5};
        u64 batch = 0;
        u32 batch_draws = 0;
        const auto close_batch = [&] {
            if (batch_draws == 0) {
                return;
            }
            const u64 payload[1] = {batch};
            stream.Push(BatchEnd, 0, payload, 1);
            short_batches += batch_draws < BatchSize ? 1 : 0;
            ++batch;
            batch_draws = 0;
        };
        for (u64 seq = 0; seq < NumDraws; ++seq) {
            while (!Ring::HasRoom(seq, committed.load(std::memory_order_acquire), 2 * BatchSize)) {
                if (stop.load()) {
                    finished.fetch_add(1);
                    return;
                }
                CpuPause();
            }
            const u64 payload[3] = {seq, batch, rng()};
            stream.Push(Draw, 0, payload, 3);
            if (++batch_draws == BatchSize) {
                close_batch();
            }
            if (rng() % 7 == 0) {
                close_batch();
                stream.Push(Serial, 0, nullptr, 0);
            }
        }
        close_batch();
        stream.Push(End, 0, nullptr, 0);
        finished.fetch_add(1);
    });

    const auto stop_fn = [&] { return stop.load(); };
    std::vector<std::thread> resolvers;
    for (u32 r = 0; r < K; ++r) {
        resolvers.emplace_back([&, r] {
            const u32 id = resolver_ids[r];
            while (true) {
                const u64* entry = stream.Peek(id);
                if (entry == nullptr) {
                    if (stop.load()) {
                        break;
                    }
                    stream.WaitForData(id, stop_fn);
                    continue;
                }
                const u8 kind = SpmcStream::HeaderKind(entry[0]);
                if (kind == Draw && entry[2] % K == r) {
                    Slot& slot = ring.Slot(entry[1]);
                    slot.seq = entry[1];
                    slot.hash = P5::Mix(entry[1] ^ entry[3]);
                } else if (kind == BatchEnd && entry[1] % K == r) {
                    cursors[r].Publish(entry[1]);
                }
                stream.Advance(id, entry);
                if (kind == End) {
                    break;
                }
            }
            finished.fetch_add(1);
        });
    }

    std::thread committer([&] {
        u64 expected_seq = 0;
        u64 expected_batch = 0;
        while (true) {
            const u64* entry = stream.Peek(committer_id);
            if (entry == nullptr) {
                if (stop.load()) {
                    break;
                }
                stream.WaitForData(committer_id, stop_fn);
                continue;
            }
            const u8 kind = SpmcStream::HeaderKind(entry[0]);
            if (kind == Draw) {
                const u64 seq = entry[1];
                const u64 batch = entry[2];
                if (seq != expected_seq || batch != expected_batch) {
                    failures.fetch_add(1);
                }
                if (!cursors[batch % K].Wait(batch, [] {}, stop_fn)) {
                    break;
                }
                const Slot& slot = ring.Slot(seq);
                if (slot.seq != seq || slot.hash != P5::Mix(seq ^ entry[3])) {
                    failures.fetch_add(1);
                }
                committed.store(seq + 1, std::memory_order_release);
                ++expected_seq;
            } else if (kind == BatchEnd) {
                if (entry[1] != expected_batch) {
                    failures.fetch_add(1);
                }
                ++expected_batch;
            }
            stream.Advance(committer_id, entry);
            if (kind == End) {
                if (expected_seq != NumDraws) {
                    failures.fetch_add(1);
                }
                break;
            }
        }
        finished.fetch_add(1);
    });

    const bool in_time = P5::WaitFinished(finished, 2 + K, std::chrono::seconds(30));
    if (!in_time) {
        stop.store(true);
        stream.WakeAll();
        for (auto& cursor : cursors) {
            cursor.Wake();
        }
    }
    seq_thread.join();
    committer.join();
    for (auto& thread : resolvers) {
        thread.join();
    }
    const bool ok = in_time && failures.load() == 0 && short_batches > 0;
    if (!ok) {
        LOG_ERROR(HW_GPU, "T5: in_time={} failures={} short_batches={}", in_time,
                  failures.load(), short_batches);
    }
    return ok;
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

/// T6: model of the sampler variant array: lock-free Find over [0, count), Emplace under a
/// mutex, keys reduced once 32 variants exist.
inline bool VariantArrayStressTest() {
    using Model = P5::VariantModel;
    constexpr u32 Rounds = 300;
    constexpr u32 NumThreads = 4;
    constexpr u32 LookupsPerRound = 64;
    std::vector<std::unique_ptr<Model>> models;
    for (u32 i = 0; i < Rounds; ++i) {
        models.push_back(std::make_unique<Model>());
    }
    std::atomic<u32> failures{0};
    std::barrier sync{NumThreads};
    std::vector<std::thread> threads;
    for (u32 t = 0; t < NumThreads; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937_64 rng{60 + t};
            for (u32 round = 0; round < Rounds; ++round) {
                Model& model = *models[round];
                u32 last_count = 0;
                for (u32 i = 0; i < LookupsPerRound; ++i) {
                    const Model::Key key{static_cast<u8>(rng() % 32), static_cast<u8>(rng() % 4)};
                    const u64 handle = model.HandleFor(key);
                    Model::Key reduced = key;
                    reduced.bits &= static_cast<u8>(~Model::SrgbBit);
                    reduced.swizzle = 0;
                    if (handle != Model::HandleOf(key) && handle != Model::HandleOf(reduced)) {
                        failures.fetch_add(1);
                    }
                    const u32 count = model.num_variants.load(std::memory_order_acquire);
                    if (count < last_count || count == 0) {
                        failures.fetch_add(1);
                    }
                    last_count = count;
                }
                sync.arrive_and_wait();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    u32 reached_reduction = 0;
    for (const auto& model : models) {
        const u32 count = model->num_variants.load();
        reached_reduction += count >= Model::MaxVariants ? 1 : 0;
        if (model->overflowed || count > Model::MaxVariants + 16) {
            failures.fetch_add(1);
        }
        for (u32 i = 0; i < count; ++i) {
            if (model->variants[i].handle != Model::HandleOf(model->variants[i].key)) {
                failures.fetch_add(1);
            }
            for (u32 j = i + 1; j < count; ++j) {
                if (model->variants[i].key == model->variants[j].key) {
                    failures.fetch_add(1); // duplicate key
                }
            }
        }
    }
    const bool ok = failures.load() == 0 && reached_reduction > 0;
    if (!ok) {
        LOG_ERROR(HW_GPU, "T6: failures={} reached_reduction={}", failures.load(),
                  reached_reduction);
    }
    return ok;
}

/// T7: channel-switch style barrier (§3.3): SEQ emits deltas and, every ~1000 packets, a barrier
/// with BarrierAcks; the committer runs the serial (mutating the real state while SEQ is
/// blocked), resolvers check their mirror against the real state, copy it and ack.
inline bool BarrierStressTest() {
    constexpr u32 K = 2;
    constexpr u32 NumBarriers = 50;
    constexpr u32 NumRegs = 16;
    enum Kind : u8 { Delta = 1, Barrier = 2, End = 3 };
    struct State {
        std::array<u64, NumRegs> regs{};
        u64 channel = 0;
        bool operator==(const State&) const noexcept = default;
    };

    State real{};
    SpmcStream stream(2048);
    std::atomic<bool> stop{false};
    std::atomic<u32> failures{0};
    std::atomic<u32> finished{0};
    std::atomic<u32> barriers_checked{0};
    const u32 committer_id = stream.AddConsumer();
    std::array<u32, K> resolver_ids;
    for (auto& id : resolver_ids) {
        id = stream.AddConsumer();
    }
    const auto stop_fn = [&] { return stop.load(); };

    std::thread seq_thread([&] {
        std::mt19937_64 rng{7};
        for (u32 barrier = 1; barrier <= NumBarriers; ++barrier) {
            const u32 packets = 900 + static_cast<u32>(rng() % 200);
            for (u32 i = 0; i < packets; ++i) {
                const u32 index = static_cast<u32>(rng() % NumRegs);
                const u64 value = rng();
                real.regs[index] = value; // outside barriers: SEQ only
                const u64 payload[2] = {index, value};
                stream.Push(Delta, 0, payload, 2);
            }
            auto acks = std::make_unique<BarrierAcks>();
            const u64 payload[2] = {reinterpret_cast<u64>(acks.get()), barrier};
            stream.Push(Barrier, 0, payload, 2);
            acks->WaitAll(K, [] { std::this_thread::yield(); });
            if (real.channel != barrier) {
                failures.fetch_add(1);
            }
        }
        stream.Push(End, 0, nullptr, 0);
        finished.fetch_add(1);
    });

    std::thread committer([&] {
        while (true) {
            const u64* entry = stream.Peek(committer_id);
            if (entry == nullptr) {
                if (stop.load()) {
                    break;
                }
                stream.WaitForData(committer_id, stop_fn);
                continue;
            }
            const u8 kind = SpmcStream::HeaderKind(entry[0]);
            if (kind == Barrier) {
                auto* const acks = reinterpret_cast<BarrierAcks*>(entry[1]);
                real.channel = entry[2]; // the serial (BindChannel) while SEQ is blocked
                acks->done.store(1, std::memory_order_release);
            }
            stream.Advance(committer_id, entry);
            if (kind == End) {
                break;
            }
        }
        finished.fetch_add(1);
    });

    std::vector<std::thread> resolvers;
    for (u32 r = 0; r < K; ++r) {
        resolvers.emplace_back([&, r] {
            const u32 id = resolver_ids[r];
            State mirror{};
            while (true) {
                const u64* entry = stream.Peek(id);
                if (entry == nullptr) {
                    if (stop.load()) {
                        break;
                    }
                    stream.WaitForData(id, stop_fn);
                    continue;
                }
                const u8 kind = SpmcStream::HeaderKind(entry[0]);
                if (kind == Delta) {
                    mirror.regs[entry[1]] = entry[2];
                } else if (kind == Barrier) {
                    auto* const acks = reinterpret_cast<BarrierAcks*>(entry[1]);
                    if (!acks->WaitDone(stop_fn)) {
                        break;
                    }
                    if (mirror.regs != real.regs || real.channel != entry[2]) {
                        failures.fetch_add(1);
                    }
                    mirror = real;
                    if (!(mirror == real)) {
                        failures.fetch_add(1);
                    }
                    barriers_checked.fetch_add(1);
                    acks->Ack();
                }
                stream.Advance(id, entry);
                if (kind == End) {
                    break;
                }
            }
            finished.fetch_add(1);
        });
    }

    const bool in_time = P5::WaitFinished(finished, 2 + K, std::chrono::seconds(30));
    if (!in_time) {
        // WaitAll has no stop: a hang here is reported by the watchdog of the caller.
        stop.store(true);
        stream.WakeAll();
    }
    seq_thread.join();
    committer.join();
    for (auto& thread : resolvers) {
        thread.join();
    }
    const bool ok = in_time && failures.load() == 0 && barriers_checked.load() == K * NumBarriers;
    if (!ok) {
        LOG_ERROR(HW_GPU, "T7: in_time={} failures={} barriers_checked={}", in_time,
                  failures.load(), barriers_checked.load());
    }
    return ok;
}

} // namespace VideoCommon::MT::Selftest
