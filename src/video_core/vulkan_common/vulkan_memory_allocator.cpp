// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/literals.h"
#include "common/logging.h"
#include <ranges>
#include "video_core/vulkan_common/vma.h"
#include "video_core/mt/c10_vram.h"
#include "video_core/mt/lite8_vram.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "common/settings.h"

namespace Vulkan {
namespace {

// Helpers translating MemoryUsage to flags/usage

    [[maybe_unused]] VkMemoryPropertyFlags MemoryUsagePropertyFlags(MemoryUsage usage) {
        switch (usage) {
            case MemoryUsage::DeviceLocal:
                return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            case MemoryUsage::Upload:
                return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            case MemoryUsage::Download:
                return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            case MemoryUsage::Stream:
                return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }
        ASSERT_MSG(false, "Invalid memory usage={}", usage);
        return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

    [[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryUsage usage) {
        if (usage == MemoryUsage::Download) {
            return VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }
        return usage != MemoryUsage::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                    : VkMemoryPropertyFlagBits{};
    }

    [[nodiscard]] VmaAllocationCreateFlags MemoryUsageVmaFlags(MemoryUsage usage) {
        switch (usage) {
            case MemoryUsage::Upload:
            case MemoryUsage::Stream:
                return VMA_ALLOCATION_CREATE_MAPPED_BIT |
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            case MemoryUsage::Download:
                return VMA_ALLOCATION_CREATE_MAPPED_BIT |
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
            case MemoryUsage::DeviceLocal:
                return {};
        }
        return {};
    }

    [[nodiscard]] VmaMemoryUsage MemoryUsageVma(MemoryUsage usage) {
        switch (usage) {
            case MemoryUsage::DeviceLocal:
            case MemoryUsage::Stream:
                return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            case MemoryUsage::Upload:
            case MemoryUsage::Download:
                return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        }
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
} // namespace

//MemoryCommit is now VMA-backed
MemoryCommit::MemoryCommit(VmaAllocator alloc, VmaAllocation a,
                            const VmaAllocationInfo &info) noexcept
        : allocator{alloc}, allocation{a}, memory{info.deviceMemory},
            offset{info.offset}, size{info.size}, mapped_ptr{info.pMappedData} {
    // Log GPU memory allocation
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_memory_tracking.GetValue()) {
        GPU::Logging::GPULogger::GetInstance().LogMemoryAllocation(
            reinterpret_cast<uintptr_t>(memory),
            static_cast<u64>(size),
            0  // Memory property flags (not easily available from VMA)
        );
    }
}

MemoryCommit::~MemoryCommit() { Release(); }

MemoryCommit::MemoryCommit(MemoryCommit &&rhs) noexcept
        : allocator{std::exchange(rhs.allocator, nullptr)},
            allocation{std::exchange(rhs.allocation, nullptr)},
            memory{std::exchange(rhs.memory, VK_NULL_HANDLE)},
            offset{std::exchange(rhs.offset, 0)},
            size{std::exchange(rhs.size, 0)},
            mapped_ptr{std::exchange(rhs.mapped_ptr, nullptr)} {}

MemoryCommit &MemoryCommit::operator=(MemoryCommit &&rhs) noexcept {
    if (this != &rhs) {
        Release();
        allocator = std::exchange(rhs.allocator, nullptr);
        allocation = std::exchange(rhs.allocation, nullptr);
        memory = std::exchange(rhs.memory, VK_NULL_HANDLE);
        offset = std::exchange(rhs.offset, 0);
        size = std::exchange(rhs.size, 0);
        mapped_ptr = std::exchange(rhs.mapped_ptr, nullptr);
    }
    return *this;
}

std::span<u8> MemoryCommit::Map()
{
    if (!allocation) return {};
    if (!mapped_ptr) {
        if (vmaMapMemory(allocator, allocation, &mapped_ptr) != VK_SUCCESS) return {};
    }
    const size_t n = static_cast<size_t>(std::min<VkDeviceSize>(size,
                                                                (std::numeric_limits<size_t>::max)()));
    return std::span<u8>{static_cast<u8 *>(mapped_ptr), n};
}

std::span<const u8> MemoryCommit::Map() const
{
    if (!allocation) return {};
    if (!mapped_ptr) {
        void *p = nullptr;
        if (vmaMapMemory(allocator, allocation, &p) != VK_SUCCESS) return {};
        const_cast<MemoryCommit *>(this)->mapped_ptr = p;
    }
    const size_t n = static_cast<size_t>(std::min<VkDeviceSize>(size,
                                                                (std::numeric_limits<size_t>::max)()));
    return std::span<const u8>{static_cast<const u8 *>(mapped_ptr), n};
}

void MemoryCommit::Unmap()
{
    if (allocation && mapped_ptr) {
        vmaUnmapMemory(allocator, allocation);
        mapped_ptr = nullptr;
    }
}

void MemoryCommit::Release() {
    if (allocation && allocator) {
        // Log GPU memory deallocation
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_memory_tracking.GetValue() &&
            memory != VK_NULL_HANDLE) {
            GPU::Logging::GPULogger::GetInstance().LogMemoryDeallocation(
                reinterpret_cast<uintptr_t>(memory)
            );
        }

        if (mapped_ptr) {
            vmaUnmapMemory(allocator, allocation);
            mapped_ptr = nullptr;
        }
        vmaFreeMemory(allocator, allocation);
    }
    allocation = nullptr;
    allocator = nullptr;
    memory = VK_NULL_HANDLE;
    offset = 0;
    size = 0;
}

MemoryAllocator::MemoryAllocator(const Device &device_)
        : device{device_}, allocator{device.GetAllocator()},
            properties{device_.GetPhysical().GetMemoryProperties().memoryProperties},
            buffer_image_granularity{
                    device_.GetPhysical().GetProperties().limits.bufferImageGranularity} {

    // Preserve the previous "RenderDoc small heap" trimming behavior that we had in original vma minus the heap bug
    if (device.HasDebuggingToolAttached())
    {
        using namespace Common::Literals;
        ForEachDeviceLocalHostVisibleHeap(device, [this](size_t heap_idx, VkMemoryHeap &heap) {
            if (heap.size <= 256_MiB) {
                for (u32 t = 0; t < properties.memoryTypeCount; ++t) {
                    if (properties.memoryTypes[t].heapIndex == heap_idx) {
                        valid_memory_types &= ~(1u << t);
                    }
                }
            }
        });
    }
    static std::atomic<bool> guard_logged{};
    if (VideoCommon::C10Vram::GuardOn() && !guard_logged.exchange(true)) {
        LOG_INFO(Render_Vulkan,
                 "VRAM guard on (EDEN_NX2_VRAM_GUARD): images and device-local buffers require "
                 "device memory before host memory; relocating up to {} host images a frame",
                 VideoCommon::C10Vram::RelocateLimit());
    }
}

MemoryAllocator::~MemoryAllocator() = default;

vk::Image MemoryAllocator::CreateImage(const VkImageCreateInfo &ci, bool dedicated,
                                       u64* host_bytes) const
{
    if (host_bytes != nullptr) {
        *host_bytes = 0;
    }
    const VmaAllocationCreateInfo alloc_ci = {
            // Best fit: images freed by the thousand leave holes in the blocks, which new ones
            // should fill before VMA takes another block.
            .flags = static_cast<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT |
                                                           VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT) |
                     (dedicated ? VmaAllocationCreateFlags{VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT}
                               : VmaAllocationCreateFlags{0}),
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .requiredFlags = 0,
            .preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            .memoryTypeBits = 0,
            .pool = VK_NULL_HANDLE,
            .pUserData = nullptr,
            .priority = 0.f,
    };

    VkImage handle{};
    VmaAllocation allocation{};
    VmaAllocationInfo alloc_info{};
    // Images this large (title 1's sparse texture arrays reach 4.5G texels, ~23 GB) are never
    // backed: VMA would quietly place them in system memory once VRAM is full. They take the
    // unbacked sparse fallback below.
    const u64 texels = u64{ci.extent.width} * ci.extent.height * ci.extent.depth * ci.arrayLayers;
    const bool huge = texels > (u64{1} << 28);
    VkResult result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    bool placed = false;
    if (!huge && VideoCommon::C10Vram::GuardOn()) {
        // Cycle 10 A (EDEN_NX2_VRAM_GUARD): with DEVICE_LOCAL only preferred, a full device heap
        // sent images to system memory without a word (MKW at 2x: 26-32 fps instead of ~50).
        // Require it: within the budget, then past it; only then the old placement below.
        VmaAllocationCreateInfo strict = alloc_ci;
        strict.requiredFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        result = CreateImageAllocation(ci, strict, handle, allocation, alloc_info);
        if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            VideoCommon::C10Vram::g_counters.over_budget.fetch_add(1, std::memory_order_relaxed);
            VideoCommon::C10Vram::RequestEmergencyCollect();
            strict.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
            result = CreateImageAllocation(ci, strict, handle, allocation, alloc_info);
            if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                // The device heap is really full: the texture cache leaves more room from now
                // on (pressure margin) and moves this image back once there is some.
                VideoCommon::C10Vram::g_counters.device_full.fetch_add(1,
                                                                       std::memory_order_relaxed);
                pressure_events.fetch_add(1, std::memory_order_relaxed);
            }
        }
        placed = result == VK_SUCCESS;
    }
    if (!huge && !placed) {
        result = CreateImageAllocation(ci, alloc_ci, handle, allocation, alloc_info);
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !huge) {
        // Over the device memory budget (Switch 2 titles fill 8 GB cards): let the driver page
        // it out rather than failing the draw and aborting.
        static u32 logged{};
        if (logged++ < 32) {
            LOG_WARNING(Render_Vulkan,
                        "Image {}x{}x{} ({} levels, {} layers, format {}) is over the memory "
                        "budget; allocating it anyway",
                        ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                        ci.arrayLayers, static_cast<u32>(ci.format));
        }
        pressure_events.fetch_add(1, std::memory_order_relaxed);
        VmaAllocationCreateInfo relaxed = alloc_ci;
        relaxed.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        result = CreateImageAllocation(ci, relaxed, handle, allocation, alloc_info);
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !huge) {
        // Still out of device memory: a real image in host memory (slow, but valid) beats one
        // with nothing bound, which AMD's drivers do not survive being sampled.
        VmaAllocationCreateInfo host = alloc_ci;
        host.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        host.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        host.preferredFlags = 0;
        result = vmaCreateImage(allocator, &ci, &host, &handle, &allocation, &alloc_info);
        static u32 host_logged{};
        if (result == VK_SUCCESS && host_logged++ < 16) {
            LOG_WARNING(Render_Vulkan,
                        "Image {}x{}x{} ({} levels, {} layers, format {}) is out of device memory; "
                        "placed in host memory",
                        ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                        ci.arrayLayers, static_cast<u32>(ci.format));
        }
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && ci.imageType != VK_IMAGE_TYPE_1D) {
        if (!huge) {
            pressure_events.fetch_add(1, std::memory_order_relaxed);
        }
        // Too big to back at all, e.g. title 1's sparse virtual texture arrays (8192x8192,
        // 68 layers, ~23 GB): on the console only resident tiles have memory. Use an image with
        // nothing bound, so views and copies still work, instead of aborting.
        if (vk::Image sparse = CreateUnbackedImage(ci)) {
            LOG_WARNING(Render_Vulkan,
                        "Image {}x{}x{} ({} levels, {} layers, format {}) does not fit; using an "
                        "unbacked sparse image",
                        ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                        ci.arrayLayers, static_cast<u32>(ci.format));
            return sparse;
        }
    }
    if (result != VK_SUCCESS) {
        LOG_CRITICAL(Render_Vulkan, "Image {}x{}x{} ({} levels, {} layers, format {}) failed: {}",
                     ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                     ci.arrayLayers, static_cast<u32>(ci.format), static_cast<int>(result));
    }
    vk::Check(result);
    if (NoteHostPlacement(true, alloc_info) && host_bytes != nullptr) {
        *host_bytes = alloc_info.size;
    }

    // Log GPU memory allocation for images
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_memory_tracking.GetValue()) {
        GPU::Logging::GPULogger::GetInstance().LogMemoryAllocation(
            reinterpret_cast<uintptr_t>(alloc_info.deviceMemory),
            static_cast<u64>(alloc_info.size),
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        );
    }

    return vk::Image(handle, ci.usage, *device.GetLogical(), allocator, allocation,
                        device.GetDispatchLoader());
}

VkResult MemoryAllocator::CreateImageAllocation(const VkImageCreateInfo& ci,
                                                const VmaAllocationCreateInfo& alloc_ci,
                                                VkImage& handle, VmaAllocation& allocation,
                                                VmaAllocationInfo& alloc_info) const {
    // Batch 8 U4 (EDEN_NX2_VRAM_NODEDICATED): vmaCreateImage gives the image its own device
    // memory whenever the driver prefers that (VK_KHR_dedicated_allocation), however small:
    // one driver allocation per image, slow with the heap at its budget. Small images are
    // suballocated from the shared blocks instead, from plain memory requirements (no
    // dedicated hint; the driver never requires one for these non-external images).
    if ((alloc_ci.flags & VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT) == 0 &&
        alloc_ci.pool == VK_NULL_HANDLE &&
        Tegra::Nx2Switch(Lite8Vram::NoDedicatedMode(), false)) {
        const auto& dld = device.GetDispatchLoader();
        const VkDevice logical = *device.GetLogical();
        VkImage image{};
        VkResult result = dld.vkCreateImage(logical, &ci, nullptr, &image);
        if (result != VK_SUCCESS) {
            return result;
        }
        VkMemoryRequirements requirements{};
        dld.vkGetImageMemoryRequirements(logical, image, &requirements);
        if (requirements.size <= Lite8Vram::NoDedicatedLimit()) {
            VmaAllocationCreateInfo block_ci = alloc_ci;
            // AUTO usages need the resource's create info, which vmaAllocateMemory lacks.
            block_ci.usage = VMA_MEMORY_USAGE_UNKNOWN;
            block_ci.memoryTypeBits = requirements.memoryTypeBits;
            if (alloc_ci.usage == VMA_MEMORY_USAGE_AUTO_PREFER_HOST) {
                block_ci.preferredFlags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            } else {
                block_ci.preferredFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            }
            VmaAllocation block_allocation{};
            result = vmaAllocateMemory(allocator, &requirements, &block_ci, &block_allocation,
                                       &alloc_info);
            if (result == VK_SUCCESS) {
                result = vmaBindImageMemory(allocator, block_allocation, image);
                if (result == VK_SUCCESS) {
                    handle = image;
                    allocation = block_allocation;
                    Lite8Vram::g_counters.nodedicated_images.fetch_add(1, std::memory_order_relaxed);
                    return VK_SUCCESS;
                }
                vmaFreeMemory(allocator, block_allocation);
            }
            dld.vkDestroyImage(logical, image, nullptr);
            return result;
        }
        dld.vkDestroyImage(logical, image, nullptr);
    }
    return vmaCreateImage(allocator, &ci, &alloc_ci, &handle, &allocation, &alloc_info);
}

vk::Image MemoryAllocator::CreateUnbackedImage(const VkImageCreateInfo &ci, bool aliased) const {
    if (ci.imageType == VK_IMAGE_TYPE_1D) {
        return vk::Image{};
    }
    VkImageCreateInfo sparse_ci = ci;
    sparse_ci.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    if (aliased) {
        sparse_ci.flags |= VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;
    }
    VkImage sparse{};
    if (device.GetDispatchLoader().vkCreateImage(*device.GetLogical(), &sparse_ci, nullptr,
                                                 &sparse) != VK_SUCCESS) {
        return vk::Image{};
    }
    return vk::Image(sparse, ci.usage, *device.GetLogical(), allocator, VK_NULL_HANDLE,
                     device.GetDispatchLoader());
}

VmaAllocation MemoryAllocator::AllocateSparseMemory(const VkMemoryRequirements &requirements,
                                                   VkDeviceMemory &memory,
                                                   VkDeviceSize &offset) const {
    const VmaAllocationCreateInfo alloc_ci{
        .flags = 0,
        .usage = VMA_MEMORY_USAGE_UNKNOWN,
        .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        .preferredFlags = 0,
        .memoryTypeBits = requirements.memoryTypeBits,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
        .priority = 0.f,
    };
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    // In a pool of their own (see Device::SeparatePool); anything over half a block, or a pool
    // allocation that fails, takes the default blocks.
    u32 memory_type{};
    if (requirements.size <= Device::SeparatePoolBlockSize / 2 &&
        vmaFindMemoryTypeIndex(allocator, requirements.memoryTypeBits, &alloc_ci, &memory_type) ==
            VK_SUCCESS) {
        if (const VmaPool pool =
                device.GetSeparatePool(Device::SeparatePool::SparsePages, memory_type)) {
            VmaAllocationCreateInfo pool_ci = alloc_ci;
            pool_ci.pool = pool;
            if (vmaAllocateMemory(allocator, &requirements, &pool_ci, &allocation, &info) ==
                VK_SUCCESS) {
                memory = info.deviceMemory;
                offset = info.offset;
                return allocation;
            }
        }
    }
    if (vmaAllocateMemory(allocator, &requirements, &alloc_ci, &allocation, &info) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    memory = info.deviceMemory;
    offset = info.offset;
    return allocation;
}

void MemoryAllocator::FreeSparseMemory(VmaAllocation allocation) const {
    if (allocation != VK_NULL_HANDLE) {
        vmaFreeMemory(allocator, allocation);
    }
}

bool MemoryAllocator::CreateBufferInSeparatePool(const VkBufferCreateInfo& ci, MemoryUsage usage,
                                                 const VmaAllocationCreateInfo& alloc_ci,
                                                 VkBuffer& handle, VmaAllocation& allocation,
                                                 VmaAllocationInfo& alloc_info) const {
    // The buffer cache's buffers, churned by its collector: in a pool of their own (see
    // Device::SeparatePool). Anything over half a block takes the default blocks (dedicated).
    if (usage != MemoryUsage::DeviceLocal || ci.size > Device::SeparatePoolBlockSize / 2) {
        return false;
    }
    u32 memory_type{};
    {
        std::scoped_lock lk{buffer_type_mutex};
        const auto key = (u64{ci.usage} << 32) | ci.flags;
        if (const auto it = buffer_memory_types.find(key); it != buffer_memory_types.end()) {
            memory_type = it->second;
        } else if (vmaFindMemoryTypeIndexForBufferInfo(allocator, &ci, &alloc_ci, &memory_type) ==
                   VK_SUCCESS) {
            buffer_memory_types.emplace(key, memory_type);
        } else {
            return false;
        }
    }
    const VmaPool pool = device.GetSeparatePool(Device::SeparatePool::DeviceBuffers, memory_type);
    if (pool == VK_NULL_HANDLE) {
        return false;
    }
    VmaAllocationCreateInfo pool_ci = alloc_ci;
    pool_ci.pool = pool;
    pool_ci.memoryTypeBits = 0;
    return vmaCreateBuffer(allocator, &ci, &pool_ci, &handle, &allocation, &alloc_info) ==
           VK_SUCCESS;
}

vk::Buffer MemoryAllocator::CreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage) const {
    // MESA will do memcpy() if not marked as host cached, so just force mark it for most buffers
    auto const anv_flags = (usage == MemoryUsage::Stream
        && device.GetDriverID() == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA)
        ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0;
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage),
        .usage = MemoryUsageVma(usage),
        .requiredFlags = 0,
        .preferredFlags = MemoryUsagePreferredVmaFlags(usage) | anv_flags,
        .memoryTypeBits = usage == MemoryUsage::Stream ? 0u : valid_memory_types,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
        .priority = 0.f,
    };

    VkBuffer handle{};
    VmaAllocationInfo alloc_info{};
    VmaAllocation allocation{};
    VkMemoryPropertyFlags property_flags{};

    if (usage == MemoryUsage::DeviceLocal && VideoCommon::C10Vram::GuardOn()) {
        // Cycle 10 A: the separate pool's memory type then requires DEVICE_LOCAL as well.
        VmaAllocationCreateInfo strict = alloc_ci;
        strict.requiredFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (!CreateBufferInSeparatePool(ci, usage, strict, handle, allocation, alloc_info)) {
            vk::Check(CreateGuardedBuffer(ci, alloc_ci, 0, handle, allocation, alloc_info));
        }
        NoteHostPlacement(false, alloc_info);
    } else if (!CreateBufferInSeparatePool(ci, usage, alloc_ci, handle, allocation, alloc_info)) {
        vk::Check(vmaCreateBuffer(allocator, &ci, &alloc_ci, &handle, &allocation, &alloc_info));
    }
    vmaGetAllocationMemoryProperties(allocator, allocation, &property_flags);

    // Log GPU memory allocation for buffers
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_memory_tracking.GetValue()) {
        GPU::Logging::GPULogger::GetInstance().LogMemoryAllocation(
            reinterpret_cast<uintptr_t>(alloc_info.deviceMemory),
            static_cast<u64>(alloc_info.size),
            property_flags
        );
    }

    u8 *data = reinterpret_cast<u8 *>(alloc_info.pMappedData);
    const std::span<u8> mapped_data = data ? std::span<u8>{data, ci.size} : std::span<u8>{};
    const bool is_coherent = (property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    const vk::MemoryLocation location{
        .memory = alloc_info.deviceMemory,
        .offset = alloc_info.offset,
        .memory_type = alloc_info.memoryType,
    };
    return vk::Buffer(handle, *device.GetLogical(), allocator, allocation, mapped_data, is_coherent,
                      location, device.GetDispatchLoader());
}

vk::Buffer MemoryAllocator::CreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage,
                                         VkDeviceSize min_alignment) const {
    if (min_alignment <= 1) {
        return CreateBuffer(ci, usage);
    }
    VkMemoryPropertyFlags anv_flags = 0;
    if (usage == MemoryUsage::Stream &&
        device.GetDriverID() == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA) {
        anv_flags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }
    u32 memory_type_bits = valid_memory_types;
    if (usage == MemoryUsage::Stream) {
        memory_type_bits = 0u;
    }
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage),
        .usage = MemoryUsageVma(usage),
        .requiredFlags = 0,
        .preferredFlags = MemoryUsagePreferredVmaFlags(usage) | anv_flags,
        .memoryTypeBits = memory_type_bits,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
        .priority = 0.f,
    };

    VkBuffer handle{};
    VmaAllocationInfo alloc_info{};
    VmaAllocation allocation{};
    VkMemoryPropertyFlags property_flags{};

    if (usage == MemoryUsage::DeviceLocal && VideoCommon::C10Vram::GuardOn()) {
        vk::Check(CreateGuardedBuffer(ci, alloc_ci, min_alignment, handle, allocation, alloc_info));
        NoteHostPlacement(false, alloc_info);
    } else {
        vk::Check(vmaCreateBufferWithAlignment(allocator, &ci, &alloc_ci, min_alignment, &handle,
                                               &allocation, &alloc_info));
    }
    vmaGetAllocationMemoryProperties(allocator, allocation, &property_flags);

    u8 *data = reinterpret_cast<u8 *>(alloc_info.pMappedData);
    std::span<u8> mapped_data{};
    if (data) {
        mapped_data = std::span<u8>{data, ci.size};
    }
    const bool is_coherent = (property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    const vk::MemoryLocation location{
        .memory = alloc_info.deviceMemory,
        .offset = alloc_info.offset,
        .memory_type = alloc_info.memoryType,
    };
    return vk::Buffer(handle, *device.GetLogical(), allocator, allocation, mapped_data, is_coherent,
                      location, device.GetDispatchLoader());
}

VkResult MemoryAllocator::CreateGuardedBuffer(const VkBufferCreateInfo& ci,
                                              const VmaAllocationCreateInfo& alloc_ci,
                                              VkDeviceSize min_alignment, VkBuffer& handle,
                                              VmaAllocation& allocation,
                                              VmaAllocationInfo& alloc_info) const {
    const auto create = [&](const VmaAllocationCreateInfo& create_ci) {
        return min_alignment > 1
                   ? vmaCreateBufferWithAlignment(allocator, &ci, &create_ci, min_alignment,
                                                  &handle, &allocation, &alloc_info)
                   : vmaCreateBuffer(allocator, &ci, &create_ci, &handle, &allocation,
                                     &alloc_info);
    };
    VmaAllocationCreateInfo strict = alloc_ci;
    strict.requiredFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkResult result = create(strict);
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        VideoCommon::C10Vram::g_counters.over_budget.fetch_add(1, std::memory_order_relaxed);
        VideoCommon::C10Vram::RequestEmergencyCollect();
        strict.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        result = create(strict);
        if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            VideoCommon::C10Vram::g_counters.device_full.fetch_add(1, std::memory_order_relaxed);
            pressure_events.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (result != VK_SUCCESS) {
        // The old placement (AUTO_PREFER_DEVICE: host memory when the device heap is full).
        result = create(alloc_ci);
    }
    return result;
}

bool MemoryAllocator::NoteHostPlacement(bool image, const VmaAllocationInfo& alloc_info) const {
    if (!VideoCommon::C10Vram::GuardOn() || alloc_info.deviceMemory == VK_NULL_HANDLE ||
        !IsHostMemoryType(alloc_info.memoryType)) {
        return false;
    }
    using VideoCommon::C10Vram::Kind;
    VideoCommon::C10Vram::CountPlacedHost(image ? Kind::Image : Kind::Buffer, alloc_info.size);
    static std::atomic<u32> logged{};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
        LOG_WARNING(Render_Vulkan, "{} of {} KiB placed in system memory (device memory full)",
                    image ? "Image" : "Buffer", alloc_info.size >> 10);
    }
    return true;
}

bool MemoryAllocator::IsHostResident(const vk::Buffer& buffer) const noexcept {
    if (!VideoCommon::C10Vram::GuardOn() || !buffer) {
        return false;
    }
    const vk::MemoryLocation location = buffer.Location();
    return location.memory != VK_NULL_HANDLE && IsHostMemoryType(location.memory_type);
}

MemoryCommit MemoryAllocator::Commit(const VkMemoryRequirements &reqs, MemoryUsage usage)
{
    const auto vma_usage = MemoryUsageVma(usage);
    VmaAllocationCreateInfo ci{};
    ci.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage);
    ci.usage = vma_usage;
    ci.memoryTypeBits = reqs.memoryTypeBits & valid_memory_types;
    ci.requiredFlags = 0;
    ci.preferredFlags = MemoryUsagePreferredVmaFlags(usage);

    VmaAllocation a{};
    VmaAllocationInfo info{};

    VkResult res = vmaAllocateMemory(allocator, &reqs, &ci, &a, &info);

    if (res != VK_SUCCESS) {
        // Relax 1: drop budget constraint
        auto ci2 = ci;
        ci2.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        res = vmaAllocateMemory(allocator, &reqs, &ci2, &a, &info);

        // Relax 2: if we preferred DEVICE_LOCAL, drop that preference
        if (res != VK_SUCCESS && (ci.preferredFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            auto ci3 = ci2;
            ci3.preferredFlags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            res = vmaAllocateMemory(allocator, &reqs, &ci3, &a, &info);
        }
    }

    vk::Check(res);
    return MemoryCommit(allocator, a, info);
}

MemoryCommit MemoryAllocator::Commit(const vk::Buffer &buffer, MemoryUsage usage) {
    // Allocate memory appropriate for this buffer automatically
    const auto vma_usage = MemoryUsageVma(usage);

    VmaAllocationCreateInfo ci{};
    ci.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage);
    ci.usage = vma_usage;
    ci.requiredFlags = 0;
    ci.preferredFlags = MemoryUsagePreferredVmaFlags(usage);
    ci.pool = VK_NULL_HANDLE;
    ci.pUserData = nullptr;
    ci.priority = 0.0f;

    const VkBuffer raw = *buffer;

    VmaAllocation a{};
    VmaAllocationInfo info{};

    // Let VMA infer memory requirements from the buffer
    VkResult res = vmaAllocateMemoryForBuffer(allocator, raw, &ci, &a, &info);

    if (res != VK_SUCCESS) {
        auto ci2 = ci;
        ci2.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        res = vmaAllocateMemoryForBuffer(allocator, raw, &ci2, &a, &info);

        if (res != VK_SUCCESS && (ci.preferredFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            auto ci3 = ci2;
            ci3.preferredFlags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            res = vmaAllocateMemoryForBuffer(allocator, raw, &ci3, &a, &info);
        }
    }

    vk::Check(res);
    vk::Check(vmaBindBufferMemory2(allocator, a, 0, raw, nullptr));
    return MemoryCommit(allocator, a, info);
}

void LogMemoryStatistics(const Device& device) {
    const VmaAllocator allocator = device.GetAllocator();
    const VkPhysicalDeviceMemoryProperties* properties{};
    vmaGetMemoryProperties(allocator, &properties);
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator, &stats);
    for (u32 type = 0; type < properties->memoryTypeCount; ++type) {
        const VmaStatistics& s = stats.memoryType[type].statistics;
        if (s.blockCount == 0) {
            continue;
        }
        LOG_DEBUG(Render_Vulkan, "VMA type {} (heap {}, flags {:#x}): {} blocks {} MiB, {} allocations {} MiB",
                 type, properties->memoryTypes[type].heapIndex,
                 static_cast<u32>(properties->memoryTypes[type].propertyFlags), s.blockCount,
                 s.blockBytes >> 20, s.allocationCount, s.allocationBytes >> 20);
    }
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(allocator, budgets.data());
    for (u32 heap = 0; heap < properties->memoryHeapCount; ++heap) {
        LOG_INFO(Render_Vulkan, "VMA heap {}: usage {} MiB of budget {} MiB, VMA blocks {} MiB",
                 heap, budgets[heap].usage >> 20, budgets[heap].budget >> 20,
                 budgets[heap].statistics.blockBytes >> 20);
    }
}

void TickVramStats(const Device& device, const VramCacheFigures& cache) {
    VideoCommon::C10Vram::TickWarning();
    if (!Lite8Vram::StatsEnabled()) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    static u64 frames{};
    struct Snapshot {
        u64 allocs, alloc_bytes, dedicated, dedicated_bytes, failed, alloc_ns, slow, frees,
            free_ns, nodedicated;
    };
    static Snapshot previous{};
    struct GuardSnapshot {
        u64 images, image_bytes, buffers, buffer_bytes, over_budget, device_full, emergencies,
            relocated, relocated_bytes;
    };
    static GuardSnapshot guard_previous{};
    ++frames;
    const Clock::time_point now = Clock::now();
    if (now - last < std::chrono::seconds{10}) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - last).count();
    last = now;
    auto& c = Lite8Vram::g_counters;
    const Snapshot current{
        .allocs = c.allocs.load(std::memory_order_relaxed),
        .alloc_bytes = c.alloc_bytes.load(std::memory_order_relaxed),
        .dedicated = c.dedicated.load(std::memory_order_relaxed),
        .dedicated_bytes = c.dedicated_bytes.load(std::memory_order_relaxed),
        .failed = c.failed.load(std::memory_order_relaxed),
        .alloc_ns = c.alloc_ns.load(std::memory_order_relaxed),
        .slow = c.slow.load(std::memory_order_relaxed),
        .frees = c.frees.load(std::memory_order_relaxed),
        .free_ns = c.free_ns.load(std::memory_order_relaxed),
        .nodedicated = c.nodedicated_images.load(std::memory_order_relaxed),
    };
    const u64 max_alloc_ns = c.max_alloc_ns.exchange(0, std::memory_order_relaxed);
    const VmaAllocator allocator = device.GetAllocator();
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> vma_budgets{};
    if (allocator != nullptr) {
        vmaGetHeapBudgets(allocator, vma_budgets.data());
    }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    const bool has_budget = device.CanReportMemoryUsage();
    const auto properties =
        device.GetPhysical().GetMemoryProperties(has_budget ? &budget : nullptr).memoryProperties;
    std::string heaps;
    for (u32 heap = 0; heap < properties.memoryHeapCount; ++heap) {
        if ((properties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) {
            continue;
        }
        const VmaStatistics& stats = vma_budgets[heap].statistics;
        const u64 slack = stats.blockBytes - (std::min)(stats.blockBytes, stats.allocationBytes);
        // usage: the driver's figure for this process; budget: what the driver lets it use
        // (other programs' VRAM already taken out); size: the heap.
        heaps += fmt::format("{}heap {}: usage {} / budget {} / size {} MiB, VMA {} blocks {} MiB "
                             "({} allocations {} MiB, slack {} MiB)",
                             heaps.empty() ? "" : "; ", heap,
                             has_budget ? budget.heapUsage[heap] >> 20 : 0,
                             has_budget ? budget.heapBudget[heap] >> 20 : 0,
                             properties.memoryHeaps[heap].size >> 20, stats.blockCount,
                             stats.blockBytes >> 20, stats.allocationCount,
                             stats.allocationBytes >> 20, slack >> 20);
    }
    const u64 allocs = current.allocs - previous.allocs;
    LOG_INFO(Render_Vulkan, "VRAM in 10 s ({:.1f} s, {} frames): {}", seconds, frames, heaps);
    LOG_INFO(Render_Vulkan,
             "VRAM in 10 s: vkAllocateMemory {} ({} MiB; dedicated {} / {} MiB; failed {}; "
             ">=20 ms {}) total {:.1f} ms max {:.2f} ms avg {:.3f} ms; vkFreeMemory {} total {:.1f} "
             "ms; nodedicated images {}; texture cache used {} MiB, expected {} MiB, critical {} "
             "MiB, pressure events {}",
             allocs, (current.alloc_bytes - previous.alloc_bytes) >> 20,
             current.dedicated - previous.dedicated,
             (current.dedicated_bytes - previous.dedicated_bytes) >> 20,
             current.failed - previous.failed, current.slow - previous.slow,
             static_cast<double>(current.alloc_ns - previous.alloc_ns) / 1e6,
             static_cast<double>(max_alloc_ns) / 1e6,
             allocs != 0 ? static_cast<double>(current.alloc_ns - previous.alloc_ns) / 1e6 /
                               static_cast<double>(allocs)
                         : 0.0,
             current.frees - previous.frees,
             static_cast<double>(current.free_ns - previous.free_ns) / 1e6,
             current.nodedicated - previous.nodedicated, cache.used >> 20, cache.expected >> 20,
             cache.critical >> 20, cache.pressure_events);
    previous = current;
    if (VideoCommon::C10Vram::GuardOn()) {
        // Cycle 10 A: resources in system memory (the slow-mode signature) and the guard's work.
        auto& g = VideoCommon::C10Vram::g_counters;
        const GuardSnapshot guard{
            .images = g.images_placed_host.load(std::memory_order_relaxed),
            .image_bytes = g.image_bytes_placed_host.load(std::memory_order_relaxed),
            .buffers = g.buffers_placed_host.load(std::memory_order_relaxed),
            .buffer_bytes = g.buffer_bytes_placed_host.load(std::memory_order_relaxed),
            .over_budget = g.over_budget.load(std::memory_order_relaxed),
            .device_full = g.device_full.load(std::memory_order_relaxed),
            .emergencies = g.emergencies.load(std::memory_order_relaxed),
            .relocated = g.relocated.load(std::memory_order_relaxed),
            .relocated_bytes = g.relocated_bytes.load(std::memory_order_relaxed),
        };
        LOG_INFO(Render_Vulkan,
                 "VRAM in 10 s: host memory now {} images {} MiB / {} buffers {} MiB; placed in "
                 "host {} images {} MiB / {} buffers {} MiB; device-local over budget {}, device "
                 "full {}; emergency collections {}; relocated {} images {} MiB",
                 g.host_images.load(std::memory_order_relaxed),
                 g.host_image_bytes.load(std::memory_order_relaxed) >> 20,
                 g.host_buffers.load(std::memory_order_relaxed),
                 g.host_buffer_bytes.load(std::memory_order_relaxed) >> 20,
                 guard.images - guard_previous.images,
                 (guard.image_bytes - guard_previous.image_bytes) >> 20,
                 guard.buffers - guard_previous.buffers,
                 (guard.buffer_bytes - guard_previous.buffer_bytes) >> 20,
                 guard.over_budget - guard_previous.over_budget,
                 guard.device_full - guard_previous.device_full,
                 guard.emergencies - guard_previous.emergencies,
                 guard.relocated - guard_previous.relocated,
                 (guard.relocated_bytes - guard_previous.relocated_bytes) >> 20);
        guard_previous = guard;
    }
    frames = 0;
}

} // namespace Vulkan
