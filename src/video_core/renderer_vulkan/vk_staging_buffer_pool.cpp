// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/common_types.h"
#include "common/literals.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/mt/c13_vram.h"
#include "video_core/mt/lite8_vram.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
namespace {

using namespace Common::Literals;

// Maximum potential alignment of a Vulkan buffer
constexpr VkDeviceSize MAX_ALIGNMENT = 256;

// Stream buffer size in bytes
// *NIX drivers are more sensitive to increased buffers for streaming.
// Windows ones however, can intake bigger buffers and generally do not OOM.
// - GTX 960 on Windows will not OOM with 256mib
// - GT 1030 on ^NIX will OOM with 256mib
#if defined(__FreeBSD__)
constexpr VkDeviceSize MAX_STREAM_BUFFER_SIZE = 128_MiB;
#else
// 128 MiB on Windows too: 256 MiB sat mostly unused in device memory on Switch 2 titles.
constexpr VkDeviceSize MAX_STREAM_BUFFER_SIZE = 128_MiB;
#endif

size_t GetStreamBufferSize(const Device& device) {
    if (!device.HasDebuggingToolAttached()) {
        if (device.IsIntegrated()) {
            // An integrated GPU's device-local host-visible heap can be no bigger than the stream
            // buffer (Radeon 780M: 256 MiB, 243 MiB budget), which then sat over that budget.
            // Leave a quarter of it free.
            VkDeviceSize heap_size = 0;
            ForEachDeviceLocalHostVisibleHeap(
                device, [&heap_size](size_t, VkMemoryHeap& heap) {
                    heap_size = (std::max)(heap_size, heap.size);
                });
            if (heap_size != 0 && heap_size <= MAX_STREAM_BUFFER_SIZE) {
                return Common::AlignUp(heap_size * 3 / 4, MAX_ALIGNMENT);
            }
        }
        return MAX_STREAM_BUFFER_SIZE;
    }

    VkDeviceSize size{0};
    bool has_device_local_host_visible_heap{};
    ForEachDeviceLocalHostVisibleHeap(device, [&size, &has_device_local_host_visible_heap](
                                                  size_t index, VkMemoryHeap& heap) {
        has_device_local_host_visible_heap = true;
        size = (std::max)(size, heap.size);
    });
    if (has_device_local_host_visible_heap) {
        // If rebar is not supported, cut the max heap size to 40%. This will allow 2 captures to be
        // loaded at the same time in RenderDoc. If rebar is supported, this shouldn't be an issue
        // as the heap will be much larger.
        if (size <= MAX_STREAM_BUFFER_SIZE) {
            size = size * 40 / 100;
        }
    } else {
        size = MAX_STREAM_BUFFER_SIZE;
    }
    return (std::min)(Common::AlignUp(size, MAX_ALIGNMENT), MAX_STREAM_BUFFER_SIZE);
}
} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Device& device_, MemoryAllocator& memory_allocator_,
                                     Scheduler& scheduler_)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
      stream_buffer_size{GetStreamBufferSize(device)}, region_size{stream_buffer_size /
                                                                   StagingBufferPool::NUM_SYNCS} {
    VkBufferCreateInfo stream_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = stream_buffer_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        stream_ci.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        stream_ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    stream_buffer = memory_allocator.CreateBuffer(stream_ci, MemoryUsage::Stream);
    if (device.HasDebuggingToolAttached()) {
        stream_buffer.SetObjectNameEXT("Stream Buffer");
    }
    if (device.IsBufferDeviceAddressSupported()) {
        stream_buffer_address = device.GetLogical().GetBufferDeviceAddress(*stream_buffer);
    }
    stream_pointer = stream_buffer.Mapped();
    ASSERT_MSG(!stream_pointer.empty(), "Stream buffer must be host visible!");
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(size_t size, MemoryUsage usage, bool deferred) {
    if (!deferred && usage == MemoryUsage::Upload && size <= region_size) {
        return GetStreamBuffer(size);
    }
    return GetStagingBuffer(size, usage, deferred);
}

void StagingBufferPool::FreeDeferred(StagingBufferRef& ref) {
    auto& entries = GetCache(ref.usage)[ref.log2_level].entries;
    const auto is_this_one = [&ref](const StagingBuffer& entry) {
        return entry.index == ref.index;
    };
    auto it = std::find_if(entries.begin(), entries.end(), is_this_one);
    ASSERT(it != entries.end());
    ASSERT(it->deferred);
    it->tick = scheduler.CurrentTick();
    it->deferred = false;
    it->last_frame = frame;
}

void StagingBufferPool::TickFrame() {
    ++frame;
    current_delete_level = (current_delete_level + 1) % NUM_LEVELS;

    ReleaseCache(MemoryUsage::DeviceLocal);
    ReleaseCache(MemoryUsage::Upload);
    ReleaseCache(MemoryUsage::Download);
    if (VideoCommon::C13Vram::StagingTrimOn() && frame % VideoCommon::C13Vram::TrimSweepFrames == 0) {
        C13Trim();
    }
    if (VideoCommon::C13Vram::CensusOn() && frame % VideoCommon::C13Vram::CensusFrames == 0) {
        C13Census();
    }
}

void StagingBufferPool::C13Trim() {
    namespace C13 = VideoCommon::C13Vram;
    const auto is_free = [this](const StagingBuffer& entry) {
        return !entry.deferred && scheduler.IsFree(entry.tick);
    };
    u64 trimmed = 0;
    u64 trimmed_bytes = 0;
    const auto erase_marked = [&](StagingBuffers& level, auto&& pred) {
        const size_t before = level.entries.size();
        std::erase_if(level.entries, [&](const StagingBuffer& entry) {
            if (!pred(entry)) {
                return false;
            }
            ++trimmed;
            trimmed_bytes += u64{1} << entry.log2_level;
            return true;
        });
        if (level.entries.size() != before) {
            level.delete_index = 0;
            level.iterate_index = 0;
        }
    };
    // Idle buffers of every pool (the batch 8 sweep keeps them 600 frames, 16 per visit).
    for (StagingBuffersCache* const cache : {&device_local_cache, &upload_cache, &download_cache}) {
        for (StagingBuffers& level : *cache) {
            erase_marked(level, [&](const StagingBuffer& entry) {
                return frame - entry.last_frame >= C13::TrimIdleFrames && is_free(entry);
            });
        }
    }
    // The device-local pool (VRAM) outside loads: at most StagingCap(), oldest first.
    if (!C13::g_loading.load(std::memory_order_relaxed)) {
        u64 held = 0;
        std::vector<StagingBuffer*> candidates;
        for (StagingBuffers& level : device_local_cache) {
            for (StagingBuffer& entry : level.entries) {
                held += u64{1} << entry.log2_level;
                if (is_free(entry)) {
                    candidates.push_back(&entry);
                }
            }
        }
        const u64 cap = C13::StagingCap();
        if (held > cap) {
            std::sort(candidates.begin(), candidates.end(),
                      [](const StagingBuffer* a, const StagingBuffer* b) {
                          return a->last_frame < b->last_frame;
                      });
            for (StagingBuffer* const entry : candidates) {
                if (held <= cap) {
                    break;
                }
                entry->c13_trim = true;
                held -= u64{1} << entry->log2_level;
            }
            for (StagingBuffers& level : device_local_cache) {
                erase_marked(level, [](const StagingBuffer& entry) { return entry.c13_trim; });
            }
        }
    }
    if (trimmed != 0) {
        C13::g_counters.staging_trimmed.fetch_add(trimmed, std::memory_order_relaxed);
        C13::g_counters.staging_trimmed_bytes.fetch_add(trimmed_bytes, std::memory_order_relaxed);
    }
}

void StagingBufferPool::C13Census() {
    namespace C13 = VideoCommon::C13Vram;
    const VkPhysicalDeviceMemoryProperties properties =
        device.GetPhysical().GetMemoryProperties().memoryProperties;
    const auto in_vram = [&properties](const vk::Buffer& buffer) {
        const u32 type = buffer.Location().memory_type;
        if (!buffer || buffer.Location().memory == VK_NULL_HANDLE ||
            type >= properties.memoryTypeCount) {
            return false;
        }
        return (properties.memoryTypes[type].propertyFlags &
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    };
    struct Totals {
        u64 count{}, bytes{}, idle{}, vram{};
    };
    u64 deferred_count = 0;
    const auto sum = [&](const StagingBuffersCache& cache) {
        Totals totals{};
        for (const StagingBuffers& level : cache) {
            for (const StagingBuffer& entry : level.entries) {
                const u64 bytes = u64{1} << entry.log2_level;
                ++totals.count;
                totals.bytes += bytes;
                if (frame - entry.last_frame >= C13::TrimIdleFrames) {
                    totals.idle += bytes;
                }
                if (in_vram(entry.buffer)) {
                    totals.vram += bytes;
                }
                deferred_count += entry.deferred ? 1 : 0;
            }
        }
        return totals;
    };
    const Totals device_totals = sum(device_local_cache);
    const Totals upload_totals = sum(upload_cache);
    const Totals download_totals = sum(download_cache);
    auto& g = C13::g_staging;
    constexpr auto relaxed = std::memory_order_relaxed;
    g.frame.store(frame, relaxed);
    g.device_count.store(device_totals.count, relaxed);
    g.device_bytes.store(device_totals.bytes, relaxed);
    g.device_idle_bytes.store(device_totals.idle, relaxed);
    g.device_vram_bytes.store(device_totals.vram, relaxed);
    g.upload_count.store(upload_totals.count, relaxed);
    g.upload_bytes.store(upload_totals.bytes, relaxed);
    g.upload_idle_bytes.store(upload_totals.idle, relaxed);
    g.upload_vram_bytes.store(upload_totals.vram, relaxed);
    g.download_count.store(download_totals.count, relaxed);
    g.download_bytes.store(download_totals.bytes, relaxed);
    g.download_idle_bytes.store(download_totals.idle, relaxed);
    g.download_vram_bytes.store(download_totals.vram, relaxed);
    g.deferred.store(deferred_count, relaxed);
    g.stream_bytes.store(stream_buffer_size, relaxed);
    g.stream_vram.store(in_vram(stream_buffer), relaxed);
}

StagingBufferRef StagingBufferPool::GetStreamBuffer(size_t size) {
    if (AreRegionsActive(Region(free_iterator) + 1,
                         (std::min)(Region(iterator + size) + 1, NUM_SYNCS))) {
        // Avoid waiting for the previous usages to be free
        return GetStagingBuffer(size, MemoryUsage::Upload);
    }
    const u64 current_tick = scheduler.CurrentTick();
    std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + Region(iterator),
              current_tick);
    used_iterator = iterator;
    free_iterator = (std::max)(free_iterator, iterator + size);

    if (iterator + size >= stream_buffer_size) {
        std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + NUM_SYNCS,
                  current_tick);
        used_iterator = 0;
        iterator = 0;
        free_iterator = size;

        if (AreRegionsActive(0, Region(size) + 1)) {
            // Avoid waiting for the previous usages to be free
            return GetStagingBuffer(size, MemoryUsage::Upload);
        }
    }
    const size_t offset = iterator;
    iterator = Common::AlignUp(iterator + size, MAX_ALIGNMENT);
    return StagingBufferRef{
        .buffer = *stream_buffer,
        .device_address = stream_buffer_address,
        .offset = static_cast<VkDeviceSize>(offset),
        .mapped_span = stream_pointer.subspan(offset, size),
        .usage{},
        .log2_level{},
        .index{},
    };
}

bool StagingBufferPool::AreRegionsActive(size_t region_begin, size_t region_end) const {
    const u64 gpu_tick = scheduler.GetMasterSemaphore().KnownGpuTick();
    return std::any_of(sync_ticks.begin() + region_begin, sync_ticks.begin() + region_end,
                       [gpu_tick](u64 sync_tick) { return gpu_tick < sync_tick; });
};

StagingBufferRef StagingBufferPool::GetStagingBuffer(size_t size, MemoryUsage usage,
                                                     bool deferred) {
    if (const std::optional<StagingBufferRef> ref = TryGetReservedBuffer(size, usage, deferred)) {
        return *ref;
    }
    return CreateStagingBuffer(size, usage, deferred);
}

std::optional<StagingBufferRef> StagingBufferPool::TryGetReservedBuffer(size_t size,
                                                                        MemoryUsage usage,
                                                                        bool deferred) {
    StagingBuffers& cache_level = GetCache(usage)[Common::Log2Ceil(size)];

    const auto is_free = [this](const StagingBuffer& entry) {
        return !entry.deferred && scheduler.IsFree(entry.tick);
    };
    auto& entries = cache_level.entries;
    const auto hint_it = entries.begin() + cache_level.iterate_index;
    auto it = std::find_if(entries.begin() + cache_level.iterate_index, entries.end(), is_free);
    if (it == entries.end()) {
        it = std::find_if(entries.begin(), hint_it, is_free);
        if (it == hint_it) {
            return std::nullopt;
        }
    }
    cache_level.iterate_index = std::distance(entries.begin(), it) + 1;
    it->tick = deferred ? (std::numeric_limits<u64>::max)() : scheduler.CurrentTick();
    ASSERT(!it->deferred);
    it->deferred = deferred;
    it->last_frame = frame;
    return it->Ref();
}

StagingBufferRef StagingBufferPool::CreateStagingBuffer(size_t size, MemoryUsage usage, bool deferred) {
    auto const log2_size = Common::Log2Ceil<u32>(u32(size));
    VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = 1ULL << log2_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        buffer_ci.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        buffer_ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    vk::Buffer buffer = memory_allocator.CreateBuffer(buffer_ci, usage);
    if (device.HasDebuggingToolAttached()) {
        ++buffer_index;
        buffer.SetObjectNameEXT(fmt::format("Staging Buffer {}", buffer_index).c_str());
    }
    const std::span<u8> mapped_span = buffer.Mapped();
    const VkDeviceAddress buffer_address =
        device.IsBufferDeviceAddressSupported()
            ? device.GetLogical().GetBufferDeviceAddress(*buffer)
            : VkDeviceAddress{};
    StagingBuffer& entry = GetCache(usage)[log2_size].entries.emplace_back(StagingBuffer{
        .buffer = std::move(buffer),
        .device_address = buffer_address,
        .mapped_span = mapped_span,
        .usage = usage,
        .log2_level = log2_size,
        .index = unique_ids++,
        .tick = deferred ? (std::numeric_limits<u64>::max)() : scheduler.CurrentTick(),
        .deferred = deferred,
        .last_frame = frame,
    });
    return entry.Ref();
}

StagingBufferPool::StagingBuffersCache& StagingBufferPool::GetCache(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::DeviceLocal:
        return device_local_cache;
    case MemoryUsage::Upload:
        return upload_cache;
    case MemoryUsage::Download:
        return download_cache;
    default:
        ASSERT_MSG(false, "Invalid memory usage={}", usage);
        return upload_cache;
    }
}

void StagingBufferPool::ReleaseCache(MemoryUsage usage) {
    ReleaseLevel(GetCache(usage), current_delete_level);
}

void StagingBufferPool::ReleaseLevel(StagingBuffersCache& cache, size_t log2) {
    constexpr size_t deletions_per_tick = 16;
    auto& staging = cache[log2];
    auto& entries = staging.entries;
    const size_t old_size = entries.size();

    // Batch 8 U4 (EDEN_NX2_VRAM_STAGING_KEEP): a buffer requested in the last 600 frames stays
    // for the next request of its size; freeing the steady-state set every sweep (64 frames)
    // only made the driver allocate it again. Buffers over 64 MiB (one huge upload) still go.
    const char keep_mode = Lite8Vram::StagingKeepMode();
    const bool keep = keep_mode != '0' && log2 <= 26 &&
                      (&cache != &device_local_cache || keep_mode == '2');
    const auto is_deletable = [this, keep](const StagingBuffer& entry) {
        if (keep && frame - entry.last_frame < Lite8Vram::StagingKeepFrames) {
            return false;
        }
        return scheduler.IsFree(entry.tick);
    };
    const size_t begin_offset = staging.delete_index;
    const size_t end_offset = (std::min)(begin_offset + deletions_per_tick, old_size);
    const auto begin = entries.begin() + begin_offset;
    const auto end = entries.begin() + end_offset;
    entries.erase(std::remove_if(begin, end, is_deletable), end);

    const size_t new_size = entries.size();
    staging.delete_index += deletions_per_tick;
    if (staging.delete_index >= new_size) {
        staging.delete_index = 0;
    }
    if (staging.iterate_index > new_size) {
        staging.iterate_index = 0;
    }
}

} // namespace Vulkan
