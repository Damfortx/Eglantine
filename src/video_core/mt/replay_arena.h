// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

/// Phase 5b of docs/mt-phase5-spec.md (§12), EDEN_NX2_MT_REPLAY: the committer's per-draw
/// recording closures (ConfigureDrawCommon, dynamic states, vertex/index binds, the draw) are
/// appended as POD records {thunk, payload} to a block instead of each becoming a TypedCommand
/// in the scheduler's chunk; the block is replayed by ONE chunk closure on the worker, which then
/// gives it back. Any other recording, DispatchWork (and so Flush/Finish), the committer's idle
/// hook and a full block end the segment first, so the worker sees exactly the order the
/// committer recorded (vk_scheduler.h). Generic over the command buffer type so the standalone
/// stress tests (mt/selftest_mt8.h) run the same code. The pool never blocks: a block is reused
/// once the worker released it, else a new one is made.
namespace VideoCommon::MT {

/// What a replayed record sets, for elision. A record of an elidable class is dropped when the
/// last record of that class in the open segment had the same thunk and the same payload bytes
/// (re-running it would set the same values). Every replayable record that writes the Vulkan
/// state of a class must carry that class; classes may group several states (VertexBuffers also
/// covers SetVertexInputEXT, which rewrites the strides).
enum class ReplayClass : u8 {
    None,      ///< replayed, never elided
    Draw,      ///< draws (counted, never elided)
    Configure, ///< GraphicsPipeline::ConfigureDrawCommon closure (never elided)
    Viewport,
    Scissor,
    DepthBias,
    BlendConstants,
    DepthBounds,
    LineWidth,
    CullMode,
    FrontFace,
    DepthCompareOp,
    DepthTestEnable,
    DepthWriteEnable,
    Topology,
    IndexBuffer,
    VertexBuffers,
    // Byte mirrors only (decided inside the Configure closure):
    PushRescaling,
    PushRenderArea,
    DescriptorBufferOffset,
    Count,
};

enum class ReplayCloseReason : u8 { Record, Dispatch, Full, Idle, Count };

template <typename CommandBuffer>
class BasicDrawReplay {
public:
    using Thunk = void (*)(const u64* payload, CommandBuffer cmdbuf);

    static constexpr size_t BLOCK_BYTES = 64 * 1024;
    static constexpr size_t BLOCK_WORDS = BLOCK_BYTES / sizeof(u64);
    /// Largest payload: SetVertexInputEXT with 32 bindings and 32 attributes (2 x 1 KiB) + header.
    static constexpr size_t MAX_PAYLOAD_BYTES = 2304;
    static constexpr u32 MAX_DRAWS = 256;
    static constexpr size_t MIRROR_BYTES = MAX_PAYLOAD_BYTES;
    static constexpr u32 NUM_CLASSES = static_cast<u32>(ReplayClass::Count);
    static constexpr u32 FIRST_ELIDABLE = static_cast<u32>(ReplayClass::Viewport);

    /// Record header: thunk pointer, then payload size in words.
    static constexpr size_t HEADER_WORDS = 2;

    struct Block {
        std::array<u64, BLOCK_WORDS> words;
        size_t used = 0;
        u32 records = 0;
        u32 draws = 0;
        u64 serial = 0; ///< EDEN_NX2_MT_REPLAY=2: the segment's number (order check)
    };

    /// Lambdas that can become a POD record: copied bytewise, never destroyed.
    template <typename T>
    static constexpr bool Replayable =
        std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T> &&
        alignof(T) <= alignof(u64) && sizeof(T) <= MAX_PAYLOAD_BYTES;

    template <typename T>
    static void Invoke(const u64* payload, CommandBuffer cmdbuf) {
        (*std::launder(reinterpret_cast<const T*>(payload)))(cmdbuf);
    }

    static constexpr size_t PayloadWords(size_t bytes) {
        return (bytes + sizeof(u64) - 1) / sizeof(u64);
    }

    explicit BasicDrawReplay(u32 level_) : level{level_} {}

    BasicDrawReplay(const BasicDrawReplay&) = delete;
    BasicDrawReplay& operator=(const BasicDrawReplay&) = delete;

    /// 0 off, 1 replay, 2 replay plus the worker-side order check.
    [[nodiscard]] u32 Level() const noexcept {
        return level;
    }

    [[nodiscard]] bool IsOpen() const noexcept {
        return open;
    }

    /// Whether a record of `payload_bytes` fits in the open block (true when none is open:
    /// Append opens one).
    [[nodiscard]] bool Fits(size_t payload_bytes, bool is_draw) const noexcept {
        if (block == nullptr) {
            return true;
        }
        if (is_draw && block->draws >= MAX_DRAWS) {
            return false;
        }
        return block->used + HEADER_WORDS + PayloadWords(payload_bytes) <= BLOCK_WORDS;
    }

    /// Appends a record and returns its payload storage (8-byte aligned, last word zeroed).
    u64* Append(Thunk fn, size_t payload_bytes, ReplayClass cls) {
        if (block == nullptr) {
            block = AcquireBlock();
            block->used = 0;
            block->records = 0;
            block->draws = 0;
            block->serial = ++segment_serial;
        }
        open = true;
        const size_t words = PayloadWords(payload_bytes);
        u64* const header = block->words.data() + block->used;
        header[0] = static_cast<u64>(reinterpret_cast<uintptr_t>(fn));
        header[1] = static_cast<u64>(words);
        u64* const payload = header + HEADER_WORDS;
        if (words != 0) {
            payload[words - 1] = 0; // deterministic tail padding
        }
        block->used += HEADER_WORDS + words;
        ++block->records;
        Count(cls);
        return payload;
    }

    /// Elision check of an elidable class; updates the class mirror. True when the record
    /// repeats the last one of its class in this segment and can be dropped.
    bool Elide(ReplayClass cls, const void* key, const void* bytes, size_t size) {
        const u32 index = static_cast<u32>(cls);
        if (index < FIRST_ELIDABLE || index >= NUM_CLASSES || size > MIRROR_BYTES) {
            return false;
        }
        Mirror& mirror = mirrors[index];
        const u32 bit = 1U << index;
        if ((mirror_valid & bit) != 0 && mirror.key == key && mirror.size == size &&
            std::memcmp(mirror.bytes.data(), bytes, size) == 0) {
            ++stats.elided[index];
            open = true;
            return true;
        }
        mirror_valid |= bit;
        mirror.key = key;
        mirror.size = static_cast<u32>(size);
        std::memcpy(mirror.bytes.data(), bytes, size);
        return false;
    }

    /// A pipeline bind was recorded: pipeline-static state, push constants, descriptor offsets
    /// and vertex strides may have changed, so nothing set before it is known any more.
    void ResetMirror() noexcept {
        mirror_valid = 0;
    }

    void Invalidate(ReplayClass cls) noexcept {
        mirror_valid &= ~(1U << static_cast<u32>(cls));
    }

    /// Ends the open segment: the block to record (nullptr when nothing was appended).
    [[nodiscard]] Block* Close(ReplayCloseReason reason) {
        if (!open) {
            return nullptr;
        }
        open = false;
        mirror_valid = 0;
        ++stats.segments;
        ++stats.closed[static_cast<u32>(reason)];
        Block* const result = std::exchange(block, nullptr);
        MaybeLog();
        return result;
    }

    /// Worker: runs a block's records in order.
    static void Run(const Block& replay_block, CommandBuffer cmdbuf) {
        const u64* it = replay_block.words.data();
        const u64* const end = it + replay_block.used;
        while (it < end) {
            const Thunk fn = reinterpret_cast<Thunk>(static_cast<uintptr_t>(it[0]));
            const size_t words = static_cast<size_t>(it[1]);
            fn(it + HEADER_WORDS, cmdbuf);
            it += HEADER_WORDS + words;
        }
    }

    /// Worker: gives an executed block back. Level 2 checks that segments run in the order
    /// they were closed (none lost, none reordered).
    void Release(Block* replay_block) {
        std::scoped_lock lock{pool_mutex};
        if (level >= 2) {
            if (replay_block->serial != executed_serial + 1) {
                ++order_errors;
                if (order_errors <= 8) {
                    LOG_ERROR(Render_Vulkan, "MT replay: segment {} ran after segment {}",
                              replay_block->serial, executed_serial);
                }
            }
            executed_serial = replay_block->serial;
        }
        free_blocks.push_back(replay_block);
    }

    [[nodiscard]] u64 OrderErrors() {
        std::scoped_lock lock{pool_mutex};
        return order_errors;
    }

    /// Blocks made so far (all of them, free or in flight).
    [[nodiscard]] size_t BlockCount() {
        std::scoped_lock lock{pool_mutex};
        return all_blocks.size();
    }

private:
    struct Mirror {
        const void* key = nullptr;
        u32 size = 0;
        std::array<u8, MIRROR_BYTES> bytes{};
    };

    struct Stats {
        u64 segments = 0;
        std::array<u64, static_cast<size_t>(ReplayCloseReason::Count)> closed{};
        std::array<u64, NUM_CLASSES> records{};
        std::array<u64, NUM_CLASSES> elided{};
    };

    void Count(ReplayClass cls) {
        ++stats.records[static_cast<u32>(cls)];
        if (cls == ReplayClass::Draw && block != nullptr) {
            ++block->draws;
        }
    }

    Block* AcquireBlock() {
        std::scoped_lock lock{pool_mutex};
        if (!free_blocks.empty()) {
            Block* const result = free_blocks.back();
            free_blocks.pop_back();
            return result;
        }
        all_blocks.push_back(std::make_unique<Block>());
        return all_blocks.back().get();
    }

    static std::string Rate(const char* name, u64 value, double seconds) {
        char text[64];
        std::snprintf(text, sizeof(text), " %s=%.0f", name, static_cast<double>(value) / seconds);
        return text;
    }

    void MaybeLog() {
        const auto now = std::chrono::steady_clock::now();
        if (last_log == std::chrono::steady_clock::time_point{}) {
            last_log = now;
            return;
        }
        const auto elapsed = now - last_log;
        if (elapsed < std::chrono::seconds{10}) {
            return;
        }
        const double seconds = std::chrono::duration<double>(elapsed).count();
        static constexpr std::array<const char*, NUM_CLASSES> NAMES{
            "other", "draw",  "config", "vp",    "sc",    "dbias", "blend",
            "dbnd",  "lw",    "cull",   "front", "dcmp",  "dtest", "dwrite",
            "topo",  "ib",    "vb",     "pushR", "pushA", "dbOff",
        };
        u64 total_records = 0;
        std::string records;
        std::string elided;
        for (u32 i = 0; i < NUM_CLASSES; ++i) {
            total_records += stats.records[i];
            if (stats.records[i] != 0) {
                records += Rate(NAMES[i], stats.records[i], seconds);
            }
            if (stats.elided[i] != 0) {
                elided += Rate(NAMES[i], stats.elided[i], seconds);
            }
        }
        const u64 draws = stats.records[static_cast<u32>(ReplayClass::Draw)];
        LOG_INFO(Render_Vulkan,
                 "MT replay /s: segments={:.0f} (record={:.0f} dispatch={:.0f} full={:.0f} "
                 "idle={:.0f}) records={:.0f} draws/segment={:.1f} blocks={} | records:{} | "
                 "elided:{} | order errors {}",
                 static_cast<double>(stats.segments) / seconds,
                 static_cast<double>(stats.closed[0]) / seconds,
                 static_cast<double>(stats.closed[1]) / seconds,
                 static_cast<double>(stats.closed[2]) / seconds,
                 static_cast<double>(stats.closed[3]) / seconds,
                 static_cast<double>(total_records) / seconds,
                 stats.segments != 0 ? static_cast<double>(draws) / stats.segments : 0.0,
                 BlockCount(), records, elided, OrderErrors());
        stats = {};
        last_log = now;
    }

    const u32 level;
    bool open = false;
    Block* block = nullptr;
    u32 mirror_valid = 0;
    u64 segment_serial = 0;
    std::array<Mirror, NUM_CLASSES> mirrors{};
    Stats stats{};
    std::chrono::steady_clock::time_point last_log{};

    std::mutex pool_mutex;
    std::vector<Block*> free_blocks;
    std::vector<std::unique_ptr<Block>> all_blocks;
    u64 executed_serial = 0; ///< worker, under pool_mutex
    u64 order_errors = 0;
};

static_assert(static_cast<u32>(ReplayClass::Count) <= 32, "mirror_valid is a 32-bit mask");

} // namespace VideoCommon::MT
