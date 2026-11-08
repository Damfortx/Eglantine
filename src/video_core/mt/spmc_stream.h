// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <bit>
#include <memory>
#include <thread>

#include "common/assert.h"
#include "common/common_types.h"

namespace VideoCommon::MT {

/// Single producer, several consumers that each read every entry with their own cursor (a
/// broadcast ring, not a work queue). The producer reuses space only once the slowest
/// consumer has passed it. Entries are runs of u64 words that never wrap: header word
/// (kind bits 0-7, total words incl. header bits 8-31, aux bits 32-63) then payload.
class SpmcStream {
public:
    static constexpr u32 MaxConsumers = 8;
    static constexpr u8 PadKind = 0;
    static constexpr u32 MaxEntryWords = (1u << 24) - 1;

    explicit SpmcStream(size_t capacity_words)
        : capacity{std::bit_ceil(capacity_words)}, mask{capacity - 1},
          words{std::make_unique<u64[]>(capacity)} {
        for (auto& cursor : cursors) {
            cursor.position.store(0, std::memory_order_relaxed);
            cursor.active.store(false, std::memory_order_relaxed);
        }
    }

    SpmcStream(const SpmcStream&) = delete;
    SpmcStream& operator=(const SpmcStream&) = delete;

    [[nodiscard]] static constexpr u64 MakeHeader(u8 kind, u32 total_words, u32 aux) {
        return u64{kind} | (u64{total_words} << 8) | (u64{aux} << 32);
    }
    [[nodiscard]] static constexpr u8 HeaderKind(u64 header) {
        return static_cast<u8>(header);
    }
    [[nodiscard]] static constexpr u32 HeaderWords(u64 header) {
        return static_cast<u32>(header >> 8) & 0xffffff;
    }
    [[nodiscard]] static constexpr u32 HeaderAux(u64 header) {
        return static_cast<u32>(header >> 32);
    }

    /// Before the producer starts (or while it is quiescent): a consumer starting at the
    /// current write position.
    [[nodiscard]] u32 AddConsumer() {
        for (u32 i = 0; i < MaxConsumers; ++i) {
            if (!cursors[i].active.load(std::memory_order_relaxed)) {
                cursors[i].position.store(published.load(std::memory_order_acquire),
                                          std::memory_order_relaxed);
                cursors[i].active.store(true, std::memory_order_release);
                return i;
            }
        }
        ASSERT_MSG(false, "SpmcStream: too many consumers");
        return 0;
    }

    void RemoveConsumer(u32 consumer) {
        cursors[consumer].active.store(false, std::memory_order_release);
    }

    // ---- producer ----

    /// Room for an entry of `total_words` (header included); blocks while consumers lag a whole
    /// ring behind. Fill the words (header first word) then Commit.
    [[nodiscard]] u64* Reserve(u32 total_words) {
        ASSERT(total_words > 0 && total_words <= capacity / 2 && total_words <= MaxEntryWords);
        const size_t offset = static_cast<size_t>(write_pos & mask);
        if (offset + total_words > capacity) {
            // Fill the tail with a pad entry so entries stay contiguous.
            const u32 pad_words = static_cast<u32>(capacity - offset);
            WaitForSpace(pad_words);
            words[offset] = MakeHeader(PadKind, pad_words, 0);
            write_pos += pad_words;
        }
        WaitForSpace(total_words);
        reserved = total_words;
        return &words[write_pos & mask];
    }

    void Commit() {
        write_pos += reserved;
        reserved = 0;
        published.store(write_pos, std::memory_order_release);
        if (waiters.load(std::memory_order_seq_cst) != 0) {
            signal.fetch_add(1, std::memory_order_seq_cst);
            signal.notify_all();
        }
    }

    /// Convenience: one entry with a payload copy.
    void Push(u8 kind, u32 aux, const u64* payload, u32 payload_words) {
        u64* const out = Reserve(payload_words + 1);
        out[0] = MakeHeader(kind, payload_words + 1, aux);
        for (u32 i = 0; i < payload_words; ++i) {
            out[i + 1] = payload[i];
        }
        Commit();
    }

    [[nodiscard]] u64 WritePosition() const noexcept {
        return write_pos;
    }

    // ---- consumers ----

    /// The next entry for `consumer` (pads skipped), nullptr when it has read everything.
    [[nodiscard]] const u64* Peek(u32 consumer) {
        Cursor& cursor = cursors[consumer];
        const u64 end = published.load(std::memory_order_acquire);
        u64 position = cursor.position.load(std::memory_order_relaxed);
        while (position < end) {
            const u64* const entry = &words[position & mask];
            if (HeaderKind(entry[0]) != PadKind) {
                return entry;
            }
            position += HeaderWords(entry[0]);
            cursor.position.store(position, std::memory_order_release);
        }
        return nullptr;
    }

    /// Done with the entry Peek returned.
    void Advance(u32 consumer, const u64* entry) {
        Cursor& cursor = cursors[consumer];
        cursor.position.store(cursor.position.load(std::memory_order_relaxed) +
                                  HeaderWords(entry[0]),
                              std::memory_order_release);
    }

    /// Blocks until there is something past `consumer`'s cursor or `stop` returns true.
    template <typename StopFn>
    void WaitForData(u32 consumer, StopFn&& stop) {
        for (u32 spins = 0;; ++spins) {
            const u64 end = published.load(std::memory_order_acquire);
            if (cursors[consumer].position.load(std::memory_order_relaxed) < end || stop()) {
                return;
            }
            if (spins < 256) {
                continue;
            }
            const u32 observed = signal.load(std::memory_order_seq_cst);
            waiters.fetch_add(1, std::memory_order_seq_cst);
            if (published.load(std::memory_order_seq_cst) == end && !stop()) {
                signal.wait(observed, std::memory_order_seq_cst);
            }
            waiters.fetch_sub(1, std::memory_order_seq_cst);
        }
    }

    /// Wakes waiting consumers (e.g. after setting their stop condition).
    void WakeAll() {
        signal.fetch_add(1, std::memory_order_seq_cst);
        signal.notify_all();
    }

    [[nodiscard]] u64 ConsumerPosition(u32 consumer) const {
        return cursors[consumer].position.load(std::memory_order_acquire);
    }

private:
    [[nodiscard]] u64 SlowestCursor() const {
        u64 slowest = write_pos;
        for (const auto& cursor : cursors) {
            if (cursor.active.load(std::memory_order_acquire)) {
                const u64 position = cursor.position.load(std::memory_order_acquire);
                slowest = position < slowest ? position : slowest;
            }
        }
        return slowest;
    }

    void WaitForSpace(u32 total_words) {
        if (write_pos + total_words - reclaimed <= capacity) {
            return;
        }
        std::chrono::steady_clock::time_point begin{};
        for (u32 spins = 0;; ++spins) {
            reclaimed = SlowestCursor();
            if (write_pos + total_words - reclaimed <= capacity) {
                if (spins != 0) {
                    space_wait_ns += static_cast<u64>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - begin)
                            .count());
                }
                return;
            }
            if (spins == 0) {
                begin = std::chrono::steady_clock::now();
            }
            if (spins > 64) {
                std::this_thread::yield();
            }
        }
    }

    struct alignas(64) Cursor {
        std::atomic<u64> position;
        std::atomic<bool> active;
    };

    const size_t capacity;
    const size_t mask;
    std::unique_ptr<u64[]> words;

public:
    /// Producer-owned: time the producer waited for ring space (MT_STATS).
    u64 space_wait_ns = 0;

private:
    // Producer-owned.
    u64 write_pos = 0;
    u64 reclaimed = 0;
    u32 reserved = 0;

    alignas(64) std::atomic<u64> published{0};
    alignas(64) std::atomic<u32> waiters{0};
    std::atomic<u32> signal{0};
    std::array<Cursor, MaxConsumers> cursors;
};

} // namespace VideoCommon::MT
