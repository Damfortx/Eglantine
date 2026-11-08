// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// EDEN_NX2_GPU_TIMESTAMPS (phase 0 of docs/mt-plan.md): a timestamp at the start and the end of
// every submitted command buffer; every 10 s the GPU time they span is logged per frame, to tell
// a GPU-bound frame from a CPU-bound one. Off, the scheduler's hooks cost one flag load.

#include <array>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Nx2GpuTimestamps {
public:
    [[nodiscard]] static bool Enabled() {
        static const bool enabled = std::getenv("EDEN_NX2_GPU_TIMESTAMPS") != nullptr;
        return enabled;
    }

    static Nx2GpuTimestamps& Get() {
        // Leaked: the query pool must not outlive the device at exit.
        static Nx2GpuTimestamps* const instance = new Nx2GpuTimestamps;
        return *instance;
    }

    /// Right after a command buffer began (the scheduler's worker).
    void Begin(const Device& device, vk::CommandBuffer cmdbuf) {
        std::scoped_lock lock{mutex};
        if (!Init(device)) {
            return;
        }
        current = next_pair++ % NumPairs;
        if (in_flight[current]) {
            // The ring wrapped before the GPU got there: skip this one.
            current = NoPair;
            ++skipped;
            return;
        }
        cmdbuf.ResetQueryPool(*pool, current * 2, 2);
        write_timestamp(*cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *pool, current * 2);
    }

    /// Right before it ends; `tick` is what its submission signals.
    void End(vk::CommandBuffer cmdbuf, u64 tick, const MasterSemaphore& semaphore) {
        std::scoped_lock lock{mutex};
        if (pool_failed || !pool || current == NoPair) {
            return;
        }
        write_timestamp(*cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *pool, current * 2 + 1);
        in_flight[current] = true;
        pending.push_back({current, tick});
        current = NoPair;
        Collect(semaphore);
    }

    /// Once per presented guest frame (RasterizerVulkan::TickFrame).
    void NoteFrame() noexcept {
        std::scoped_lock lock{mutex};
        ++frames;
    }

private:
    static constexpr u32 NumPairs = 512;
    static constexpr u32 NoPair = ~0U;

    struct Pending {
        u32 pair;
        u64 tick;
    };

    bool Init(const Device& device) {
        if (pool || pool_failed) {
            return !pool_failed;
        }
        logical = &device.GetLogical();
        const VkPhysicalDeviceProperties properties = device.GetPhysical().GetProperties();
        period_ns = properties.limits.timestampPeriod;
        write_timestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
            device.GetDispatchLoader().vkGetDeviceProcAddr(*device.GetLogical(),
                                                           "vkCmdWriteTimestamp"));
        if (!properties.limits.timestampComputeAndGraphics || write_timestamp == nullptr) {
            pool_failed = true;
            LOG_WARNING(Render_Vulkan, "GPU timestamps: not supported by the device");
            return false;
        }
        pool = device.GetLogical().CreateQueryPool({
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = NumPairs * 2,
            .pipelineStatistics = 0,
        });
        window_start = std::chrono::steady_clock::now();
        LOG_INFO(Render_Vulkan, "GPU timestamps: on ({} ns per tick)", period_ns);
        return true;
    }

    void Collect(const MasterSemaphore& semaphore) {
        while (!pending.empty() && semaphore.IsFree(pending.front().tick)) {
            const Pending done = pending.front();
            pending.pop_front();
            in_flight[done.pair] = false;
            std::array<u64, 2> values{};
            const VkResult result =
                logical->GetQueryResults(*pool, done.pair * 2, 2, sizeof(values), values.data(),
                                         sizeof(u64), VK_QUERY_RESULT_64_BIT);
            if (result != VK_SUCCESS || values[1] < values[0]) {
                ++unavailable;
                continue;
            }
            busy_ns += static_cast<double>(values[1] - values[0]) * period_ns;
            ++submissions;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - window_start < std::chrono::seconds{10}) {
            return;
        }
        const double seconds = std::chrono::duration<double>(now - window_start).count();
        const double busy_ms = busy_ns / 1e6;
        LOG_INFO(Render_Vulkan,
                 "GPU timestamps: {:.2f} ms GPU per frame ({} frames, {:.1f} fps), {:.1f} "
                 "submissions per frame, GPU busy {:.0f}% ({} skipped, {} unavailable)",
                 frames != 0 ? busy_ms / static_cast<double>(frames) : 0.0, frames,
                 static_cast<double>(frames) / seconds,
                 frames != 0 ? static_cast<double>(submissions) / static_cast<double>(frames)
                             : 0.0,
                 100.0 * busy_ms / (seconds * 1000.0), skipped, unavailable);
        window_start = now;
        busy_ns = 0.0;
        submissions = 0;
        frames = 0;
        skipped = 0;
        unavailable = 0;
    }

    std::mutex mutex;
    const vk::Device* logical{};
    vk::QueryPool pool;
    bool pool_failed{};
    PFN_vkCmdWriteTimestamp write_timestamp{};
    float period_ns{1.0f};
    u32 current{NoPair};
    u32 next_pair{};
    std::array<bool, NumPairs> in_flight{};
    std::deque<Pending> pending;
    std::chrono::steady_clock::time_point window_start{};
    double busy_ns{};
    u64 submissions{};
    u64 frames{};
    u64 skipped{};
    u64 unavailable{};
};

} // namespace Vulkan
