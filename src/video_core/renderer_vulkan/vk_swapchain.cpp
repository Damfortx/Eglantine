// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <vector>

#ifdef __ANDROID__
#include <android/api-level.h>
#endif

#include "common/cpu_features.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "video_core/nx2.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "vulkan/vulkan_core.h"

namespace Vulkan {

namespace {

VkSurfaceFormatKHR ChooseSwapSurfaceFormat(vk::Span<VkSurfaceFormatKHR> formats) {
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        VkSurfaceFormatKHR format;
        format.format = VK_FORMAT_B8G8R8A8_UNORM;
        format.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        return format;
    }
    const auto& found = std::find_if(formats.begin(), formats.end(), [](const auto& format) {
        return format.format == VK_FORMAT_B8G8R8A8_UNORM &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    return found != formats.end() ? *found : formats[0];
}

static VkPresentModeKHR ChooseSwapPresentModeBase(bool has_imm, bool has_mailbox,
                                                  bool has_fifo_relaxed) {
    // Mailbox doesn't lock the application like FIFO (vsync)
    // FIFO present mode locks the framerate to the monitor's refresh rate
    Settings::VSyncMode setting = [has_imm, has_mailbox]() {
        // Choose Mailbox or Immediate if unlocked and those modes are supported
        const auto mode = Settings::values.vsync_mode.GetValue();
        if (Settings::values.frame_gen.GetValue()) {
            return mode == Settings::VSyncMode::FifoRelaxed ? mode : Settings::VSyncMode::Fifo;
        }
        if (Settings::values.use_speed_limit.GetValue() &&
            Settings::values.current_speed_mode.GetValue() != Settings::SpeedMode::Turbo) {
            return mode;
        }
        switch (mode) {
        case Settings::VSyncMode::Fifo:
        case Settings::VSyncMode::FifoRelaxed:
            if (has_mailbox) {
                return Settings::VSyncMode::Mailbox;
            } else if (has_imm) {
                return Settings::VSyncMode::Immediate;
            }
            [[fallthrough]];
        default:
            return mode;
        }
    }();
    if (setting == Settings::VSyncMode::Immediate && !has_imm) {
        setting = Settings::VSyncMode::Mailbox;
    }
    if ((setting == Settings::VSyncMode::Mailbox && !has_mailbox) ||
        (setting == Settings::VSyncMode::FifoRelaxed && !has_fifo_relaxed)) {
        setting = Settings::VSyncMode::Fifo;
    }

    switch (setting) {
    case Settings::VSyncMode::Immediate:
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case Settings::VSyncMode::Mailbox:
        return VK_PRESENT_MODE_MAILBOX_KHR;
    case Settings::VSyncMode::Fifo:
        return VK_PRESENT_MODE_FIFO_KHR;
    case Settings::VSyncMode::FifoRelaxed:
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    default:
        return VK_PRESENT_MODE_FIFO_KHR;
    }
}

// EDEN_NX2_PRESENT_MAILBOX: present a FIFO request with MAILBOX when the surface has it, so
// vkAcquireNextImageKHR/vkQueuePresentKHR don't block on vblank (no tearing, frames may drop).
// Not with frame generation, which relies on FIFO pacing.
static VkPresentModeKHR ChooseSwapPresentMode(bool has_imm, bool has_mailbox,
                                              bool has_fifo_relaxed, bool force_mailbox) {
    const VkPresentModeKHR mode = ChooseSwapPresentModeBase(has_imm, has_mailbox, has_fifo_relaxed);
    if (force_mailbox && has_mailbox && !Settings::values.frame_gen.GetValue() &&
        (mode == VK_PRESENT_MODE_FIFO_KHR || mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return mode;
}

struct PresentStatsState {
    std::array<std::atomic<u64>, static_cast<size_t>(PresentWait::Count)> ns{};
    std::atomic<u64> presents{};
    std::atomic<u64> window_start{};
};
PresentStatsState g_present_stats;

VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, u32 width, u32 height) {
    constexpr auto undefined_size{(std::numeric_limits<u32>::max)()};
    if (capabilities.currentExtent.width != undefined_size) {
        return capabilities.currentExtent;
    }
    VkExtent2D extent;
    extent.width = (std::max)(capabilities.minImageExtent.width,
                            (std::min)(capabilities.maxImageExtent.width, width));
    extent.height = (std::max)(capabilities.minImageExtent.height,
                             (std::min)(capabilities.maxImageExtent.height, height));
    return extent;
}

VkCompositeAlphaFlagBitsKHR ChooseAlphaFlags(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    } else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
        return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    } else {
        LOG_ERROR(Render_Vulkan, "Unknown composite alpha flags value {:#x}",
                  capabilities.supportedCompositeAlpha);
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
}

} // Anonymous namespace

bool PresentStatsOn() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_PRESENT_STATS");
    return Tegra::Nx2Switch(mode, false);
}

u64 PresentStatsNow() {
    return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
}

void PresentStatsAdd(PresentWait site, u64 start_ns) {
    g_present_stats.ns[static_cast<size_t>(site)].fetch_add(PresentStatsNow() - start_ns,
                                                            std::memory_order_relaxed);
}

void PresentStatsCountPresent() {
    constexpr u64 window_ns = 10'000'000'000ULL;
    const u64 now = PresentStatsNow();
    g_present_stats.presents.fetch_add(1, std::memory_order_relaxed);
    u64 start = g_present_stats.window_start.load(std::memory_order_relaxed);
    if (start == 0) {
        g_present_stats.window_start.compare_exchange_strong(start, now);
        return;
    }
    if (now - start < window_ns ||
        !g_present_stats.window_start.compare_exchange_strong(start, now)) {
        return;
    }
    const double seconds = static_cast<double>(now - start) / 1e9;
    std::array<double, static_cast<size_t>(PresentWait::Count)> ms{};
    for (size_t i = 0; i < ms.size(); ++i) {
        ms[i] = static_cast<double>(g_present_stats.ns[i].exchange(0, std::memory_order_relaxed)) /
                1e6 / seconds;
    }
    const u64 presents = g_present_stats.presents.exchange(0, std::memory_order_relaxed);
    LOG_INFO(Render_Vulkan,
             "Present waits per second: a={:.2f} b={:.2f} c={:.2f} d={:.2f} e-submit={:.2f} "
             "(lock {:.2f}) e-present={:.2f} (lock {:.2f}) ms ({} presents)",
             ms[0], ms[1], ms[2], ms[3], ms[4], ms[5], ms[7], ms[6], presents);
}

Swapchain::Swapchain(
    VkSurfaceKHR_T* surface_,
    const Device& device_,
    Scheduler& scheduler_,
    u32 width_,
    u32 height_)
    : surface(surface_)
    , device{device_}
    , scheduler{scheduler_}
{
    Create(surface, width_, height_);
}

Swapchain::~Swapchain() = default;

void Swapchain::Create(
    VkSurfaceKHR_T* surface_,
    u32 width_,
    u32 height_)
{
    is_outdated = false;
    is_suboptimal = false;
    width = width_;
    height = height_;
    surface = surface_;

    const auto physical_device = device.GetPhysical();
    const auto capabilities{physical_device.GetSurfaceCapabilitiesKHR(VkSurfaceKHR(surface))};
    if (capabilities.maxImageExtent.width == 0 || capabilities.maxImageExtent.height == 0) {
        return;
    }

    // Callers drain any deferred present first (PresentManager::DrainDeferredPresents); the lock
    // only guards against a present racing the destruction anyway.
    std::scoped_lock access_lock{access_mutex};

    // Submitted work may still wait on the old acquire/present semaphores; destroying them under
    // it loses the device on MoltenVK (window moved to a display with a different scale).
    if (swapchain) {
        std::scoped_lock lock{scheduler.submit_mutex};
        const VkResult result = device.GetLogical().WaitIdle();
        if (result == VK_ERROR_DEVICE_LOST) {
            device.ReportLoss();
        }
        vk::Check(result);
    }
    Destroy();

    CreateSwapchain(capabilities);
    CreateSemaphores();

    resource_ticks.clear();
    resource_ticks.resize(image_count);
}

bool Swapchain::AcquireNextImage(bool may_flush) {
    const bool stats = PresentStatsOn();
    const u64 acquire_start = stats ? PresentStatsNow() : 0;
    VkResult result;
    {
        // External synchronization with a deferred PresentImage on another thread.
        std::scoped_lock access_lock{access_mutex};
        result = device.GetLogical().AcquireNextImageKHR(
            *swapchain, (std::numeric_limits<u64>::max)(), *present_semaphores[frame_index],
            VK_NULL_HANDLE, &image_index);
    }
    switch (result) {
    case VK_SUCCESS:
        break;
    case VK_SUBOPTIMAL_KHR:
        is_suboptimal = true;
        break;
    case VK_ERROR_OUT_OF_DATE_KHR:
        is_outdated = true;
        break;
    case VK_ERROR_SURFACE_LOST_KHR:
        vk::Check(result);
        break;
    default:
        LOG_ERROR(Render_Vulkan, "vkAcquireNextImageKHR returned {}", string_VkResult(result));
        break;
    }
    if (stats) {
        PresentStatsAdd(PresentWait::Acquire, acquire_start);
    }

    const auto wait_with_frame_pacing = [this, may_flush] {
    switch (Settings::values.frame_pacing_mode.GetValue()) {
    case Settings::FramePacingMode::Target_Auto:
        scheduler.Wait(resource_ticks[image_index], 0.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_30:
        scheduler.Wait(resource_ticks[image_index], 30.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_60:
        scheduler.Wait(resource_ticks[image_index], 60.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_90:
        scheduler.Wait(resource_ticks[image_index], 90.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_120:
        scheduler.Wait(resource_ticks[image_index], 120.0, may_flush);
        break;
    }
    };

    // EDEN_NX2_PRESENT_TICKWAIT_SKIP: skip the wait for the GPU work recorded since this image's
    // last use (and the Target_N fps limiter). The swapchain image is still guarded by its
    // semaphores and PresentManager's present_done fence still throttles the composites.
    static const char tickwait_skip_mode = Tegra::Nx2EnvMode("EDEN_NX2_PRESENT_TICKWAIT_SKIP");
    if (!Tegra::Nx2Switch(tickwait_skip_mode, false)) {
        const u64 pacing_start = stats ? PresentStatsNow() : 0;
#ifdef __ANDROID__
        if (android_get_device_api_level() >= 30) {
            scheduler.Wait(resource_ticks[image_index], 0.0, may_flush);
        } else {
            wait_with_frame_pacing();
        }
#else
        wait_with_frame_pacing();
#endif
        if (stats) {
            PresentStatsAdd(PresentWait::Pacing, pacing_start);
        }
    }

    resource_ticks[image_index] = scheduler.CurrentTick();

    // A suboptimal acquire still acquired an image and signals its semaphore: present it, and let
    // NeedsRecreation() recreate at the next frame. Out of date acquired nothing.
    return is_outdated;
}

void Swapchain::Present(VkSemaphore render_semaphore) {
    PresentImage(render_semaphore, image_index);
}

void Swapchain::PresentImage(VkSemaphore render_semaphore, u32 present_index) {
    std::scoped_lock access_lock{access_mutex};
    const auto present_queue{device.GetPresentQueue()};
    const VkPresentInfoKHR present_info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = render_semaphore ? 1U : 0U,
        .pWaitSemaphores = &render_semaphore,
        .swapchainCount = 1,
        .pSwapchains = swapchain.address(),
        .pImageIndices = &present_index,
        .pResults = nullptr,
    };
    const bool stats = PresentStatsOn();
    const u64 present_start = stats ? PresentStatsNow() : 0;
    std::unique_lock lock{scheduler.submit_mutex};
    const u64 present_call = stats ? PresentStatsNow() : 0;
    if (stats) {
        PresentStatsAdd(PresentWait::PresentLock, present_start);
    }
    const VkResult result = present_queue.Present(present_info);
    lock.unlock();
    if (stats) {
        PresentStatsAdd(PresentWait::PresentCall, present_call);
    }
    switch (result) {
    case VK_SUCCESS:
        break;
    case VK_SUBOPTIMAL_KHR:
        LOG_DEBUG(Render_Vulkan, "Suboptimal swapchain");
        break;
    case VK_ERROR_OUT_OF_DATE_KHR:
        is_outdated = true;
        break;
    case VK_ERROR_SURFACE_LOST_KHR:
        vk::Check(result);
        break;
    default:
        LOG_CRITICAL(Render_Vulkan, "Failed to present with error {}", string_VkResult(result));
        break;
    }
    ++frame_index;
    if (frame_index >= image_count) {
        frame_index = 0;
    }
}

void Swapchain::CreateSwapchain(const VkSurfaceCapabilitiesKHR& capabilities) {
    const auto physical_device{device.GetPhysical()};
    const auto formats{physical_device.GetSurfaceFormatsKHR(VkSurfaceKHR(surface))};
    const auto present_modes = physical_device.GetSurfacePresentModesKHR(VkSurfaceKHR(surface));

    has_mailbox = std::find(present_modes.begin(), present_modes.end(), VK_PRESENT_MODE_MAILBOX_KHR)
                  != present_modes.end();
    has_imm = std::find(present_modes.begin(), present_modes.end(),
                        VK_PRESENT_MODE_IMMEDIATE_KHR) != present_modes.end();
    has_fifo_relaxed = std::find(present_modes.begin(), present_modes.end(),
                                 VK_PRESENT_MODE_FIFO_RELAXED_KHR) != present_modes.end();

    const VkCompositeAlphaFlagBitsKHR alpha_flags{ChooseAlphaFlags(capabilities)};
    surface_format = ChooseSwapSurfaceFormat(formats);
    // Latched per creation so 'a' doesn't make NeedsPresentModeUpdate() recreate every 30 s;
    // for A/B use separate runs (0 vs 1).
    static const char mailbox_mode = Tegra::Nx2EnvMode("EDEN_NX2_PRESENT_MAILBOX");
    force_mailbox = Tegra::Nx2Switch(mailbox_mode, false);
    present_mode = ChooseSwapPresentMode(has_imm, has_mailbox, has_fifo_relaxed, force_mailbox);

    // EDEN_NX2_PRESENT_EXTRA=<n>: n more images than minImageCount + 1, read at creation. The
    // PresentManager caps its frames at 7, so more swapchain images than that buy nothing.
    static const u32 extra_images = [] {
        const char* value = std::getenv("EDEN_NX2_PRESENT_EXTRA");
        return value != nullptr ? static_cast<u32>(std::clamp(std::atoi(value), 0, 6)) : 0U;
    }();
    u32 requested_image_count{capabilities.minImageCount + 1 + extra_images};
    // Ensure Triple buffering if possible.
    if (capabilities.maxImageCount > 0) {
        if (requested_image_count > capabilities.maxImageCount) {
            requested_image_count = capabilities.maxImageCount;
        } else {
            requested_image_count =
                (std::max)(requested_image_count, (std::min)(3U, capabilities.maxImageCount));
        }
    } else {
        requested_image_count = (std::max)(requested_image_count, 3U);
    }
    if (extra_images > 0) {
        requested_image_count = (std::max)(capabilities.minImageCount,
                                           (std::min)(requested_image_count, 7U));
    }
    VkSwapchainCreateInfoKHR swapchain_ci{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = VkSurfaceKHR(surface),
        .minImageCount = requested_image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = {},
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
#ifdef __ANDROID__
        // On Android, do not allow surface rotation to deviate from the frontend.
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
#else
        .preTransform = capabilities.currentTransform,
#endif
        .compositeAlpha = alpha_flags,
        .presentMode = present_mode,
        .clipped = VK_FALSE,
        .oldSwapchain = VkSwapchainKHR{},
    };
    const u32 graphics_family{device.GetGraphicsFamily()};
    const u32 present_family{device.GetPresentFamily()};
    const std::array<u32, 2> queue_indices{graphics_family, present_family};
    if (graphics_family != present_family) {
        swapchain_ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchain_ci.queueFamilyIndexCount = static_cast<u32>(queue_indices.size());
        swapchain_ci.pQueueFamilyIndices = queue_indices.data();
    }
    // According to Vulkan spec, when using VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR,
    // the base format (imageFormat) MUST be included in pViewFormats
    const std::array view_formats{
        swapchain_ci.imageFormat,  // Base format MUST be first
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_B8G8R8A8_SRGB,
#ifdef __ANDROID__
        VK_FORMAT_R8G8B8A8_UNORM,  // Android may use RGBA
        VK_FORMAT_R8G8B8A8_SRGB,
#endif
    };
    VkImageFormatListCreateInfo format_list{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR,
        .pNext = nullptr,
        .viewFormatCount = static_cast<u32>(view_formats.size()),
        .pViewFormats = view_formats.data(),
    };
    if (device.IsKhrSwapchainMutableFormatEnabled()) {
        format_list.pNext = std::exchange(swapchain_ci.pNext, &format_list);
        swapchain_ci.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
    }
    // Request the size again to reduce the possibility of a TOCTOU race condition.
    const auto updated_capabilities = physical_device.GetSurfaceCapabilitiesKHR(VkSurfaceKHR(surface));
    swapchain_ci.imageExtent = ChooseSwapExtent(updated_capabilities, width, height);
    // Don't add code within this and the swapchain creation.
    swapchain = device.GetLogical().CreateSwapchainKHR(swapchain_ci);

    extent = swapchain_ci.imageExtent;

    images = swapchain.GetImages();
    image_count = static_cast<u32>(images.size());
#ifdef __ANDROID__
    // Android is already ordered the same as Switch.
    image_view_format = VK_FORMAT_R8G8B8A8_UNORM;
#else
    image_view_format = VK_FORMAT_B8G8R8A8_UNORM;
#endif
}

void Swapchain::CreateSemaphores() {
    present_semaphores.resize(image_count);
    std::ranges::generate(present_semaphores,
                          [this] { return device.GetLogical().CreateSemaphore(); });
    render_semaphores.resize(image_count);
    std::ranges::generate(render_semaphores,
                          [this] { return device.GetLogical().CreateSemaphore(); });
}

void Swapchain::Destroy() {
    frame_index = 0;
    present_semaphores.clear();
    render_semaphores.clear();
    swapchain.reset();
}

bool Swapchain::NeedsPresentModeUpdate() const {
    const auto requested_mode =
        ChooseSwapPresentMode(has_imm, has_mailbox, has_fifo_relaxed, force_mailbox);
    return present_mode != requested_mode;
}

} // namespace Vulkan
