// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <boost/container/deque.hpp>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core::Frontend {
class EmuWindow;
} // namespace Core::Frontend

namespace Vulkan {

class Device;
class Scheduler;
class Swapchain;

struct Frame {
    u32 width;
    u32 height;
    u32 index;
    vk::Image image;
    vk::ImageView image_view;
    vk::ImageView storage_view;
    vk::Framebuffer framebuffer;
    vk::CommandBuffer cmdbuf;
    vk::Semaphore render_ready;
    vk::Fence present_done;
    bool storage_capable{};
};

class PresentManager {
public:
    PresentManager(const vk::Instance& instance,
                   Core::Frontend::EmuWindow& render_window,
                   const Device& device,
                   MemoryAllocator& memory_allocator,
                   Scheduler& scheduler,
                   Swapchain& swapchain,
                   vk::SurfaceKHR& surface);
    ~PresentManager();

    /// Returns the last used presentation frame
    Frame* GetRenderFrame();

    /// Pushes a frame for presentation
    void Present(Frame* frame);

    /// Recreates the present frame to match the provided parameters
    void RecreateFrame(Frame* frame, u32 width, u32 height, VkFormat image_view_format,
                       VkRenderPass rd, bool storage);

    [[nodiscard]] bool NeedsStorage(const Frame* frame, bool required) const;

    /// Waits for the present thread to finish presenting all queued frames.
    void WaitPresent();

    /// How many additional frames can be queued without stalling the render thread
    [[nodiscard]] size_t MaxExtraFrames() const;

    [[nodiscard]] std::size_t SwapchainImageCount() const { return swapchain_image_count; }
    [[nodiscard]] VkFormat SwapchainImageFormat() const { return swapchain_image_format; }

private:
    void PresentThread(std::stop_token token);

    /// Returns true when the frame was handed to the deferred present thread, which then owns it
    /// and returns it to free_queue itself.
    bool CopyToSwapchain(Frame* frame);

    bool CopyToSwapchainImpl(Frame* frame);

    /// EDEN_NX2_PRESENT_DEFER (sync presentation only): a job for DeferThread.
    struct DeferredPresent {
        Frame* frame;
        VkSemaphore render_semaphore;
        u32 image_index;
    };

    void QueueDeferredPresent(const DeferredPresent& job);

    void RunDeferredPresent(const DeferredPresent& job);

    void DeferThread(std::stop_token token);

    /// Blocks until no deferred present is queued or running. Must be called (on the presenting
    /// back thread) before any acquire, swapchain recreation/destruction or direct present.
    void DrainDeferredPresents();

    void RecreateSwapchain(Frame* frame);

    void DiscardFrame(Frame* frame);

    void SetImageCount();

private:
    const vk::Instance& instance;
    Core::Frontend::EmuWindow& render_window;
    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    Swapchain& swapchain;
    vk::SurfaceKHR& surface;
    vk::CommandPool cmdpool;
    std::vector<Frame> frames;
    boost::container::deque<Frame*> present_queue;
    boost::container::deque<Frame*> free_queue;
    std::condition_variable_any frame_cv;
    std::condition_variable free_cv;
    std::mutex swapchain_mutex;
    std::mutex queue_mutex;
    std::mutex free_mutex;
    std::jthread present_thread;
    bool blit_supported;
    bool storage_supported;
    bool use_present_thread;
    std::atomic<std::size_t> image_count{};
    std::atomic<std::size_t> swapchain_image_count{};
    std::atomic<VkFormat> swapchain_image_format{};

    // EDEN_NX2_PRESENT_DEFER state. defer_thread is declared last so it is destroyed (joined)
    // before the members it uses.
    boost::container::deque<DeferredPresent> defer_queue;
    std::mutex defer_mutex;
    std::condition_variable_any defer_cv;
    std::condition_variable defer_idle_cv;
    bool defer_busy{};
    std::jthread defer_thread;
};

} // namespace Vulkan
