// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <array>
#include <bit>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include "common/container/unordered_map.h"

#include "common/common_types.h"
#include "common/div_ceil.h"
#include "common/literals.h"
#include "common/lru_cache.h"
#include "common/range_sets.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/slot_vector.h"
#include "video_core/nx2.h"
#include "common/spin_recursive_mutex.h"
#include "video_core/buffer_cache/buffer_base.h"
#include "video_core/buffer_cache/virtual_range_cache.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/delayed_destruction_ring.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/mt/capture.h"
#include "video_core/mt/c12_commit.h"
#include "video_core/mt/c13_bank.h"
#include "video_core/mt/lite2_tracker_switches.h"
#include "video_core/mt/lite_switches.h"
#include "video_core/mt/resolve_packet.h"
#include "video_core/mt/role.h"
#include "video_core/mt/stable_slot_vector.h"
#include "video_core/mt/seq_diet.h"
#include "video_core/mt/tracker_lock.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon {

using BufferId = Common::SlotId;

using VideoCore::Surface::PixelFormat;
using namespace Common::Literals;

#ifdef __APPLE__
constexpr u32 NUM_VERTEX_BUFFERS = 16;
#else
constexpr u32 NUM_VERTEX_BUFFERS = 32;
#endif
constexpr u32 NUM_TRANSFORM_FEEDBACK_BUFFERS = 4;
constexpr u32 NUM_GRAPHICS_UNIFORM_BUFFERS = 18;
constexpr u32 NUM_COMPUTE_UNIFORM_BUFFERS = 8;
// Native compute shaders bind up to 18 global memory ranges; the enable masks are 32 bits wide.
constexpr u32 NUM_STORAGE_BUFFERS = 32;
// Compute programs can take pointer table entries beyond 32 bindings (Shader::Info::MAX_SSBOS).
constexpr u32 NUM_COMPUTE_STORAGE_BUFFERS = 64;
constexpr u32 NUM_TEXTURE_BUFFERS = 32;
constexpr u32 NUM_STAGES = 5;

static_assert(NUM_GRAPHICS_UNIFORM_BUFFERS <= 32, "fast bitmask must fit u32");

using UniformBufferSizes = std::array<std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS>, NUM_STAGES>;
using ComputeUniformBufferSizes = std::array<u32, NUM_COMPUTE_UNIFORM_BUFFERS>;

enum class ObtainBufferSynchronize : u32 {
    NoSynchronize = 0,
    FullSynchronize = 1,
    SynchronizeNoDirty = 2,
};

enum class ObtainBufferOperation : u32 {
    DoNothing = 0,
    MarkAsWritten = 1,
    DiscardWrite = 2,
    MarkQuery = 3,
};

static constexpr BufferId NULL_BUFFER_ID{0};
static constexpr u32 DEFAULT_SKIP_CACHE_SIZE = static_cast<u32>(4_KiB);

struct Binding {
    DAddr device_addr{};
    GPUVAddr gpu_addr{};
    u32 size{};
    BufferId buffer_id;
    u32 segment_first{};
    u32 segment_count{};
};

struct MultiRangeSegment {
    BufferId buffer_id;
    DAddr device_addr{};
    u32 size{};
};

struct TextureBufferBinding : Binding {
    PixelFormat format;
};

static constexpr Binding NULL_BINDING{
    .device_addr = 0,
    .size = 0,
    .buffer_id = NULL_BUFFER_ID,
};

/// EDEN_NX2_C12_COMMIT_REPEAT, buffer half (cycle 12 W2, video_core/mt/c12_commit.h): a graphics
/// pipeline's last resolved uniform and storage bindings (in enabled-bit order, stage by stage).
/// The ids are FindBuffer's (and ResolveStorageBinding's single-range) results for these
/// (device address, size): while no buffer was created or deleted (deletion epoch, c12 buffer
/// epoch) and the page table layout holds, the same inputs resolve to the same ids. Kept by the
/// pipeline, used by BufferCache::C12UpdateGraphicsBuffers only (the committer).
struct C12GraphicsBufferMemo {
    bool valid{};
    const void* channel{};
    const void* gpu_memory{};
    u64 generation{};     ///< odd page table layout generation of the save
    u64 deletion_epoch{}; ///< buffer_deletion_epoch after the save's resolution
    u64 c12_epoch{};      ///< c12_buffer_epoch after the save's resolution
    std::array<u32, NUM_STAGES> uniform_masks{};
    std::array<u32, NUM_STAGES> storage_masks{};
    std::array<u32, NUM_STAGES> written_storage{};
    boost::container::small_vector<Binding, 24> uniforms;
    boost::container::small_vector<Binding, 8> storage;
};

template <typename Buffer>
struct HostBindings {
    boost::container::static_vector<Buffer*, NUM_VERTEX_BUFFERS> buffers;
    boost::container::static_vector<u64, NUM_VERTEX_BUFFERS> offsets;
    boost::container::static_vector<u64, NUM_VERTEX_BUFFERS> sizes;
    boost::container::static_vector<u64, NUM_VERTEX_BUFFERS> strides;
    u32 min_index{NUM_VERTEX_BUFFERS};
    u32 max_index{0};
};

class BufferCacheChannelInfo : public ChannelInfo {
public:
    BufferCacheChannelInfo() = delete;
    BufferCacheChannelInfo(Tegra::Control::ChannelState& state) noexcept : ChannelInfo(state) {}
    BufferCacheChannelInfo(const BufferCacheChannelInfo& state) = delete;
    BufferCacheChannelInfo& operator=(const BufferCacheChannelInfo&) = delete;

    Binding index_buffer;
    std::array<Binding, NUM_VERTEX_BUFFERS> vertex_buffers;
    std::array<std::array<Binding, NUM_GRAPHICS_UNIFORM_BUFFERS>, NUM_STAGES> uniform_buffers;
    std::array<std::array<Binding, NUM_STORAGE_BUFFERS>, NUM_STAGES> storage_buffers;
    std::array<std::array<TextureBufferBinding, NUM_TEXTURE_BUFFERS>, NUM_STAGES> texture_buffers;
    std::array<Binding, NUM_TRANSFORM_FEEDBACK_BUFFERS> transform_feedback_buffers;
    Binding count_buffer_binding;
    Binding indirect_buffer_binding;

    std::array<Binding, NUM_COMPUTE_UNIFORM_BUFFERS> compute_uniform_buffers;
    std::array<Binding, NUM_COMPUTE_STORAGE_BUFFERS> compute_storage_buffers;
    std::array<TextureBufferBinding, NUM_TEXTURE_BUFFERS> compute_texture_buffers;

    std::array<u32, NUM_STAGES> enabled_uniform_buffer_masks{};
    u32 enabled_compute_uniform_buffer_mask = 0;

    const UniformBufferSizes* uniform_buffer_sizes{};
    const ComputeUniformBufferSizes* compute_uniform_buffer_sizes{};

    std::array<u32, NUM_STAGES> enabled_storage_buffers{};
    std::array<u32, NUM_STAGES> written_storage_buffers{};
    u64 enabled_compute_storage_buffers = 0;
    u64 written_compute_storage_buffers = 0;

    u32 total_graphics_storage_buffers = 0;
    u32 total_compute_storage_buffers = 0;

    std::array<u32, NUM_STAGES> enabled_texture_buffers{};
    std::array<u32, NUM_STAGES> written_texture_buffers{};
    std::array<u32, NUM_STAGES> image_texture_buffers{};
    u32 enabled_compute_texture_buffers = 0;
    u32 written_compute_texture_buffers = 0;
    u32 image_compute_texture_buffers = 0;

    std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS> uniform_cache_hits{};
    std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS> uniform_cache_shots{};

    u32 uniform_buffer_skip_cache_size = DEFAULT_SKIP_CACHE_SIZE;

    bool has_deleted_buffers = false;

    std::array<u32, NUM_STAGES> dirty_uniform_buffers{};
    std::array<u32, NUM_STAGES> fast_bound_uniform_buffers{};
    std::array<std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS>, NUM_STAGES>
        uniform_buffer_binding_sizes{};
};

template <class P>
class BufferCache : public VideoCommon::ChannelSetupCaches<BufferCacheChannelInfo> {
    // Page size for caching purposes.
    // This is unrelated to the CPU page size and it can be changed as it seems optimal.
#ifdef YUZU_LEGACY
    static constexpr u32 CACHING_PAGEBITS = 12;
#else
    static constexpr u32 CACHING_PAGEBITS = 16;
#endif
    static constexpr u64 CACHING_PAGESIZE = u64{1} << CACHING_PAGEBITS;

    static constexpr bool IS_OPENGL = P::IS_OPENGL;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS =
        P::HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT =
        P::HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = P::NEEDS_BIND_UNIFORM_INDEX;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = P::NEEDS_BIND_STORAGE_INDEX;
    static constexpr bool USE_MEMORY_MAPS = P::USE_MEMORY_MAPS;
    static constexpr bool SEPARATE_IMAGE_BUFFERS_BINDINGS = P::SEPARATE_IMAGE_BUFFER_BINDINGS;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = P::USE_MEMORY_MAPS_FOR_UPLOADS;

#ifdef YUZU_LEGACY
    static constexpr s64 TARGET_THRESHOLD = 3_GiB;
#else
    static constexpr s64 TARGET_THRESHOLD = 4_GiB;
#endif

    static constexpr s64 DEFAULT_EXPECTED_MEMORY = 512_MiB;
    static constexpr s64 DEFAULT_CRITICAL_MEMORY = 1_GiB;

    // Debug Flags.

    static constexpr bool DISABLE_DOWNLOADS = true;

    using Maxwell = Tegra::Engines::Maxwell3D::Regs;

    using Runtime = typename P::Runtime;
    using Buffer = typename P::Buffer;
    using Async_Buffer = typename P::Async_Buffer;
    using MemoryTracker = typename P::MemoryTracker;

    struct OverlapResult {
        boost::container::small_vector<BufferId, 16> ids;
        DAddr begin;
        DAddr end;
        bool has_stream_leap = false;
    };

public:
    explicit BufferCache(Tegra::MaxwellDeviceMemoryManager& device_memory_, Runtime& runtime_);

    ~BufferCache();

    void TickFrame();

    bool BindMultiRangeStorage(const Binding& binding, bool is_written,
                               std::span<const MultiRangeSegment> pool);

    void ResolveMultiRangeStorage(Binding& binding, bool is_written,
                                  std::vector<MultiRangeSegment>& pool);

    void UnmapGPUMemory(size_t as_id, GPUVAddr gpu_addr, size_t size);

    /// `keep_page_gpu_data`: download GPU-written bytes sharing the written pages first (false
    /// when the memory is going away).
    void WriteMemory(DAddr device_addr, u64 size, bool keep_page_gpu_data = true);

    void CachedWriteMemory(DAddr device_addr, u64 size);

    // Multithreaded back (buffer_cache_mt.h). Sequencer, with `mutex` held: what the draw path
    // decides and reads from the tracker and guest memory, ahead of the committer.
    [[nodiscard]] bool MtHasHostBuffer(DAddr device_addr) const noexcept;
    [[nodiscard]] u32 MtUniformBufferAlignment();
    [[nodiscard]] u32 MtSkipCacheSize() const noexcept;
    [[nodiscard]] bool MtIsRegionGpuModified(DAddr device_addr, u64 size);
    [[nodiscard]] bool MtPageHoldsGpuData(DAddr device_addr, u64 size);
    [[nodiscard]] bool MtPrefersSparseSources();
    [[nodiscard]] Binding MtStorageBinding(GPUVAddr gpu_addr, u64 next_qword, u32 cbuf_index,
                                           bool is_written, bool native_pointer = false,
                                           bool mapping_sized = false,
                                           bool table_pointer = false) const;
    /// Takes the CPU-modified parts of the range out of the tracker (SynchronizeBuffer's walk):
    /// emit(device_addr, size) for each, whose bytes the caller copies now.
    template <typename Func>
    void MtTakeUploads(DAddr device_addr, u32 size, Func&& emit);
    /// The tracker side of MarkWrittenBuffer, at capture.
    void MtCaptureWrittenRange(DAddr device_addr, u32 size);
    /// Committer: copies captured bytes into the buffers that hold their ranges.
    void MtApplyUploads(std::span<const MT::UploadEntry> uploads, const u8* blob);
    /// C13 W2 (EDEN_NX2_C13_BANK=1, sequencer): the native uniform bank re-upload, once per
    /// frame, as a guest write marked before the bound range's uploads are taken (call right
    /// before SeqTakeUploads of a non-streamed uniform). `declared_size` is the shader's size for
    /// the slot (the 64 KiB test), `native_tiling` the channel's MemoryManager::UsesNativeTiling.
    void MtC13CaptureBank(DAddr device_addr, u32 size, u32 declared_size, bool native_tiling);
    /// C13 W2 B2 (EDEN_NX2_C13_CLAIM=1): ClaimCpuWrites for the MT sequencer's invalidations.
    /// The caller holds `mutex` (as for the WriteMemory calls that follow).
    void MtC13ClaimCpuWrites(std::span<const std::pair<DAddr, std::size_t>> regions);
    /// Defer slot erasure through `manager` (nullptr detaches). Only while no other thread uses
    /// the cache.
    void MtSetEpochManager(MT::EpochManager* manager) {
        slot_buffers.SetEpochManager(manager);
        mt_epochs.store(manager, std::memory_order_release);
    }

    // ---- MT resolver reads (buffer_cache_mt.h; docs/mt-phase5-spec.md §7.2) ----
    struct MtResolvedRange {
        BufferId id{};
        u32 gen{};
        u32 offset{};
        const Buffer* buffer{}; ///< valid while the resolver's EpochGuard is held
    };
    enum class MtRangeResolve : u8 { Ok, Null, Miss };
    /// Resolver: FindBuffer without creation, synchronization or LRU. Lock-free.
    [[nodiscard]] MtRangeResolve TryResolveRange(DAddr device_addr, u32 size,
                                                 bool sparse_compatible,
                                                 MtResolvedRange& out) const noexcept;
    /// The null buffer (slot 0, never erased), from any thread.
    [[nodiscard]] const Buffer& MtNullBuffer() const noexcept {
        return slot_buffers.UncheckedAt(NULL_BUFFER_ID);
    }
    [[nodiscard]] u64 MtBufferEpoch() const noexcept {
        return buffer_epoch.load(std::memory_order_acquire);
    }
    /// Committer (EDEN_NX2_MT_SHADOW): the buffer id the last configured draw bound for a
    /// uniform (0), storage (1), vertex (2) or index (3) binding.
    [[nodiscard]] BufferId MtBindingBufferId(u32 kind, size_t stage, u32 slot) const noexcept {
        switch (kind) {
        case 0:
            return channel_state->uniform_buffers[stage][slot].buffer_id;
        case 1:
            return channel_state->storage_buffers[stage][slot].buffer_id;
        case 2:
            return channel_state->vertex_buffers[slot].buffer_id;
        default:
            return channel_state->index_buffer.buffer_id;
        }
    }
    /// Committer (EDEN_NX2_MT_RBUF), `mutex` held: what ConfigureImpl's buffer half leaves in the
    /// channel state and the caches for a draw a resolver resolved: storage bindings, uniform
    /// fast-bind state, LRU touches, usage marks, written storage ranges, and the dirty vertex and
    /// index bindings (BindHostGeometryBuffers then binds them). Returns how many uniform bindings
    /// held another buffer id than the resolver's (MT_RCHECK).
    u32 MtCommitResolvedBuffers(std::span<const MT::ResolvedBufferRef> refs,
                                const std::array<bool, NUM_STAGES>& enabled_stages,
                                bool is_indexed);

    /// Committer: whether a resolved buffer is still the one the resolver saw.
    [[nodiscard]] bool MtValidateBuffer(BufferId id, u32 gen) const noexcept {
        return id == NULL_BUFFER_ID || slot_buffers.IsCurrent(id, gen);
    }

    /// What MtTrapWrite did.
    enum class MtTrapResult : u8 {
        Done,        ///< as WriteMemory/OnCPUWrite returning false
        GpuModified, ///< OnCPUWrite would return true: nothing changed
        NeedsMutex,  ///< GPU-written data in the range: run the full path under `mutex`
    };
    /// EDEN_NX2_MT_TRAPLOCK: a guest write's buffer cache side with only tracker_mutex (callers
    /// hold neither cache mutex). `trap`: OnCPUWrite's semantics (unregistered ranges and
    /// tracker GPU-modified ranges are left alone), else WriteMemory's.
    MtTrapResult MtTrapWrite(DAddr device_addr, u64 size, bool trap);
    /// Lock-free IsRegionRegistered (page_table loads; buffers never relocate).
    [[nodiscard]] bool MtIsRegionRegistered(DAddr device_addr, u64 size);
    /// The stale marks MtTrapWrite queued (committer, `mutex` held).
    void MtApplyStaleRanges();
    /// DMACopy's tracker side at capture (tracker_mutex held, destination not GPU-modified):
    /// the source's GPU-written ranges move to the destination, returned in `intervals`.
    void MtCaptureDmaCopy(DAddr src, DAddr dst, u64 amount,
                          std::vector<std::pair<DAddr, u64>>& intervals);
    /// DMACopy's buffer side on the committer (`mutex` held), the guest copy already made.
    void MtCommitDmaCopy(DAddr src, DAddr dst, u64 amount,
                         std::span<const std::pair<DAddr, u64>> intervals);
    [[nodiscard]] Common::SpinRecursiveMutex& MtTrackerMutex() noexcept {
        return tracker_mutex;
    }

    /// EDEN_NX2_MT_LITE_CTRACK: while one is open on the committer recording a packet
    /// (MT::t_capture set), MarkWrittenBuffer queues its tracker half; the outermost one applies
    /// the queue under a single tracker_mutex hold when it closes. Nests (bind passes open their
    /// own; a caller may wrap a whole draw). Inert with the switch off or outside a packet.
    class MtWrittenBatch {
    public:
        explicit MtWrittenBatch(BufferCache& cache_)
            : cache{MT::t_capture != nullptr && MT::LiteCommitTracker() ? &cache_ : nullptr} {
            if (cache != nullptr) {
                ++cache->mt_written_batch_depth;
            }
        }
        ~MtWrittenBatch() {
            if (cache != nullptr && --cache->mt_written_batch_depth == 0) {
                cache->MtFlushWrittenBatch();
            }
        }
        MtWrittenBatch(const MtWrittenBatch&) = delete;
        MtWrittenBatch& operator=(const MtWrittenBatch&) = delete;

    private:
        BufferCache* cache;
    };

    bool OnCPUWrite(DAddr device_addr, u64 size);

    void DownloadMemory(DAddr device_addr, u64 size);

    std::optional<VideoCore::RasterizerDownloadArea> GetFlushArea(DAddr device_addr, u64 size);

    /// Without downloads at every fence, a guest CPU read is the only way GPU-written buffer
    /// data reaches guest memory, so reads must ask the buffer cache (GetFlushArea).
    bool FlushesOnCpuRead() const {
        return !AsyncBufferDownloads();
    }

    bool InlineMemory(DAddr dest_address, size_t copy_size, std::span<const u8> inlined_buffer);

    void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr, u32 size);

    void DisableGraphicsUniformBuffer(size_t stage, u32 index);

    void UpdateGraphicsBuffers(bool is_indexed);

    /// EDEN_NX2_C12_COMMIT_REPEAT (buffer half): UpdateGraphicsBuffers for a graphics pipeline
    /// that keeps `memo`: takes the memo's uniform/storage buffer ids when every binding and key
    /// matches (index and vertex buffers still update), else the full path and saves the memo.
    /// Leaves the buffers updated either way. Committer only, under the buffer cache mutex.
    void C12UpdateGraphicsBuffers(C12GraphicsBufferMemo& memo, bool is_indexed);

    void UpdateComputeBuffers();

    void BindHostGeometryBuffers(bool is_indexed);

    void BindHostStageBuffers(size_t stage);

    void BindHostComputeBuffers();

    void SetUniformBuffersState(const std::array<u32, NUM_STAGES>& mask,
                                const UniformBufferSizes* sizes);

    void SetComputeUniformBufferState(u32 mask, const ComputeUniformBufferSizes* sizes);

    void UnbindGraphicsStorageBuffers(size_t stage);

    bool BindGraphicsStorageBuffer(size_t stage, size_t ssbo_index, u32 cbuf_index, u32 cbuf_offset,
                                   bool is_written, u32 indirect_offset = ~0U);

    void UnbindGraphicsTextureBuffers(size_t stage);

    void BindGraphicsTextureBuffer(size_t stage, size_t tbo_index, GPUVAddr gpu_addr, u32 size,
                                   PixelFormat format, bool is_written, bool is_image);

    void UnbindComputeStorageBuffers();

    void BindComputeStorageBuffer(size_t ssbo_index, u32 cbuf_index, u32 cbuf_offset,
                                  bool is_written, u32 indirect_offset = ~0U);

    void UnbindComputeTextureBuffers();

    void BindComputeTextureBuffer(size_t tbo_index, GPUVAddr gpu_addr, u32 size, PixelFormat format,
                                  bool is_written, bool is_image);

    [[nodiscard]] std::pair<Buffer*, u32> ObtainBuffer(GPUVAddr gpu_addr, u32 size,
                                                       ObtainBufferSynchronize sync_info,
                                                       ObtainBufferOperation post_op);

    [[nodiscard]] std::pair<Buffer*, u32> ObtainCPUBuffer(DAddr gpu_addr, u32 size,
                                                          ObtainBufferSynchronize sync_info,
                                                          ObtainBufferOperation post_op);
    void FlushCachedWrites();

    /// Where the CPU writes into GPU-written memory that were collected but not applied yet
    /// (WriteMemory) come from: downloads must not write the GPU's older copy over them.
    using PendingCpuWrites = std::function<void(const std::function<void(DAddr, size_t)>&)>;
    void SetPendingCpuWrites(PendingCpuWrites source) {
        pending_cpu_writes = std::move(source);
    }

    /// Takes CPU writes about to be applied one by one out of the GPU-written ranges first, so
    /// applying one doesn't download the GPU's copy over another (WriteMemory keeps the GPU data
    /// on the pages it writes).
    void ClaimCpuWrites(std::span<const std::pair<DAddr, u64>> ranges);

    /// Return true when there are uncommitted buffers to be downloaded
    [[nodiscard]] bool HasUncommittedFlushes() const noexcept;

    /// False only if no download was queued since a commit that left none (no lock needed).
    [[nodiscard]] bool MayHaveUncommittedFlushes() const noexcept {
        return nx2_flush_hint.load(std::memory_order_relaxed);
    }
    /// Under the lock, after a commit: whether downloads are still queued.
    void UpdateFlushHint() noexcept;

    void AccumulateFlushes();

    /// Return true when the caller should wait for async downloads
    [[nodiscard]] bool ShouldWaitAsyncFlushes() const noexcept;

    /// Commit asynchronous downloads
    void CommitAsyncFlushes();
    void CommitAsyncFlushesHigh();

    /// Pop asynchronous downloads
    void PopAsyncFlushes();
    void PopAsyncBuffers();

    bool DMACopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount);

    bool DMAClear(GPUVAddr src_address, u64 amount, u32 value);

    /// Switch 2 GPU-written command lists (nx2.h, g_nx2_command_list_pages). While collecting (a
    /// compute dispatch or a DMA copy being recorded), writes to command list pages are noted;
    /// Nx2CopyListPages then records copies of those pages into host memory after the work.
    void Nx2CollectListCopies(bool collect) {
        nx2_collect_list_copies = collect;
        if (!collect) {
            nx2_list_copy_requests.clear();
        }
    }
    /// True when it recorded copies, which the GPU completes at `tick` (the caller submits).
    bool Nx2CopyListPages(u64 tick);
    enum class Nx2ListRead { Unknown, Copied, Synced };
    /// Front thread: Synced when guest memory holds the page `device_addr` lies in (flushed and
    /// written by no GPU work since), else Copied with `out` filled from the copy of its last
    /// write once the GPU is done with it. Unknown otherwise: the caller drains and flushes.
    Nx2ListRead Nx2ReadListCopy(DAddr device_addr, std::span<u8> out);
    /// Front thread, the split drained: the command list page was flushed to guest memory.
    void Nx2NoteListSynced(DAddr page);
    /// CPU and inline writes: they reach guest memory, so a synced page stays synced, but its copy
    /// no longer holds the page.
    void Nx2NoteListHostWrite(DAddr device_addr, u64 size);
    /// EDEN_NX2_LIST_STATS (front): the state of the command list page `page`: 0 no entry,
    /// 1 synced, 2 copy current, 3 written since (no current copy), 4 written by draws.
    u8 Nx2PeekListPage(DAddr page) {
        if constexpr (IS_OPENGL) {
            return 0;
        } else {
            std::scoped_lock lock{nx2_list_mutex};
            const auto it = nx2_list_pages.find(page);
            if (it == nx2_list_pages.end()) {
                return 0;
            }
            const auto& entry = it->second;
            if (entry.draw_written) {
                return 4;
            }
            if (entry.synced_writes == entry.writes) {
                return 1;
            }
            return entry.copy && entry.copy_writes == entry.writes ? 2 : 3;
        }
    }

    /// Return true when a CPU region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(DAddr addr, size_t size);

    /// Visits the uniform and storage buffers bound for the last draw or dispatch with the cached
    /// host buffer holding each range, if one does, for the frame dump. Nothing is created or
    /// synchronized. Stage 5 is compute.
    template <typename Func>
    void ForEachBoundBuffer(bool compute, Func&& func) {
        const auto visit = [&](u32 stage, bool storage, u32 index, bool written,
                               const Binding& binding, u32 size) {
            if (binding.device_addr == 0 || size == 0) {
                return;
            }
            const Buffer* host = nullptr;
            u32 offset = 0;
            const u64 page = binding.device_addr >> CACHING_PAGEBITS;
            if (const BufferId id = page < page_table.size() ? page_table[page] : BufferId{}) {
                const Buffer& buffer = slot_buffers[id];
                if (buffer.IsInBounds(binding.device_addr, size)) {
                    host = &buffer;
                    offset = buffer.Offset(binding.device_addr);
                }
            }
            func(stage, storage, index, written, binding, size, host, offset);
        };
        if (compute) {
            const auto* const sizes = channel_state->compute_uniform_buffer_sizes;
            ForEachEnabledBit(channel_state->enabled_compute_uniform_buffer_mask, [&](u32 index) {
                const Binding& binding = channel_state->compute_uniform_buffers[index];
                visit(5, false, index, false, binding,
                      sizes ? (std::min)(binding.size, (*sizes)[index]) : binding.size);
            });
            ForEachEnabledBit(channel_state->enabled_compute_storage_buffers, [&](u32 index) {
                const Binding& binding = channel_state->compute_storage_buffers[index];
                visit(5, true, index,
                      ((channel_state->written_compute_storage_buffers >> index) & 1) != 0, binding,
                      binding.size);
            });
            return;
        }
        const auto* const sizes = channel_state->uniform_buffer_sizes;
        for (u32 stage = 0; stage < NUM_STAGES; ++stage) {
            ForEachEnabledBit(channel_state->enabled_uniform_buffer_masks[stage], [&](u32 index) {
                const Binding& binding = channel_state->uniform_buffers[stage][index];
                visit(stage, false, index, false, binding,
                      sizes ? (std::min)(binding.size, (*sizes)[stage][index]) : binding.size);
            });
            ForEachEnabledBit(channel_state->enabled_storage_buffers[stage], [&](u32 index) {
                const Binding& binding = channel_state->storage_buffers[stage][index];
                visit(stage, true, index,
                      ((channel_state->written_storage_buffers[stage] >> index) & 1) != 0, binding,
                      binding.size);
            });
        }
    }

    /// Return true when a region is registered on the cache
    [[nodiscard]] bool IsRegionRegistered(DAddr addr, size_t size);

    /// Return true when a CPU region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(DAddr addr, size_t size);

    void SetDrawIndirect(
        const Tegra::Engines::Maxwell3D::DrawManager::IndirectParams* current_draw_indirect_) {
        current_draw_indirect = current_draw_indirect_;
    }

    [[nodiscard]] std::pair<Buffer*, u32> GetDrawIndirectCount();

    [[nodiscard]] std::pair<Buffer*, u32> GetDrawIndirectBuffer();

    template <typename Func>
    void BufferOperations(Func&& func) {
        do {
            channel_state->has_deleted_buffers = false;
            func();
        } while (channel_state->has_deleted_buffers);
    }

    Common::SpinRecursiveMutex mutex{"buffer"};
    Runtime& runtime;
    bool any_buffer_uploaded = false;
    /// Indirectly read banks of native programs already uploaded this frame.
    std::unordered_set<DAddr> native_indirect_uniforms_this_frame;
    /// C13 W2: the sequencer's own per-frame bank set (sequencer thread only).
    MT::C13Bank::SeqFrameSet c13_seq_banks;

private:
    template <typename Mask, typename Func>
    static void ForEachEnabledBit(Mask enabled_mask, Func&& func) {
        for (u32 index = 0; enabled_mask != 0; ++index, enabled_mask >>= 1) {
            const int disabled_bits = std::countr_zero(enabled_mask);
            index += disabled_bits;
            enabled_mask >>= disabled_bits;
            func(index);
        }
    }

    template <typename Func>
    void ForEachBufferInRange(DAddr device_addr, u64 size, Func&& func) {
        const u64 page_end = Common::DivCeil(device_addr + size, CACHING_PAGESIZE);
        for (u64 page = device_addr >> CACHING_PAGEBITS; page < page_end;) {
            const BufferId buffer_id = page_table[page];
            if (!buffer_id) {
                ++page;
                continue;
            }
            Buffer& buffer = slot_buffers[buffer_id];
            func(buffer_id, buffer);

            const DAddr end_addr = buffer.CpuAddr() + buffer.SizeBytes();
            page = Common::DivCeil(end_addr, CACHING_PAGESIZE);
        }
    }

    static bool IsRangeGranular(DAddr device_addr, size_t size) {
        return (device_addr & ~Core::DEVICE_PAGEMASK) ==
               ((device_addr + size) & ~Core::DEVICE_PAGEMASK);
    }

    void RunGarbageCollector(bool aggressive_gc);

    void BindHostIndexBuffer();

    void BindHostVertexBuffers();

    void BindHostDrawIndirectBuffers();

    void BindHostGraphicsUniformBuffers(size_t stage);

    void BindHostGraphicsUniformBuffer(size_t stage, u32 index, u32 binding_index, bool needs_bind);

    void BindHostGraphicsStorageBuffers(size_t stage);

    void BindHostGraphicsTextureBuffers(size_t stage);

    void BindHostTransformFeedbackBuffers();

    void BindHostVertexBuffer(u32 index, Buffer& buffer, u32 offset, u32 size, u32 stride);

    void BindHostComputeUniformBuffers();

    void BindHostComputeStorageBuffers();

    void BindHostComputeTextureBuffers();

    void DoUpdateGraphicsBuffers(bool is_indexed);

    void DoUpdateComputeBuffers();

    void UpdateIndexBuffer();

    void UpdateVertexBuffers();

    void UpdateVertexBuffer(u32 index);

    void UpdateDrawIndirect();

    void UpdateUniformBuffers(size_t stage);

    void UpdateStorageBuffers(size_t stage);

    void UpdateTextureBuffers(size_t stage);

    void UpdateTransformFeedbackBuffers();

    void UpdateTransformFeedbackBuffer(u32 index);

    void UpdateComputeUniformBuffers();

    void UpdateComputeStorageBuffers();

    void UpdateComputeTextureBuffers();

    void MarkWrittenBuffer(BufferId buffer_id, DAddr device_addr, u32 size,
                           bool needs_sync = false);

    /// MarkWrittenBuffer's tracker half (tracker_mutex held).
    void MarkWrittenTracked(BufferId buffer_id, DAddr device_addr, u32 size);

    /// EDEN_NX2_MT_LITE_CTRACK: applies the written ranges queued by MtWrittenBatch.
    void MtFlushWrittenBatch();

    /// Counts a GPU write to the command list pages in the range (invalidating their copies) and,
    /// while collecting, asks for new copies.
    void Nx2NoteListWrite(BufferId buffer_id, DAddr device_addr, u64 size);

    [[nodiscard]] BufferId FindBuffer(DAddr device_addr, u32 size, bool sparse_compatible);

    void SynchronizeBufferWrites(Buffer& buffer);

    [[nodiscard]] OverlapResult ResolveOverlaps(DAddr device_addr, u32 wanted_size);

    void JoinOverlap(BufferId new_buffer_id, BufferId overlap_id, bool accumulate_stream_score);

    [[nodiscard]] BufferId CreateBuffer(DAddr device_addr, u32 wanted_size, bool sparse_compatible);

    void Register(BufferId buffer_id);

    void Unregister(BufferId buffer_id);

    template <bool insert>
    void ChangeRegister(BufferId buffer_id);

    void TouchBuffer(Buffer& buffer, BufferId buffer_id) noexcept;

    bool SynchronizeBuffer(Buffer& buffer, DAddr device_addr, u32 size);

    void UploadMemory(Buffer& buffer, u64 total_size_bytes, u64 largest_copy,
                      std::span<BufferCopy> copies);

    void ImmediateUploadMemory(Buffer& buffer, u64 largest_copy,
                               std::span<const BufferCopy> copies);

    void MappedUploadMemory(Buffer& buffer, u64 total_size_bytes, std::span<BufferCopy> copies);

    void DownloadBufferMemory(Buffer& buffer_id);

    void DownloadBufferMemory(Buffer& buffer_id, DAddr device_addr, u64 size);

    /// Downloads the GPU-written data of several buffers with one wait for the GPU.
    void DownloadBuffersMemory(std::span<const BufferId> buffer_ids);

    void DeleteBuffer(BufferId buffer_id, bool do_not_mark = false);

    [[nodiscard]] Binding StorageBufferBinding(GPUVAddr ssbo_addr, u32 cbuf_index,
                                               bool is_written, bool native_pointer = false,
                                               bool mapping_sized = false,
                                               bool table_pointer = false) const;

    /// StorageBufferBinding's derivation from an already read descriptor (address, next qword).
    [[nodiscard]] Binding StorageBufferBindingFromDescriptor(GPUVAddr gpu_addr, u64 next_qword,
                                                             u32 cbuf_index, bool is_written,
                                                             bool native_pointer,
                                                             bool mapping_sized,
                                                             bool table_pointer = false) const;

    /// Per-slot memo of a storage buffer binding and of its buffer resolution.
    struct StorageBufferMemo {
        // Derivation key: the binding is a pure function of these while the page table holds.
        // The channel is part of it only to drop memos on a channel switch (conservative).
        const void* channel = nullptr;
        const Tegra::MemoryManager* gpu_memory = nullptr;
        u64 generation = 0; ///< Odd layout generation the binding was derived under (0: empty)
        GPUVAddr ssbo_addr = 0;
        u64 descriptor[2]{};
        u32 flags = 0;
        Binding binding{}; ///< Unresolved (buffer_id empty), as StorageBufferBinding returns
        // Resolution memo: FindBuffer result for (device_addr, size), valid while no buffer was
        // deleted anywhere (deletion_epoch) and the layout holds (no multi-range change).
        const Tegra::MemoryManager* resolved_gpu_memory = nullptr;
        u64 resolved_generation = 0;
        u64 resolved_epoch = 0;
        DAddr resolved_device_addr = 0;
        GPUVAddr resolved_gpu_addr = 0;
        u32 resolved_size = 0;
        bool resolved_is_written = false;
        BufferId resolved_buffer_id{};
    };

    /// StorageBufferBinding through a memo (EDEN_NX2_SSBO_MEMO).
    [[nodiscard]] Binding MemoStorageBufferBinding(StorageBufferMemo& memo, GPUVAddr ssbo_addr,
                                                   u32 cbuf_index, bool is_written,
                                                   bool native_pointer = false,
                                                   bool mapping_sized = false,
                                                   bool table_pointer = false);

    /// Resolves a storage binding's buffer (FindBuffer + multi-range), reusing the memo's last
    /// single-range result when nothing it depends on changed.
    void ResolveStorageBinding(Binding& binding, bool is_written, StorageBufferMemo& memo,
                               std::vector<MultiRangeSegment>& pool);

    [[nodiscard]] TextureBufferBinding GetTextureBufferBinding(GPUVAddr gpu_addr, u32 size,
                                                               PixelFormat format);

    [[nodiscard]] std::span<const u8> ImmediateBufferWithData(DAddr device_addr, size_t size);

    [[nodiscard]] std::span<u8> ImmediateBuffer(size_t wanted_capacity);

    [[nodiscard]] bool HasFastUniformBufferBound(size_t stage, u32 binding_index) const noexcept;

    [[nodiscard]] Binding& VertexBufferSlot(u32 index);

    [[nodiscard]] const Binding& VertexBufferSlot(u32 index) const;

    void UpdateVertexBufferSlot(u32 index, const Binding& binding);

    /// Flags the vertex buffers overlapping a written range so host copies derived from them are
    /// rebuilt.
    void MarkVertexBuffersStale(DAddr device_addr, u64 size);

    /// Re-dirties repacked vertex buffers whose source data was written since they were bound.
    void RefreshRepackedVertexBuffers();

    void ClearDownload(DAddr base_addr, u64 size);

    /// CPU writes collected but not yet applied (SetPendingCpuWrites).
    [[nodiscard]] Common::RangeSet<DAddr> PendingCpuWriteRanges();

    /// Writes downloaded bytes to guest memory, except over `cpu_written`.
    void WriteDownloadedMemory(const Common::RangeSet<DAddr>& cpu_written, DAddr device_addr,
                               const u8* data, u64 size);

    void InlineMemoryImplementation(DAddr dest_address, size_t copy_size,
                                    std::span<const u8> inlined_buffer);

    Tegra::MaxwellDeviceMemoryManager& device_memory;

    // Stable storage: resolver threads hold references across inserts (docs/mt-plan.md).
    MT::StableSlotVector<Buffer> slot_buffers;
    /// Bumped when a buffer goes away (DeleteBuffer, including the joins of JoinOverlap).
    std::atomic<u64> buffer_epoch{0};
    /// The epoch manager slot erasure is deferred through (MtIsRegionRegistered's readers).
    std::atomic<MT::EpochManager*> mt_epochs{nullptr};
#ifdef YUZU_LEGACY
    static constexpr size_t TICKS_TO_DESTROY = 6;
#else
    static constexpr size_t TICKS_TO_DESTROY = 8;
#endif
    DelayedDestructionRing<Buffer, TICKS_TO_DESTROY> delayed_destruction_ring;

    const Tegra::Engines::Maxwell3D::DrawManager::IndirectParams* current_draw_indirect{};

    u32 last_index_count = 0;

    u32 enabled_vertex_buffers_mask = 0;
    u32 stale_vertex_buffers_mask = 0;
    u64 vertex_buffers_serial = 0;
    std::array<Binding, 32> v_buffer{};

    boost::container::small_vector<BufferCopy, 4> upload_copies;

    MemoryTracker memory_tracker;
    /// Guards memory_tracker and gpu_modified_ranges once the sequencer, CPU write traps and the
    /// committer share them (EDEN_NX2_MT_TRAPLOCK, mt/tracker_lock.h; unused otherwise). Taken
    /// after `mutex`, never before it.
    Common::SpinRecursiveMutex tracker_mutex{"tracker"};
    /// Vertex buffer stale marks of writes that only took tracker_mutex (MtTrapWrite), applied
    /// by the next RefreshRepackedVertexBuffers. Guarded by tracker_mutex.
    std::vector<std::pair<DAddr, u64>> mt_stale_ranges;
    std::atomic<bool> mt_stale_pending{};
    /// EDEN_NX2_MT_LITE_CTRACK: mt_stale_ranges swapped out under the lock, applied after it
    /// (committer only).
    std::vector<std::pair<DAddr, u64>> mt_stale_scratch;
    /// EDEN_NX2_MT_LITE_CTRACK: MarkWrittenBuffer's tracker halves deferred by an open
    /// MtWrittenBatch (committer only, like the bind passes that fill it).
    struct MtWrittenRange {
        BufferId buffer_id;
        DAddr device_addr;
        u32 size;
    };
    boost::container::small_vector<MtWrittenRange, 16> mt_written_batch;
    u32 mt_written_batch_depth = 0;
    Common::RangeSet<DAddr> uncommitted_gpu_modified_ranges;
    /// Set (by the GPU thread) whenever downloads are queued for the next fence; cleared once a
    /// commit leaves none. Fences check it before taking the contended cache lock.
    std::atomic<bool> nx2_flush_hint{true};

    /// Whether GPU-written buffer ranges are copied back to guest memory at the next fence;
    /// otherwise only when something flushes them (as upstream yuzu below High GPU accuracy).
    /// Not for Switch 2 titles: title 1 had 250-330 MB/s copied back (a fence per compute
    /// dispatch, the fencing thread holding the buffer cache while it wrote them), while its CPU
    /// read GPU-written memory at most 64 times in 200 s (counted with reactive flushing) and
    /// the native MME's DMA reads never touched those pages. Without them a fence costs ~1 us
    /// instead of ~5 and draw setup ~7% less. EDEN_NX2_BUFFER_DOWNLOADS=0/1 forces it.
    bool AsyncBufferDownloads() const {
        static const int forced = [] {
            const char* value = std::getenv("EDEN_NX2_BUFFER_DOWNLOADS");
            return value == nullptr ? -1 : (value[0] == '0' ? 0 : 1);
        }();
        if (forced >= 0) {
            return forced == 1;
        }
        return gpu_memory == nullptr || !gpu_memory->UsesNativeTiling();
    }

    /// Whether a GPU write to this range is copied back at the next fence. Ranges the guest CPU
    /// has read (Preflushable, set by the first reactive flush) always are: later reads skip the
    /// flush and rely on these downloads, e.g. collision meshes skinned by compute shaders.
    bool DownloadsAtFence(DAddr device_addr, u64 size) noexcept {
        MT::TrackerScope tracker_scope{tracker_mutex};
        return AsyncBufferDownloads() ||
               (Tegra::g_nx2_preflush_downloads.load(std::memory_order_relaxed) &&
                memory_tracker.IsRegionPreflushable(device_addr, size));
    }

    /// A GPU write to the next fence's downloads, or read traps for it. Title 1's compute
    /// dispatches rewrite buffers of several MiB of which the CPU reads a few pages (collision
    /// data): with the whole write downloaded once any page was Preflushable, 9-17 GB/s came back
    /// and the GPU thread drew a third as many draws. Only the Preflushable pages are downloaded;
    /// the rest of the write keeps its read traps.
    void QueueFenceDownload(DAddr device_addr, u64 size) {
        MT::TrackerScope tracker_scope{tracker_mutex};
        if (AsyncBufferDownloads()) {
            nx2_flush_hint.store(true, std::memory_order_relaxed);
            uncommitted_gpu_modified_ranges.Add(device_addr, size);
            return;
        }
        if (!Tegra::g_nx2_preflush_downloads.load(std::memory_order_relaxed)) {
            ArmReadTraps(device_addr, size);
            return;
        }
        if (!Tegra::g_nx2_preflush_pages_only.load(std::memory_order_relaxed)) {
            if (memory_tracker.IsRegionPreflushable(device_addr, size)) {
                nx2_flush_hint.store(true, std::memory_order_relaxed);
                uncommitted_gpu_modified_ranges.Add(device_addr, size);
            } else {
                ArmReadTraps(device_addr, size);
            }
            return;
        }
        bool whole = false;
        memory_tracker.ForEachPreflushableRange(device_addr, size, [&](u64 begin, u64 length) {
            const DAddr lower = (std::max<DAddr>)(begin, device_addr);
            const DAddr upper = (std::min<DAddr>)(begin + length, device_addr + size);
            if (lower < upper) {
                nx2_flush_hint.store(true, std::memory_order_relaxed);
                uncommitted_gpu_modified_ranges.Add(lower, upper - lower);
                whole |= lower == device_addr && upper == device_addr + size;
            }
        });
        if (!whole) {
            ArmReadTraps(device_addr, size);
        }
    }

    /// Whether the range holds GPU-written data that no fence downloads (not Preflushable).
    bool HasGpuWritesNotDownloadedAtFence(DAddr device_addr, u64 size) {
        if (AsyncBufferDownloads()) {
            return false;
        }
        bool found = false;
        memory_tracker.ForEachDownloadRange(
            device_addr, size, false, [&](u64 start, u64 range_size) {
                if (found) {
                    return;
                }
                u64 covered = 0;
                memory_tracker.ForEachPreflushableRange(
                    start, range_size, [&](u64 lower, u64 length) {
                        const u64 begin = (std::max)(lower, start);
                        const u64 end = (std::min)(lower + length, start + range_size);
                        covered += end > begin ? end - begin : 0;
                    });
                found = covered < range_size;
            });
        return found;
    }

    /// A GPU write: queues its Preflushable parts for download at the next fence and traps reads
    /// of the rest. Queuing the whole range when any page of it was Preflushable copied whole
    /// written storage buffers (up to 8 MiB) back at every fence: ~5 GB/s in title 1's forest.
    void QueueGpuWrite(DAddr device_addr, u64 size) {
        if (AsyncBufferDownloads()) {
            uncommitted_gpu_modified_ranges.Add(device_addr, size);
            return;
        }
        const DAddr end = device_addr + size;
        DAddr cursor = device_addr;
        memory_tracker.ForEachPreflushableRange(
            device_addr, size, [&](DAddr start, u64 range_size) {
                const DAddr lower = (std::max)(start, cursor);
                const DAddr upper = (std::min)(start + range_size, end);
                if (lower >= upper) {
                    return;
                }
                if (lower > cursor) {
                    ArmReadTraps(cursor, lower - cursor);
                }
                uncommitted_gpu_modified_ranges.Add(lower, upper - lower);
                cursor = upper;
            });
        if (cursor < end) {
            ArmReadTraps(cursor, end - cursor);
        }
    }

    /// GPU writes that are not downloaded at fences: trap the guest CPU's next read of their
    /// pages, which flushes them once and marks them Preflushable (then DownloadsAtFence).
    void ArmReadTraps(DAddr device_addr, u64 size) {
        const DAddr begin = Common::AlignDown(device_addr, Core::DEVICE_PAGESIZE);
        const DAddr end = Common::AlignUp(device_addr + size, Core::DEVICE_PAGESIZE);
        u64 armed_bytes = 0;
        read_trapped_ranges.ForEachInRange(
            begin, end - begin, [&](DAddr lower, DAddr upper) { armed_bytes += upper - lower; });
        if (armed_bytes == end - begin) {
            return;
        }
        read_trapped_ranges.Add(begin, end - begin);
        device_memory.ArmReadTraps(begin, end - begin);
    }

    Common::RangeSet<DAddr> read_trapped_ranges;
    u64 preflush_decay_frames{};
    Common::RangeSet<DAddr> gpu_modified_ranges;
    std::deque<Common::RangeSet<DAddr>> committed_gpu_modified_ranges;

    // Async Buffers
    std::deque<std::optional<Async_Buffer>> async_buffers;
    std::deque<boost::container::small_vector<BufferCopy, 4>> pending_downloads;
    /// For each entry of pending_downloads: the ranges it still writes to guest memory when its
    /// fence pops. A write to a range after a download was taken removes it from that download
    /// (and every other pending one): its bytes are older than the write.
    std::deque<Common::RangeSet<DAddr>> pending_download_ranges;
    PendingCpuWrites pending_cpu_writes;
    std::optional<Async_Buffer> current_buffer;

    std::deque<Async_Buffer> async_buffers_death_ring;

    size_t immediate_buffer_capacity = 0;
    Common::ScratchBuffer<u8> immediate_buffer_alloc;
    /// Guest memory large uploads came from since the last cold hint (device address, size).
    std::vector<std::pair<DAddr, u64>> uploaded_buffer_ranges;

    struct LRUItemParams {
        using ObjectType = BufferId;
        using TickType = u64;
    };
    Common::LeastRecentlyUsedCache<LRUItemParams> lru_cache;
    VirtualRangeCache virtual_ranges;
    std::vector<MultiRangeSegment> graphics_segments;
    std::vector<MultiRangeSegment> compute_segments;
    u64 frame_tick = 0;
    u64 total_used_memory = 0;
    u64 buffer_bytes = 0; ///< The buffers' own size (total_used_memory may be the device's)
    u64 minimum_memory = 0;
    u64 critical_memory = 0;
    BufferId inline_buffer_id;

    /// Bumped whenever a buffer slot is freed. DeleteBuffer only clears the current channel's
    /// bindings, so memos holding a BufferId (which slots reuse) require this to be unchanged.
    u64 buffer_deletion_epoch = 1;
    /// EDEN_NX2_C12_COMMIT_REPEAT: bumped by every page_table change (ChangeRegister: each buffer
    /// creation, each deletion) under the buffer cache mutex. The c12 memos also require the
    /// deletion epoch. CPU writes, uploads and GPU writes (MarkWrittenBuffer) do not change what
    /// FindBuffer returns (without accurate GPU fences, where the memos are off) and on the
    /// committer SynchronizeBuffer is a no-op (the packet carries the bytes), so they do not bump
    /// it: the per-stage binds that depend on them (BindHostStageBuffers) run every draw.
    u64 c12_buffer_epoch = 1;
    /// EDEN_NX2_C12_UBIND_MEMO, per graphics uniform slot: the last bind's translation (gpu
    /// address -> device address under one odd layout generation of one memory manager) and the
    /// buffer UpdateUniformBuffers resolved for (device address, size) under both epochs.
    struct C12UniformBindMemo {
        const void* channel = nullptr;
        const Tegra::MemoryManager* gpu_memory = nullptr;
        u64 generation = 0; ///< 0: no translation kept
        GPUVAddr gpu_addr = 0;
        DAddr device_addr = 0;
        const void* resolved_channel = nullptr;
        u64 resolved_deletion_epoch = 0;
        u64 resolved_c12_epoch = 0;
        DAddr resolved_device_addr = 0;
        u32 resolved_size = 0;
        BufferId resolved_buffer_id{}; ///< empty: no resolution kept
    };
    std::array<std::array<C12UniformBindMemo, NUM_GRAPHICS_UNIFORM_BUFFERS>, NUM_STAGES>
        c12_ubind_memo{};
    /// EDEN_NX2_C13_MEMO_WAYS (cycle 13 W4 M3, mt/c13_memo.h): the uniform bind memo's other
    /// ways per slot (c12_ubind_memo is way 0, the most recent), most recent first.
    std::array<std::array<std::array<C12UniformBindMemo, MT::kC13MaxWays - 1>,
                          NUM_GRAPHICS_UNIFORM_BUFFERS>,
               NUM_STAGES>
        c13_ubind_ways{};

    /// C12UpdateGraphicsBuffers' pieces.
    [[nodiscard]] bool C12SameBindings(const C12GraphicsBufferMemo& memo) const;
    void C12ApplyBindings(const C12GraphicsBufferMemo& memo);
    void C12SaveGraphicsBuffers(C12GraphicsBufferMemo& memo, u64 generation);
    void C12CheckGraphicsBuffers(C12GraphicsBufferMemo& memo, bool is_indexed, u64 generation);
    std::array<std::array<StorageBufferMemo, NUM_STORAGE_BUFFERS>, NUM_STAGES> graphics_ssbo_memo{};
    std::array<StorageBufferMemo, NUM_COMPUTE_STORAGE_BUFFERS> compute_ssbo_memo{};
    /// Index buffer memo (EDEN_NX2_INDEX_MEMO): FindBuffer(device_addr, size) returned buffer_id
    /// for size <= resolved_size; while the epoch and layout hold, any smaller size from the same
    /// start resolves to the same buffer (it is still the one in page_table and covers it).
    struct IndexBufferMemo {
        const void* channel = nullptr;
        const Tegra::MemoryManager* gpu_memory = nullptr;
        u64 generation = 0;
        u64 epoch = 0;
        GPUVAddr gpu_addr = 0;
        DAddr device_addr = 0;
        u32 size = 0;
        BufferId buffer_id{};
    } index_buffer_memo{};
    /// Vertex stream memo (EDEN_NX2_VERTEX_MEMO): direct mapped on the stream's GPU start. The
    /// (start, end) range resolved to (device_addr, size, buffer_id) under one odd layout
    /// generation of one memory manager and one buffer deletion epoch.
    struct VertexBufferMemo {
        u64 gpu_memory_id = ~u64{0};
        u64 generation = 0;
        u64 epoch = 0;
        GPUVAddr gpu_begin = 0;
        GPUVAddr gpu_end = 0;
        DAddr device_addr = 0;
        u32 size = 0;
        BufferId buffer_id{};
    };
    static constexpr size_t VERTEX_MEMO_ENTRIES = 1024;
    std::array<VertexBufferMemo, VERTEX_MEMO_ENTRIES> vertex_buffer_memo{};
#ifdef YUZU_LEGACY
    bool immediately_free = false;
#endif

    // Covers the device address space (MaxwellDeviceTraits::device_virtual_bits).
    std::array<BufferId, ((1ULL << 36) >> CACHING_PAGEBITS)> page_table;
    Common::ScratchBuffer<u8> tmp_buffer;

    /// A command list page (Nx2CopyListPages): the GPU writes noted to it and the last copy.
    using Nx2Staging = decltype(std::declval<Runtime&>().DownloadStagingBuffer(size_t{}, true));
    struct Nx2ListPage {
        u64 writes{};      ///< GPU writes noted to the page
        u64 copy_writes{}; ///< `writes` when the copy was made: the copy is current while equal
        u64 copy_tick{};   ///< GPU tick that completes the copy
        /// `writes` when the front last flushed the page: guest memory holds it while equal
        u64 synced_writes{~u64{0}};
        u32 copy_begin{}; ///< Bytes of the page the copy holds (a buffer can end inside it)
        u32 copy_end{};
        /// Written outside a compute dispatch or a DMA copy (a draw's storage writes) at least
        /// once: the front can't tell from what it sent whether such a write is still queued, so
        /// the page's lists always drain the split instead of reading a copy.
        bool draw_written{};
        std::optional<Nx2Staging> copy;
    };
    std::mutex nx2_list_mutex; ///< nx2_list_pages: the split's front reads the copies
    std::unordered_map<DAddr, Nx2ListPage> nx2_list_pages;
    /// Copies replaced while the GPU may still write them: freed once it is past their tick.
    std::vector<std::pair<u64, Nx2Staging>> nx2_retired_list_copies;
    boost::container::small_vector<std::pair<BufferId, DAddr>, 8> nx2_list_copy_requests;
    bool nx2_collect_list_copies{};
};

} // namespace VideoCommon
