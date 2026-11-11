// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/vulkan_common/vma.h"

namespace Vulkan {

    class Device;

/// Hints and requirements for the backing memory type of a commit
    enum class MemoryUsage {
        DeviceLocal, ///< Requests device local host visible buffer, falling back to device local memory.
        Upload,      ///< Requires a host visible memory type optimized for CPU to GPU uploads
        Download,    ///< Requires a host visible memory type optimized for GPU to CPU readbacks
        Stream,      ///< Requests device local host visible buffer, falling back host memory.
    };

    template<typename F>
    void ForEachDeviceLocalHostVisibleHeap(const Device &device, F &&f) {
        auto memory_props = device.GetPhysical().GetMemoryProperties().memoryProperties;
        for (size_t i = 0; i < memory_props.memoryTypeCount; i++) {
            auto &memory_type = memory_props.memoryTypes[i];
            if ((memory_type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                (memory_type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                f(memory_type.heapIndex, memory_props.memoryHeaps[memory_type.heapIndex]);
            }
        }
    }

/// Ownership handle of a memory commitment (real VMA allocation).
    class MemoryCommit {
    public:
        MemoryCommit() noexcept = default;

        MemoryCommit(VmaAllocator allocator, VmaAllocation allocation,
                     const VmaAllocationInfo &info) noexcept;

        ~MemoryCommit();

        MemoryCommit(const MemoryCommit &) = delete;

        MemoryCommit &operator=(const MemoryCommit &) = delete;

        MemoryCommit(MemoryCommit &&) noexcept;

        MemoryCommit &operator=(MemoryCommit &&) noexcept;

        [[nodiscard]] std::span<u8> Map();

        [[nodiscard]] std::span<const u8> Map() const;

        void Unmap();

        explicit operator bool() const noexcept { return allocation != nullptr; }

        VkDeviceMemory Memory() const noexcept { return memory; }

        VkDeviceSize Offset() const noexcept { return offset; }

        VkDeviceSize Size() const noexcept { return size; }

        VmaAllocation Allocation() const noexcept { return allocation; }

    private:
        void Release();

        VmaAllocator allocator{};   ///< VMA allocator
        VmaAllocation allocation{};  ///< VMA allocation handle
        VkDeviceMemory memory{};      ///< Underlying VkDeviceMemory chosen by VMA
        VkDeviceSize offset{};      ///< Offset of this allocation inside VkDeviceMemory
        VkDeviceSize size{};        ///< Size of the allocation
        void *mapped_ptr{};  ///< Optional persistent mapped pointer
    };

/// Memory allocator container.
/// Allocates and releases memory allocations on demand.
    class MemoryAllocator {
    public:
        /**
         * Construct memory allocator
         *
         * @param device_  Device to allocate from
         *
         * @throw vk::Exception on failure
         */
        explicit MemoryAllocator(const Device &device_);

        ~MemoryAllocator();

        MemoryAllocator &operator=(const MemoryAllocator &) = delete;

        MemoryAllocator(const MemoryAllocator &) = delete;

        /// `dedicated`: own device memory, returned to the driver on destruction (short-lived
        /// images would otherwise leave holes in the shared blocks).
        /// `host_bytes` (EDEN_NX2_VRAM_GUARD, mt/c10_vram.h): set to the allocation size when the
        /// image ended up in host memory, else 0.
        vk::Image CreateImage(const VkImageCreateInfo &ci, bool dedicated = false,
                              u64* host_bytes = nullptr) const;

        /// Cycle 13 V2 (EDEN_NX2_C13_RELOC): an image in DEVICE_LOCAL memory within the budget,
        /// or a null image (no host fallback, no pressure event).
        vk::Image C13CreateDeviceImage(const VkImageCreateInfo &ci) const;

        /// EDEN_NX2_VRAM_GUARD: the buffer's memory lacks DEVICE_LOCAL (false with the guard off).
        [[nodiscard]] bool IsHostResident(const vk::Buffer& buffer) const noexcept;

        /// A sparse resident image with nothing bound (reads return zero, writes are dropped), or
        /// a null image when the device can't make one.
        /// aliased: its sparse blocks may share memory (VK_IMAGE_CREATE_SPARSE_ALIASED_BIT).
        vk::Image CreateUnbackedImage(const VkImageCreateInfo &ci, bool aliased = false) const;

        /// Device memory to bind to part of a sparse resource. Null when there is none left.
        /// Device-local buffers in their own VMA pool; false when it doesn't apply or failed.
        bool CreateBufferInSeparatePool(const VkBufferCreateInfo& ci, MemoryUsage usage,
                                        const VmaAllocationCreateInfo& alloc_ci, VkBuffer& handle,
                                        VmaAllocation& allocation,
                                        VmaAllocationInfo& alloc_info) const;

        VmaAllocation AllocateSparseMemory(const VkMemoryRequirements &requirements,
                                           VkDeviceMemory &memory, VkDeviceSize &offset) const;

        void FreeSparseMemory(VmaAllocation allocation) const;

        /// Counts the images that did not fit the device memory budget (made anyway, in host
        /// memory or with nothing bound): the texture cache collects harder when it grows.
        u32 MemoryPressureEvents() const noexcept {
            return pressure_events.load(std::memory_order_relaxed);
        }

        vk::Buffer CreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage) const;

        vk::Buffer CreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage,
                                VkDeviceSize min_alignment) const;

        /**
         * Commits a memory with the specified requirements.
         *
         * @param requirements Requirements returned from a Vulkan call.
         * @param usage        Indicates how the memory will be used.
         *
         * @returns A memory commit.
         */
        MemoryCommit Commit(const VkMemoryRequirements &requirements, MemoryUsage usage);

        /// Commits memory required by the buffer and binds it (for buffers created outside VMA).
        MemoryCommit Commit(const vk::Buffer &buffer, MemoryUsage usage);

    private:
        /// Cycle 10 A: a memory type without DEVICE_LOCAL (system memory over PCIe).
        [[nodiscard]] bool IsHostMemoryType(u32 type) const noexcept {
            return type < properties.memoryTypeCount &&
                   (properties.memoryTypes[type].propertyFlags &
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0;
        }

        /// Cycle 10 A (EDEN_NX2_VRAM_GUARD): a DeviceLocal buffer in DEVICE_LOCAL memory (within
        /// the budget, then past it), else with `alloc_ci` (the old placement, may be host).
        VkResult CreateGuardedBuffer(const VkBufferCreateInfo& ci,
                                     const VmaAllocationCreateInfo& alloc_ci,
                                     VkDeviceSize min_alignment, VkBuffer& handle,
                                     VmaAllocation& allocation,
                                     VmaAllocationInfo& alloc_info) const;

        /// Cycle 13 V1 (EDEN_NX2_C13_OOM): records a device-full failure of `bytes` with the
        /// device usage and budget now (mt/c13_vram.h NoteDeviceFull).
        void C13NoteDeviceFull(u64 bytes) const;

        /// Cycle 10 A: counts a resource placed in host memory; true when it was.
        bool NoteHostPlacement(bool image, const VmaAllocationInfo& alloc_info) const;

        /// vmaCreateImage, or (EDEN_NX2_VRAM_NODEDICATED) a small image in the shared blocks.
        VkResult CreateImageAllocation(const VkImageCreateInfo& ci,
                                       const VmaAllocationCreateInfo& alloc_ci, VkImage& handle,
                                       VmaAllocation& allocation,
                                       VmaAllocationInfo& alloc_info) const;

        static bool IsAutoUsage(VmaMemoryUsage u) noexcept {
            switch (u) {
                case VMA_MEMORY_USAGE_AUTO:
                case VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE:
                case VMA_MEMORY_USAGE_AUTO_PREFER_HOST:
                    return true;
                default:
                    return false;
            }
        }

        const Device &device;                              ///< Device handle.
        VmaAllocator allocator;                           ///< VMA allocator.
        const VkPhysicalDeviceMemoryProperties properties; ///< Physical device memory properties.
        VkDeviceSize buffer_image_granularity;            ///< Adjacent buffer/image granularity
        u32 valid_memory_types{~0u};
        mutable std::atomic<u32> pressure_events{0};
        mutable std::mutex buffer_type_mutex;
        /// Memory type of device-local buffers by usage and flags (CreateBufferInSeparatePool).
        mutable std::unordered_map<u64, u32> buffer_memory_types;
    };

    /// Logs VMA's blocks and allocations per memory type, and each heap's budget.
    void LogMemoryStatistics(const Device& device);

    /// Batch 8 U4: the texture cache's view of device memory for the VRAM stats line.
    struct VramCacheFigures {
        u64 used;      ///< Texture cache total_used_memory
        u64 expected;  ///< Collector thresholds
        u64 critical;
        u32 pressure_events; ///< MemoryAllocator::MemoryPressureEvents
    };
    /// EDEN_NX2_VRAM_STATS=1: every 10 s, "VRAM in 10 s: ..." (per device-local heap driver usage
    /// and budget, VMA blocks / slack, driver allocations, texture cache thresholds). Call once
    /// a frame from one thread; does nothing in between or with the switch off.
    void TickVramStats(const Device& device, const VramCacheFigures& cache);

} // namespace Vulkan
