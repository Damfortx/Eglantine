// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// BufferCache's multithreaded back methods (docs/mt-phase4-spec.md §3.4, §6.5). Included at the
// end of buffer_cache.h.

#include <atomic>
#include <cstring>

#include "common/scope_exit.h"
#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/mt/epoch.h"

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
    MT::TrackerScope tracker_scope{tracker_mutex};
    return memory_tracker.IsRegionGpuModified(device_addr, size);
}

template <class P>
bool BufferCache<P>::MtPageHoldsGpuData(DAddr device_addr, u64 size) {
    const DAddr page_begin = Common::AlignDown(device_addr, DEVICE_PAGESIZE);
    const DAddr page_end = Common::AlignUp(device_addr + size, DEVICE_PAGESIZE);
    MT::TrackerScope tracker_scope{tracker_mutex};
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
    MT::TrackerScope tracker_scope{tracker_mutex};
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
    MT::TrackerScope tracker_scope{tracker_mutex};
    MT::BumpWriteGeneration(device_addr, size);
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

template <class P>
auto BufferCache<P>::TryResolveRange(DAddr device_addr, u32 size, bool sparse_compatible,
                                     MtResolvedRange& out) const noexcept -> MtRangeResolve {
    const u64 page = device_addr >> CACHING_PAGEBITS;
    if (device_addr == 0 || page >= page_table.size()) {
        out = MtResolvedRange{NULL_BUFFER_ID, 0, 0, &slot_buffers.UncheckedAt(NULL_BUFFER_ID)};
        return MtRangeResolve::Null;
    }
    const BufferId id = std::atomic_ref<BufferId>{const_cast<BufferId&>(page_table[page])}.load(
        std::memory_order_acquire);
    if (!id) {
        return MtRangeResolve::Miss;
    }
    const u32 gen = slot_buffers.Generation(id);
    if ((gen & 1) == 0) {
        return MtRangeResolve::Miss;
    }
    const Buffer& buffer = slot_buffers.UncheckedAt(id);
    // CreateBuffer would make or join a buffer: a structure change for the committer.
    if (!buffer.IsInBounds(device_addr, size)) {
        return MtRangeResolve::Miss;
    }
    if constexpr (requires { buffer.IsSparseCompatible(); }) {
        if (sparse_compatible && !buffer.IsSparseCompatible()) {
            return MtRangeResolve::Miss;
        }
    }
    const u32 offset = buffer.Offset(device_addr);
    if (slot_buffers.Generation(id) != gen) {
        return MtRangeResolve::Miss;
    }
    out = MtResolvedRange{id, gen, offset, &buffer};
    return MtRangeResolve::Ok;
}

template <class P>
u32 BufferCache<P>::MtCommitResolvedBuffers(std::span<const MT::ResolvedBufferRef> refs,
                                            const std::array<bool, NUM_STAGES>& enabled_stages,
                                            bool is_indexed) {
    using Ref = MT::ResolvedBufferRef;
    u32 mismatches = 0;
    // config_stage(1): every enabled stage's storage bindings are rebuilt (no driver limit here).
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if (enabled_stages[stage]) {
            channel_state->enabled_storage_buffers[stage] = 0;
            channel_state->written_storage_buffers[stage] = 0;
        }
    }
    const Ref* index_ref = nullptr;
    std::array<const Ref*, NUM_VERTEX_BUFFERS> vertex_refs{};
    for (const Ref& ref : refs) {
        const BufferId id{ref.buffer};
        switch (ref.kind) {
        case Ref::Uniform: {
            // UpdateUniformBuffers + BindHostGraphicsUniformBuffer's cached path (binding index 0
            // on this backend).
            Binding& binding = channel_state->uniform_buffers[ref.stage][ref.slot];
            if (!binding.buffer_id) {
                binding.buffer_id = id;
            } else if (binding.buffer_id != id) {
                ++mismatches;
            }
            Buffer& buffer = slot_buffers[id];
            TouchBuffer(buffer, id);
            buffer.MarkUsage(ref.offset, ref.size);
            channel_state->fast_bound_uniform_buffers[ref.stage] &= ~1u;
            break;
        }
        case Ref::UniformStream:
            channel_state->fast_bound_uniform_buffers[ref.stage] |= 1u;
            channel_state->uniform_buffer_binding_sizes[ref.stage][0] = ref.size;
            break;
        case Ref::Storage: {
            channel_state->enabled_storage_buffers[ref.stage] |= 1U << ref.slot;
            channel_state->written_storage_buffers[ref.stage] |= (ref.written != 0 ? 1U : 0U)
                                                                 << ref.slot;
            channel_state->storage_buffers[ref.stage][ref.slot] = Binding{
                .device_addr = ref.device_addr,
                .gpu_addr = 0,
                .size = ref.size,
                .buffer_id = id,
                .segment_first = 0,
                .segment_count = 0,
            };
            Buffer& buffer = slot_buffers[id];
            TouchBuffer(buffer, id);
            buffer.MarkUsage(ref.offset, ref.size);
            if (ref.written != 0) {
                MarkWrittenBuffer(id, ref.device_addr, ref.size);
            }
            break;
        }
        case Ref::Vertex:
            if (ref.slot < NUM_VERTEX_BUFFERS) {
                vertex_refs[ref.slot] = &ref;
            }
            break;
        case Ref::Index:
            index_ref = &ref;
            break;
        default:
            break;
        }
    }
    const auto binding_of = [](const Ref* ref) {
        if (ref == nullptr || ref->size == 0 || ref->device_addr == 0) {
            return NULL_BINDING;
        }
        return Binding{
            .device_addr = ref->device_addr,
            .gpu_addr = 0,
            .size = ref->size,
            .buffer_id = BufferId{ref->buffer},
            .segment_first = 0,
            .segment_count = 0,
        };
    };
    // UpdateIndexBuffer / UpdateVertexBuffers, dirty-gated as they are.
    auto& flags = maxwell3d->dirty.flags;
    if (is_indexed && flags[Dirty::IndexBuffer]) {
        flags[Dirty::IndexBuffer] = false;
        channel_state->index_buffer = binding_of(index_ref);
    }
    if (flags[Dirty::VertexBuffers]) {
        flags[Dirty::VertexBuffers] = false;
        for (u32 index = 0; index < NUM_VERTEX_BUFFERS; ++index) {
            if (!flags[Dirty::VertexBuffer0 + index]) {
                continue;
            }
            const Binding binding = binding_of(vertex_refs[index]);
            channel_state->vertex_buffers[index] = binding;
            UpdateVertexBufferSlot(index, binding);
        }
    }
    return mismatches;
}

template <class P>
bool BufferCache<P>::MtIsRegionRegistered(DAddr device_addr, u64 size) {
    // Game threads (write traps) read slots the committer may erase meanwhile: with deferred
    // erasure (EDEN_NX2_MT_EBR) they read under an epoch guard of their own reader slot, so a
    // buffer erased after the page_table load is not destroyed (or its slot reused) under them.
    MT::EpochManager* const epochs = mt_epochs.load(std::memory_order_acquire);
    int reader = -1;
    if (epochs != nullptr) {
        reader = epochs->ThreadReader();
        if (reader < 0) {
            return true; // no reader slot left: treat as registered (the trap marks the pages)
        }
        epochs->Enter(reader);
    }
    SCOPE_EXIT {
        if (reader >= 0) {
            epochs->Exit(reader);
        }
    };
    const DAddr end_addr = device_addr + size;
    const u64 page_end = Common::DivCeil(end_addr, CACHING_PAGESIZE);
    for (u64 page = device_addr >> CACHING_PAGEBITS; page < page_end && page < page_table.size();
         ++page) {
        const BufferId buffer_id =
            std::atomic_ref<BufferId>{page_table[page]}.load(std::memory_order_acquire);
        if (!buffer_id) {
            continue;
        }
        const Buffer& buffer = slot_buffers[buffer_id];
        const DAddr buf_start_addr = buffer.CpuAddr();
        const DAddr buf_end_addr = buf_start_addr + buffer.SizeBytes();
        if (buf_start_addr < end_addr && device_addr < buf_end_addr) {
            return true;
        }
    }
    return false;
}

template <class P>
typename BufferCache<P>::MtTrapResult BufferCache<P>::MtTrapWrite(DAddr device_addr, u64 size,
                                                                  bool trap) {
    MT::BumpWriteGeneration(device_addr, size, true);
    {
        MT::TrackerScope tracker_scope{tracker_mutex};
        // Registration is checked under the tracker lock: a buffer the committer creates
        // concurrently is in page_table before its first upload clears (and protects) its pages.
        if (trap && !MtIsRegionRegistered(device_addr, size)) {
            return MtTrapResult::Done;
        }
        if (trap && memory_tracker.IsRegionGpuModified(device_addr, size)) {
            return MtTrapResult::GpuModified;
        }
        // WriteMemory's GPU-data cases (ClearDownload, keeping the page's GPU bytes) need the
        // structure under `mutex`.
        const DAddr page_begin = Common::AlignDown(device_addr, DEVICE_PAGESIZE);
        const DAddr page_end = Common::AlignUp(device_addr + size, DEVICE_PAGESIZE);
        if (IsRegionGpuModified(page_begin, page_end - page_begin)) {
            return MtTrapResult::NeedsMutex;
        }
        memory_tracker.MarkRegionAsCpuModified(device_addr, size);
        MT::NoteGuestWrite(device_addr, size);
        if constexpr (requires { runtime.IsVertexRepacked(u32{}); }) {
            mt_stale_ranges.emplace_back(device_addr, size);
            mt_stale_pending.store(true, std::memory_order_release);
        }
    }
    Nx2NoteListHostWrite(device_addr, size);
    return MtTrapResult::Done;
}

template <class P>
void BufferCache<P>::MtCaptureDmaCopy(DAddr src, DAddr dst, u64 amount,
                                      std::vector<std::pair<DAddr, u64>>& intervals) {
    MT::TrackerScope tracker_scope{tracker_mutex};
    MT::BumpWriteGeneration(dst, amount);
    gpu_modified_ranges.ForEachInRange(src, amount, [&](DAddr begin, DAddr end) {
        intervals.emplace_back(dst + (begin - src), end - begin);
    });
    gpu_modified_ranges.Subtract(dst, amount);
    for (const auto& [addr, size] : intervals) {
        gpu_modified_ranges.Add(addr, size);
        if (Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
            Tegra::Nx2MarkGpuWritten(addr, size);
        }
    }
    if (!intervals.empty()) {
        memory_tracker.MarkRegionAsGpuModified(dst, amount);
    }
}

template <class P>
void BufferCache<P>::MtCommitDmaCopy(DAddr src, DAddr dst, u64 amount,
                                     std::span<const std::pair<DAddr, u64>> intervals) {
    ClearDownload(dst, amount);
    MarkVertexBuffersStale(dst, amount);
    BufferId buffer_a;
    BufferId buffer_b;
    do {
        channel_state->has_deleted_buffers = false;
        buffer_a = FindBuffer(src, static_cast<u32>(amount), false);
        buffer_b = FindBuffer(dst, static_cast<u32>(amount), false);
    } while (channel_state->has_deleted_buffers);
    auto& src_buffer = slot_buffers[buffer_a];
    auto& dest_buffer = slot_buffers[buffer_b];
    // Their CPU-modified bytes were captured (MtApplyUploads): no SynchronizeBuffer.
    std::array copies{BufferCopy{
        .src_offset = src_buffer.Offset(src),
        .dst_offset = dest_buffer.Offset(dst),
        .size = amount,
    }};
    for (const auto& [addr, size] : intervals) {
        QueueFenceDownload(addr, size);
        if (Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
            Nx2NoteListWrite(buffer_b, addr, size);
        }
    }
    const auto& copy = copies[0];
    src_buffer.MarkUsage(copy.src_offset, copy.size);
    dest_buffer.MarkUsage(copy.dst_offset, copy.size);
    runtime.CopyBuffer(dest_buffer, src_buffer, copies, true);
    if (!intervals.empty() &&
        (Settings::IsGPUFenceBehaviorBalanced() || Settings::IsGPUFenceBehaviorAccurate())) {
        runtime.Finish();
    }
}

template <class P>
void BufferCache<P>::MtApplyStaleRanges() {
    if (!mt_stale_pending.load(std::memory_order_acquire)) {
        return;
    }
    MT::TrackerScope tracker_scope{tracker_mutex};
    mt_stale_pending.store(false, std::memory_order_relaxed);
    for (const auto& [addr, size] : mt_stale_ranges) {
        MarkVertexBuffersStale(addr, size);
    }
    mt_stale_ranges.clear();
}

} // namespace VideoCommon
