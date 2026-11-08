// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// T9 (compact packet replicas, EDEN_NX2_MT_COMPACT) and T10 (replay op blocks,
// EDEN_NX2_MT_REPLAY). Only mt/*.h, so they build outside CMake (mt/selftest_standalone.sh).

#include <algorithm>
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
#include "video_core/mt/packet_compact.h"
#include "video_core/mt/replay_arena.h"
#include "video_core/mt/selftest_phase5.h"
#include "video_core/mt/spmc_stream.h"

namespace VideoCommon::MT::Selftest {

/// T9: a sequencer sends packets through the SPMC stream with TIC/TSC-like 32-byte entries
/// (slot references when its table holds them unchanged, inserts otherwise) and streamed
/// uniform slots (unchanged, patched or full); a reader that decodes every packet and one that
/// decodes only every other packet (skipping the rest as resolvers do) must both see the bytes
/// the sequencer read, for every entry and uniform slot, at every packet they decode.
inline bool PacketReplicaStressTest() {
    constexpr u32 NumPackets = 30000;
    constexpr u32 NumAddresses = 96; // collide in the 4096-slot table now and then
    constexpr u32 UniformSlots = 4;
    enum Kind : u8 { Packet = 1, End = 2 };
    using Replica = DescriptorReplica;
    using View = CaptureView;

    // Guest memory model: an entry's bytes depend on (address, version).
    const auto entry_bytes = [](u64 gpu_addr, u64 version, u8* out) {
        for (u32 i = 0; i < Replica::EntryBytes; ++i) {
            out[i] = static_cast<u8>(P5::Mix(gpu_addr * 131 + version * 7 + i));
        }
    };
    const auto addr_of = [](u32 index) -> u64 {
        // Spread so some share a table slot.
        return 0x10000000ULL + u64{index} * 32 * (index % 3 == 0 ? 4096 : 1);
    };

    SpmcStream stream(1 << 16);
    std::atomic<bool> stop{false};
    std::atomic<u32> failures{0};
    std::atomic<u32> finished{0};
    std::array<u32, 2> readers{stream.AddConsumer(), stream.AddConsumer()};

    // Packet words: [0] n_wire, [1] n_refs, [2] n_inserts, [3] n_ubo, [4] checksum seed,
    // then wire OverlayEntry[], refs (u16, padded), inserts u64[], UboEntry[], blob.
    std::thread sequencer([&] {
        std::mt19937_64 rng{9};
        Replica table;
        std::array<u64, NumAddresses> versions{};
        std::vector<u8> kept(size_t{5} * View::UboMemoSlots * View::UboMemoBytes);
        std::vector<u64> words;
        std::vector<OverlayEntry> wire;
        std::vector<u16> refs;
        std::vector<u64> inserts;
        std::vector<UboEntry> ubos;
        std::vector<u8> blob;
        std::vector<u8> now(View::UboMemoBytes);
        for (u32 p = 0; p < NumPackets && !stop.load(); ++p) {
            wire.clear();
            refs.clear();
            inserts.clear();
            ubos.clear();
            blob.clear();
            // Distinct sorted addresses for this packet.
            std::array<bool, NumAddresses> used{};
            const u32 count = 1 + static_cast<u32>(rng() % 8);
            for (u32 i = 0; i < count; ++i) {
                used[rng() % NumAddresses] = true;
            }
            // In address order, as the overlay is.
            std::array<u32, NumAddresses> order{};
            for (u32 i = 0; i < NumAddresses; ++i) {
                order[i] = i;
            }
            std::sort(order.begin(), order.end(),
                      [&](u32 a, u32 b) { return addr_of(a) < addr_of(b); });
            for (const u32 index : order) {
                if (!used[index]) {
                    continue;
                }
                if (rng() % 10 == 0) {
                    ++versions[index]; // the guest changed the entry
                }
                const u64 gpu_addr = addr_of(index);
                std::array<u8, Replica::EntryBytes> bytes;
                entry_bytes(gpu_addr, versions[index], bytes.data());
                const u32 slot = Replica::SlotOf(gpu_addr);
                Replica::Slot& entry = table.slots[slot];
                if (entry.gpu_addr == gpu_addr &&
                    std::memcmp(entry.bytes.data(), bytes.data(), bytes.size()) == 0) {
                    refs.push_back(static_cast<u16>(slot));
                    continue;
                }
                inserts.push_back(gpu_addr);
                wire.push_back({gpu_addr, Replica::EntryBytes, static_cast<u32>(blob.size())});
                blob.insert(blob.end(), bytes.begin(), bytes.end());
            }
            for (const u64 gpu_addr : inserts) {
                Replica::Slot& entry = table.slots[Replica::SlotOf(gpu_addr)];
                entry.gpu_addr = gpu_addr;
                for (const OverlayEntry& w : wire) {
                    if (w.gpu_addr == gpu_addr) {
                        std::memcpy(entry.bytes.data(), blob.data() + w.offset, Replica::EntryBytes);
                    }
                }
            }
            // Refs must be sorted by the slot's address for Decode's merge.
            std::sort(refs.begin(), refs.end(), [&](u16 a, u16 b) {
                return table.slots[a].gpu_addr < table.slots[b].gpu_addr;
            });
            // Uniform slots: stage 0..1, index 0..1.
            for (u32 u = 0; u < UniformSlots; ++u) {
                if (rng() % 2 == 0) {
                    continue;
                }
                UboEntry entry{};
                entry.stage = static_cast<u8>(u / 2);
                entry.index = static_cast<u8>(u % 2);
                entry.size = 256 * (1 + u);
                u8* const k = kept.data() + (size_t{entry.stage} * View::UboMemoSlots +
                                             entry.index) * View::UboMemoBytes;
                std::memcpy(now.data(), k, entry.size);
                const u32 mode = static_cast<u32>(rng() % 3);
                if (mode == 1) {
                    now[rng() % entry.size] ^= 0x5a; // a few chunks
                } else if (mode == 2) {
                    for (u32 i = 0; i < entry.size; ++i) {
                        now[i] = static_cast<u8>(rng());
                    }
                }
                u32 changed = 0;
                const u64 mask = UniformPatchMask(now.data(), k, entry.size, changed);
                entry.offset = static_cast<u32>(blob.size());
                if (mask == 0) {
                    entry.staged = 2;
                } else if (sizeof(u64) + changed < entry.size) {
                    entry.staged = 3;
                    blob.resize(blob.size() + ((sizeof(u64) + changed + 7) & ~size_t{7}));
                    WriteUniformPatch(blob.data() + entry.offset, now.data(), entry.size, mask);
                } else {
                    entry.staged = 1;
                    entry.memo = 1;
                    blob.resize(blob.size() + ((entry.size + 7) & ~size_t{7}));
                    std::memcpy(blob.data() + entry.offset, now.data(), entry.size);
                }
                std::memcpy(k, now.data(), entry.size);
                ubos.push_back(entry);
            }
            blob.resize((blob.size() + 7) & ~size_t{7});
            // Expected checksum: every entry and every sent uniform slot after this packet.
            u64 sum = 0;
            for (const u32 index : order) {
                if (used[index]) {
                    std::array<u8, Replica::EntryBytes> bytes;
                    entry_bytes(addr_of(index), versions[index], bytes.data());
                    for (u8 b : bytes) {
                        sum = sum * 31 + b;
                    }
                }
            }
            for (const UboEntry& entry : ubos) {
                const u8* k = kept.data() + (size_t{entry.stage} * View::UboMemoSlots +
                                             entry.index) * View::UboMemoBytes;
                for (u32 i = 0; i < entry.size; ++i) {
                    sum = sum * 37 + k[i];
                }
            }
            words.assign(5, 0);
            words[0] = wire.size();
            words[1] = refs.size();
            words[2] = inserts.size();
            words[3] = ubos.size();
            words[4] = sum;
            const auto append = [&](const void* data, size_t bytes) {
                if (bytes == 0) {
                    return;
                }
                const size_t at = words.size();
                words.resize(at + (bytes + 7) / 8, 0);
                std::memcpy(words.data() + at, data, bytes);
            };
            append(wire.data(), wire.size() * sizeof(OverlayEntry));
            append(refs.data(), refs.size() * sizeof(u16));
            append(inserts.data(), inserts.size() * sizeof(u64));
            append(ubos.data(), ubos.size() * sizeof(UboEntry));
            append(blob.data(), blob.size());
            stream.Push(Packet, 0, words.data(), static_cast<u32>(words.size()));
        }
        stream.Push(End, 0, nullptr, 0);
        finished.fetch_add(1);
    });

    const auto reader = [&](u32 id, bool skip_odd) {
        PacketDecoder decoder;
        decoder.EnableUniforms();
        u32 index = 0;
        const auto stop_fn = [&] { return stop.load(); };
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
            if (kind == End) {
                stream.Advance(id, entry);
                break;
            }
            const u64* p = entry + 1;
            const size_t n_wire = p[0], n_refs = p[1], n_inserts = p[2], n_ubo = p[3];
            const u64 sum_expected = p[4];
            const u64* cursor = p + 5;
            const std::span<const OverlayEntry> wire{reinterpret_cast<const OverlayEntry*>(cursor),
                                                     n_wire};
            cursor += n_wire * sizeof(OverlayEntry) / 8;
            const std::span<const u16> refs{reinterpret_cast<const u16*>(cursor), n_refs};
            cursor += (n_refs * 2 + 7) / 8;
            const std::span<const u64> inserts{cursor, n_inserts};
            cursor += n_inserts;
            const std::span<const UboEntry> ubos{reinterpret_cast<const UboEntry*>(cursor), n_ubo};
            cursor += n_ubo * sizeof(UboEntry) / 8;
            const u8* const blob = reinterpret_cast<const u8*>(cursor);
            if (skip_odd && (index & 1) != 0) {
                decoder.ApplyInserts(wire, blob, inserts);
                decoder.ApplyUniforms(ubos, blob);
            } else {
                decoder.Decode(wire, blob, refs);
                decoder.ApplyInserts(wire, blob, inserts);
                decoder.ApplyUniforms(ubos, blob);
                const CaptureView view{.overlay = decoder.overlay, .ubos = {}, .blob = blob,
                                       .overlay_blob = decoder.bytes.data()};
                u64 sum = 0;
                for (const OverlayEntry& o : decoder.overlay) {
                    std::array<u8, Replica::EntryBytes> bytes;
                    if (!view.Read(o.gpu_addr, bytes.data(), bytes.size())) {
                        failures.fetch_add(1);
                    }
                    for (u8 b : bytes) {
                        sum = sum * 31 + b;
                    }
                }
                for (const UboEntry& ubo : ubos) {
                    u64 source = 0;
                    const u8* bytes = decoder.UniformBytes(ubo, blob, source);
                    if (bytes == nullptr) {
                        failures.fetch_add(1);
                        continue;
                    }
                    for (u32 i = 0; i < ubo.size; ++i) {
                        sum = sum * 37 + bytes[i];
                    }
                }
                if (sum != sum_expected) {
                    failures.fetch_add(1);
                }
            }
            ++index;
            stream.Advance(id, entry);
        }
        finished.fetch_add(1);
    };
    std::thread full_reader(reader, readers[0], false);
    std::thread skipping_reader(reader, readers[1], true);
    const bool in_time = P5::WaitFinished(finished, 3, std::chrono::seconds(60));
    if (!in_time) {
        stop.store(true);
        stream.WakeAll();
    }
    sequencer.join();
    full_reader.join();
    skipping_reader.join();
    const bool ok = in_time && failures.load() == 0;
    if (!ok) {
        LOG_ERROR(HW_GPU, "T9: in_time={} failures={}", in_time, failures.load());
    }
    return ok;
}

/// T10: a committer appends replay records (draws carrying the state they expect, elidable
/// state records, some repeated) and closes segments at random reasons, handing each block to a
/// worker queue; the worker runs blocks in order and releases them for reuse. Every draw must
/// see the state last set before it in recording order (elided records included), and every
/// record must run exactly once, in order.
inline bool ReplayArenaStressTest() {
    struct FakeCommandBuffer {
        struct Worker {
            u64 next_serial = 1;
            u64 state = 0;
            u64 draws = 0;
            u32 errors = 0;
        }* worker;
    };
    using Replay = BasicDrawReplay<FakeCommandBuffer>;
    constexpr u64 NumRecords = 400000;

    struct SetState {
        u64 serial;
        u64 value;
        void operator()(FakeCommandBuffer cmd) const {
            if (serial != 0) {
                cmd.worker->errors += cmd.worker->next_serial == serial ? 0 : 1;
                cmd.worker->next_serial = serial + 1;
            }
            cmd.worker->state = value;
        }
    };
    struct Draw {
        u64 serial;
        u64 expected;
        void operator()(FakeCommandBuffer cmd) const {
            cmd.worker->errors += cmd.worker->next_serial == serial ? 0 : 1;
            cmd.worker->next_serial = serial + 1;
            cmd.worker->errors += cmd.worker->state == expected ? 0 : 1;
            ++cmd.worker->draws;
        }
    };
    static_assert(Replay::Replayable<SetState> && Replay::Replayable<Draw>);

    Replay replay{2};
    std::mutex queue_mutex;
    std::deque<Replay::Block*> queue;
    bool done = false;
    std::atomic<u32> finished{0};
    u64 draws_sent = 0;

    std::thread committer([&] {
        std::mt19937_64 rng{10};
        u64 serial = 0;
        u64 state = 0;
        const auto close = [&](ReplayCloseReason reason) {
            if (Replay::Block* block = replay.Close(reason)) {
                std::scoped_lock lock{queue_mutex};
                queue.push_back(block);
            }
        };
        while (serial < NumRecords) {
            const u32 roll = static_cast<u32>(rng() % 100);
            if (roll < 40) {
                // A state record; often the same value again (elidable).
                const u64 value = rng() % 3 == 0 ? state + 1 : state;
                const SetState record{0, value};
                if (!replay.Elide(ReplayClass::Viewport, reinterpret_cast<const void*>(
                                                             &Replay::Invoke<SetState>),
                                  &record, sizeof(record))) {
                    if (!replay.Fits(sizeof(SetState), false)) {
                        close(ReplayCloseReason::Full);
                    }
                    ::new (replay.Append(&Replay::Invoke<SetState>, sizeof(record),
                                         ReplayClass::Viewport)) SetState{record};
                }
                state = value;
            } else if (roll < 90) {
                ++serial;
                if (!replay.Fits(sizeof(Draw), true)) {
                    close(ReplayCloseReason::Full);
                }
                ::new (replay.Append(&Replay::Invoke<Draw>, sizeof(Draw), ReplayClass::Draw))
                    Draw{serial, state};
                ++draws_sent;
            } else {
                // Another recording ends the segment: the worker state persists across
                // segments (one command buffer), the encoder's mirror does not.
                close(rng() % 2 == 0 ? ReplayCloseReason::Record : ReplayCloseReason::Dispatch);
            }
        }
        close(ReplayCloseReason::Idle);
        {
            std::scoped_lock lock{queue_mutex};
            done = true;
        }
        finished.fetch_add(1);
    });

    FakeCommandBuffer::Worker worker_state;
    std::thread worker([&] {
        while (true) {
            Replay::Block* block = nullptr;
            bool finished_queue = false;
            {
                std::scoped_lock lock{queue_mutex};
                if (!queue.empty()) {
                    block = queue.front();
                    queue.pop_front();
                } else {
                    finished_queue = done;
                }
            }
            if (block == nullptr) {
                if (finished_queue) {
                    break;
                }
                std::this_thread::yield();
                continue;
            }
            Replay::Run(*block, FakeCommandBuffer{&worker_state});
            replay.Release(block);
        }
        finished.fetch_add(1);
    });
    const bool in_time = P5::WaitFinished(finished, 2, std::chrono::seconds(60));
    committer.join();
    worker.join();
    const bool ok = in_time && worker_state.errors == 0 && worker_state.draws == draws_sent &&
                    replay.OrderErrors() == 0;
    if (!ok) {
        LOG_ERROR(HW_GPU, "T10: in_time={} errors={} draws={}/{} order_errors={} blocks={}",
                  in_time, worker_state.errors, worker_state.draws, draws_sent,
                  replay.OrderErrors(), replay.BlockCount());
    }
    return ok;
}

} // namespace VideoCommon::MT::Selftest
