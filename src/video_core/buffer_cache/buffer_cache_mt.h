// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// BufferCache's multithreaded back methods (docs/mt-phase4-spec.md §3.4, §6.5). Included at the
// end of buffer_cache.h.

#include <atomic>
#include <cstring>

#include "video_core/buffer_cache/buffer_cache_base.h"

namespace VideoCommon {

template <class P>
bool BufferCache<P>::MtHasHostBuffer(DAddr device_addr) const noexcept {
    return device_addr != 0 && (device_addr >> CACHING_PAGEBITS) < page_table.size();
}

template <class P>
u32 BufferCache<P>::MtUniformBufferAlignment() {
    if constexpr (IS_OPENGL) {
        return 1;
    } else {
        return runtime.GetUniformBufferAlignment();
    }
}

template <class P>
u32 BufferCache<P>::MtSkipCacheSize() const noexcept {
    // Written by TickFrame on the committer; the decision is made once, at capture.
    return std::atomic_ref<u32>(channel_state->uniform_buffer_skip_cache_size)
        .load(std::memory_order_relaxed);
}

template <class P>
bool BufferCache<P>::MtIsRegionGpuModified(DAddr device_addr, u64 size) {
    return memory_tracker.IsRegionGpuModified(device_addr, size);
}

template <class P>
bool BufferCache<P>::MtPageHoldsGpuData(DAddr device_addr, u64 size) {
    const DAddr page_begin = Common::AlignDown(device_addr, DEVICE_PAGESIZE);
    const DAddr page_end = Common::AlignUp(device_addr + size, DEVICE_PAGESIZE);
    return Tegra::g_nx2_keep_page_gpu_data.load(std::memory_order_relaxed) !=
               Tegra::Nx2KeepPageGpuData::None &&
           gpu_modified_ranges.Intersects(page_begin, page_end - page_begin);
}

template <class P>
bool BufferCache<P>::MtPrefersSparseSources() {
    if constexpr (requires { runtime.PrefersSparseSources(); }) {
        return runtime.PrefersSparseSources();
    } else {
        return false;
    }
}

template <class P>
Binding BufferCache<P>::MtStorageBinding(GPUVAddr gpu_addr, u64 next_qword, u32 cbuf_index,
                                         bool is_written, bool native_pointer,
                                         bool mapping_sized) const {
    return StorageBufferBindingFromDescriptor(gpu_addr, next_qword, cbuf_index, is_written,
                                              native_pointer, mapping_sized);
}

template <class P>
template <typename Func>
void BufferCache<P>::MtTakeUploads(DAddr device_addr, u32 size, Func&& emit) {
    if (device_addr == 0 || size == 0 || !memory_tracker.HasPendingCpuUpload(device_addr, size)) {
        return;
    }
    memory_tracker.ForEachUploadRange(device_addr, size, [&](u64 range_addr, u64 range_size) {
        DAddr upload_start = range_addr;
        gpu_modified_ranges.ForEachInRange(range_addr, range_size,
                                           [&](DAddr gpu_start, DAddr gpu_end) {
                                               if (gpu_start > upload_start) {
                                                   emit(upload_start, gpu_start - upload_start);
                                               }
                                               upload_start = gpu_end;
                                           });
        if (range_addr + range_size > upload_start) {
            emit(upload_start, range_addr + range_size - upload_start);
        }
    });
}

template <class P>
void BufferCache<P>::MtCaptureWrittenRange(DAddr device_addr, u32 size) {
    memory_tracker.MarkRegionAsGpuModified(device_addr, size);
    gpu_modified_ranges.Add(device_addr, size);
    if (Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
        Tegra::Nx2MarkGpuWritten(device_addr, size);
    }
}

template <class P>
void BufferCache<P>::MtApplyUploads(std::span<const MT::UploadEntry> uploads, const u8* blob) {
    if constexpr (USE_MEMORY_MAPS_FOR_UPLOADS) {
        size_t first = 0;
        while (first < uploads.size()) {
            // The pieces of one bound range go into its buffer together, as SynchronizeBuffer
            // copies them.
            size_t last = first + 1;
            u64 total = uploads[first].size;
            while (last < uploads.size() && uploads[last].range_addr == uploads[first].range_addr &&
                   uploads[last].range_size == uploads[first].range_size) {
                total += uploads[last].size;
                ++last;
            }
            const MT::UploadEntry& head = uploads[first];
            const BufferId buffer_id = FindBuffer(head.range_addr, head.range_size, false);
            if (buffer_id != NULL_BUFFER_ID) {
                Buffer& buffer = slot_buffers[buffer_id];
                auto upload_staging = runtime.UploadStagingBuffer(total);
                upload_copies.clear();
                u64 offset = 0;
                for (size_t i = first; i < last; ++i) {
                    const MT::UploadEntry& entry = uploads[i];
                    const u8* const source = entry.heap != 0
                                                 ? reinterpret_cast<const u8*>(entry.data)
                                                 : blob + entry.data;
                    std::memcpy(upload_staging.mapped_span.data() + offset, source, entry.size);
                    upload_copies.push_back(BufferCopy{
                        .src_offset = upload_staging.offset + offset,
                        .dst_offset = buffer.Offset(entry.device_addr),
                        .size = entry.size,
                    });
                    if (entry.size >= 256_KiB) {
                        uploaded_buffer_ranges.emplace_back(entry.device_addr, entry.size);
                    }
                    offset += entry.size;
                }
                const std::span<BufferCopy> copies(upload_copies.data(), upload_copies.size());
                const bool can_reorder = runtime.CanReorderUpload(buffer, copies);
                runtime.CopyBuffer(buffer, upload_staging.buffer, copies, true, can_reorder);
            }
            for (size_t i = first; i < last; ++i) {
                if (uploads[i].heap != 0) {
                    delete[] reinterpret_cast<u8*>(uploads[i].data);
                }
            }
            first = last;
        }
    }
}

} // namespace VideoCommon
