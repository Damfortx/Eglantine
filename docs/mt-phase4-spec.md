# MT phase 4 — sequencer capture + committer thread, K=0 (implementation spec)

Drives the implementation of phase 4 of `docs/mt-plan.md`: `EDEN_NX2_MT=1`,
`EDEN_NX2_MT_RESOLVERS=0`, `EDEN_NX2_MT_COMPUTE=serial|packet` (default `serial`),
`EDEN_NX2_MT_LOCKSTEP`, `EDEN_NX2_MT_VERIFY`. With `EDEN_NX2_MT` unset every path below must be
byte-for-byte today's path (all new code behind `MT::Active()` checks).

Snapshot: HEAD `77e46bcb97` plus the in-progress `src/video_core/mt/` primitives
(`draw_packet.h`, `capture_plan.h`, `spmc_stream.h`, `engine_ptr.h`, `reg_delta.h`, `arena.h`,
`page_counter.h`, `mt_switches.h`, ...). `gpu_split.cpp` is being edited concurrently (it already
moved +6 lines: the selftest hook in `MaybeStart`); line numbers below are those of this snapshot,
use the function names as anchors. Paths are relative to `src/video_core/`.

Legend used everywhere below:

| Tag | Meaning |
|---|---|
| **SEQ** | sequencer-local: runs inline on the sequencer (GPUBack) against the real engines |
| **CMD** | Command: async, ordered; the sequencer captures a payload, the committer runs it in stream order |
| **SER** | Serial: the sequencer closes the batch, emits the entry and blocks until the committer ran it (old code, against the mirror, with the sequencer quiescent) |
| **PKT** | part of a DrawPacket / ComputePacket (capture on SEQ, resolve+record on committer) |
| **XREQ** | called by a thread that is neither sequencer nor committer (front, CPU cores, services, fencing thread); stays as today (own locks), or posts to the committer |
| **ANY** | thread-safe as written (const queries, own mutex) |

Thread roles (new `MT::Role` thread-local, see §6.1): Front (`GPU`), Sequencer (`GPUBack`,
`GpuSplit::BackLoop`), Committer (`GPUCommit`, new), fencing thread (`FenceManager::fence_thread`),
`VulkanWorker`, CPU core / service threads, async present thread.

Phase-4 rule of thumb (K=0): the committer does what the resolver will do later, under
`texture_cache.mutex` + `buffer_cache.mutex` (`ResolveDraw<ExclusiveAccess>`). The sequencer never
records, never touches the Scheduler / StagingBufferPool / DescriptorQueues / FenceManager /
QueryCache / texture structure, never calls `BufferCache::FindBuffer`. Guest memory is read only by
the sequencer except: committer texture uploads/image creation (I5), Serial items (sequencer
blocked, so memory equals the stream point), downloads and fence callbacks (guest writes).

---

## 1. Entry points and their classification

### 1.1 `VideoCore::RasterizerInterface` / `Vulkan::RasterizerVulkan`

Declarations `rasterizer_interface.h:46-253`, overrides `renderer_vulkan/vk_rasterizer.h:89-168`,
bodies `renderer_vulkan/vk_rasterizer.cpp`. Every override gets the dispatch prologue

```cpp
if (MT::OnSequencer()) [[unlikely]] { return mt.Route<...>(...); }   // routes per table below
// else: old body (committer, Serial on committer, other threads, MT off)
```

so Serial items and Commands run the *old* body on the committer unchanged.

| Entry (vk_rasterizer.cpp) | Called by (thread today) | MT class | Payload / notes |
|---|---|---|---|
| `Draw(is_indexed, instances)` 590-762 | `DrawManager::ProcessDraw` draw_manager.cpp:321 (back) | **PKT** (SER when forced slow, §2.3) | DrawPacket (§2). Pre-checks 594-610 (16M vertex guard), all debug traces 611-719 (`TraceWindowTarget`, CB24, TRACE_VS, SKIP_VS, RT_TRACE: read guest memory and regs) stay SEQ before capture. `gpu.TickWork()` in `PrepareDraw`'s SCOPE_EXIT 335-337 → SEQ check `GPU::HasSyncRequests()` (new, atomic `last_sync_fence != current_sync_fence`) → CMD `TickWork` |
| `DrawIndirect()` 764-838 | draw_manager.cpp:330 (back) | **SER** | old body; reads `draw_manager.indirect_state` (copied into mirror by the Serial prologue, §3.3) |
| `DrawTexture()` 840-885 | draw_manager.cpp:287 (back) | **SER** | reads `draw_texture_state`, TIC/TSC via `GetImageView(u32)`/`GetSampler(u32)` (guest reads) |
| `Clear(layer_count)` 887-1079 | draw_manager.cpp:71 (back) | **CMD** `Clear{layer_count}` | SEQ part: trace 889-896, `FlushWork` moves to committer, `gpu_memory->FlushCaching()` 898 stays SEQ; the `!use_color&&!use_depth&&!use_stencil` early return 905-907 may be decided on SEQ (skip emit). Committer runs 909-1078 against the mirror (regs: clear_surface, clear_color, clear_depth, clear_stencil, stencil_front_mask, stencil_front_func_mask, clear_control, scissor_test[0], surface_clip, window_origin, rt[n].format, zpass_pixel_count_enable) |
| `DispatchCompute()` 1091-1203 | kepler_compute.cpp:606 (`DispatchNativeCompute`), :1138 (`ProcessLaunch`) (back) | **SER** (`EDEN_NX2_MT_COMPUTE=serial`, default) / **PKT** ComputePacket (`=packet`, §2.6) | throws `Shader::RuntimeError` in native mode 1106/1117 and the exception decides `dispatched` in `DispatchNativeCompute` 598-617 → only SER is exact |
| `ResetCounter(type)` 1205 | maxwell_3d.cpp:750 (back) | **CMD** `ResetCounter{type}` | query cache |
| `Query(addr,type,flags,payload,sub)` 1219 | maxwell_3d.cpp:661,669; kepler_compute.cpp:124,427; maxwell_dma.cpp:387,391; puller.cpp:451,453,565,595 (back, puller via `OnBack`) | **CMD** `Query{gpu_addr, type, flags, payload, subreport}`; immediate Payload sub-case **SEQ** (see §7 R7) | `QueryCacheBase::CounterReport` query_cache/query_cache.h:234-346 |
| `BindGraphicsUniformBuffer(stage,i,addr,size)` 1224 | `Maxwell3D::ProcessCBBind` maxwell_3d.cpp:787 (back) | **SEQ** (seq CB table) + **CbBind delta** | `CbBindDelta{stage,slot,enabled=1,size,gpu_addr}` + **add `u64 device_addr`** (translated on SEQ); committer calls new `BufferCache::BindGraphicsUniformBufferTranslated` |
| `DisableGraphicsUniformBuffer(stage,i)` 1229 | maxwell_3d.cpp:782 | **SEQ** + CbBind delta `enabled=0` | |
| `FlushAll()` 1233 | — | ANY (empty) | |
| `FlushRegion(addr,size,which)` 1258 | `MemoryManager::FlushRegion` memory_manager.cpp:553-571 (from `ReadBlock` safe 439/452, `FlushRegion` 561/565), `RunCommand`/`FlushRegionCommand` deferred thunks gpu_thread.cpp:150-153, `GpuSplit::Run` thunks (puller.cpp:95, dma_pusher.cpp:821, maxwell_3d.cpp:1417/1449), sync requests gpu.cpp:341-345/357-358 (TickWork), kepler_compute.cpp:1031/1052/1103 (debug), nx2_gsv.h:2208 | on SEQ: **SER**; on committer (TickWork/Serial): old body; CPU threads never call it directly | downloads record + submit |
| `ReadGpuWrittenCommandList` 1241 | dma_pusher.cpp:789 (front) | **XREQ/ANY** | `nx2_list_mutex` + `WaitSubmittedTick` (→ committer, §5) |
| `NoteGpuWrittenCommandListSynced` 1254 | dma_pusher.cpp:833 (front, after a drain) | XREQ (mutex) | |
| `MustFlushRegion` 1313 | `MemoryManager::IsMemoryDirty` memory_manager.cpp:574-596 (front: dma_pusher.cpp:819, maxwell_3d.cpp:1415/1444; SEQ: deferred `current_dirty` thunk dma_pusher.cpp:186-188), puller.cpp:91 (front), puller.cpp:325 (front, log), CPU threads | ANY (takes buffer/texture mutex) | sees capture-time GPU-modified state (§3.4) — conservative, correct |
| `GetFlushArea` 1333 | `GPU::OnCPURead` gpu.cpp:332 (CPU threads) | XREQ (mutexes) | |
| `InvalidateRegion(addr,size,which)` 1356 | `MemoryManager::InvalidateRegion` 778-790 (`WriteBlock` safe 515/529: DMA, Fermi2D software paths, `AccelerateInlineToMemory` unmapped case 1732), legacy `query_cache.h:331,336` (GL) | on SEQ: **SER** if `Nx2MayHoldTexture` or `Nx2AnyPage(g_nx2_query_pages)` over the range, else SEQ (buffer `WriteMemory` + shader cache, both locked); other threads: as today | texture part is the I5 barrier |
| `InnerInvalidation(seqs)` 1376 | `MemoryManager::FlushCaching` memory_manager.cpp:1001 (SEQ) | **SEQ** | under MT: buffer part + `pipeline_cache.InvalidateRegions` inline; texture part only for ranges with `Nx2MayHoldTexture` and only after the I5 barrier was taken at write time (§3.5), so here `WaitCommitted()` + inline `texture_cache.WriteMemory`; query part → CMD `InvalidateQueryRegion{addr,size}` per range with query pages |
| `OnCPUWrite` 1417 | `GPU::OnCPUWrite` gpu.cpp:366 ← core/memory.cpp:901 (CPU traps) | XREQ (locks) | unchanged; buffer `OnCPUWrite` only marks tracker / returns GPU-modified |
| `OnCacheInvalidation` 1479 | gpu_thread.cpp:211,225 (CPU threads: `InvalidateRegion`, `FlushAndInvalidateRegion`), gpu.cpp:208/361 (sync requests), `InvalidateRegionCommand` thunk gpu_thread.cpp:154-157 (SEQ) | XREQ (locks); on SEQ: SEQ | |
| `OnCacheInvalidations` 1495 | `Impl::InvalidateGPUCache` gpu.cpp:227 | ANY (locks) | reached from SEQ (`InvalidateGPUCache`) and committer (`FenceManager::SignalFence` fence_manager.h:96,131) |
| `TryCacheInvalidation` 1528 | `Impl::InvalidateRegion` gpu.cpp:358 (CPU) | XREQ (locks) | falls back to `RequestSyncOperation` (→ CMD TickWork) |
| `InvalidateGPUCache` 1544 | maxwell_3d.cpp:461, puller.cpp:700 (`OnBack`), fence_manager.h:96,131 (committer) | **SEQ** when on SEQ (gathers CPU writes now; locks), old body on committer | not CMD: the gather must not lag behind later captures |
| `UnmapMemory` 1548 | `MemoryManager::Unmap` memory_manager.cpp:256 ← `gmmu->Unmap` in `RunInOrder` closures (nvhost_as_gpu.cpp:192,242) | **SER** (the enclosing `DeferFunction` is Serial, §1.4) | deletes images (`TextureCache::DeleteImage` dirties RT flags on `maxwell3d` → must resolve to the committer mirror, §4) |
| `ModifyGPUMemory` 1560 | memory_manager.cpp:138,177 (Map/MapSparse) | **SER** (enclosing closure) | |
| `SignalFence(func)` 1568 | dma_pusher.cpp:332 (`Run`), kepler_compute.cpp:110,392,412 (SEQ), query_cache/query_cache.h:308 (committer) | on SEQ: **CMD** `SignalFence{std::function<void()>* heap}` (moved into a heap cell, deleted by the committer); the trailing `rasterizer.InvalidateGPUCache()` (fence_manager.h:96,131) stays inside the committer's old body | closures write guest memory (semaphore add/max) on the committer — guest-observable, ordered (I6) |
| `SyncOperation(func)` 1572 | vk_query_cache.cpp:206,335,543 (committer) | old body (committer only) | if ever reached on SEQ: CMD |
| `SignalSyncPoint(v)` 1576 | maxwell_3d.cpp:759, puller.cpp:413 (`OnBack`) | **CMD** `SignalSyncPoint{v}` | `IncrementGuest` (fence_manager.h:149) runs on committer; composite may run in the action (gpu.cpp:401-430) → committer, correct (it records) |
| `SignalReference()` 1580 | puller.cpp:678 | **CMD** | |
| `ReleaseFences(force)` 1584 | puller.cpp:121 (`Run`), 409 (`OnBack`), gpu.cpp:233 (`OnCommandListEnd`), query_cache/query_cache.h:610 | **CMD** `ReleaseFences{force}` | front's `Run` then `Drain`s on `settled` (§5) |
| `FlushAndInvalidateRegion` 1588 | — (via CPU paths of GPU::) | on SEQ: **SER**; else XREQ | |
| `WaitForIdle()` 1596 | maxwell_3d.cpp:399,462; puller.cpp:684 | **CMD** | records barrier + `query_cache.NotifyWFI` + `fence_manager.SignalOrdering` |
| `FragmentBarrier()` 1632 | maxwell_3d.cpp:459 | **CMD** | |
| `TiledCacheBarrier()` 1637 | maxwell_3d.cpp:464 | SEQ (no-op; emit nothing) | |
| `FlushCommands()` 1641 | `Impl::FlushCommands` gpu.cpp:113 ← dma_pusher.cpp:106-110 (deferred), fence_manager.h:124 (committer) | **CMD** (the dma_pusher thunk becomes CMD `FlushCommandsAndListEnd`) | uses `draw_counter` (committer-owned) |
| `TickFrame()` 1653 | renderer_vulkan.cpp:254 (inside `Composite`, committer under MT) | committer only | `pipeline_cache.TickFrame()` is the exception: eviction mutates sequencer-owned `graphics_cache` → split, §1.3 |
| `AccelerateConditionalRendering()` 1671 | `Maxwell3D::ProcessQueryCondition` maxwell_3d.cpp:681 (SEQ) | **SER** returning `bool` | `FlushCaching` part stays SEQ before the Serial; query cache + HCR recording on committer |
| `HasDrawTransformFeedback()` 1676 | engines (any) | ANY | |
| `AccelerateSurfaceCopy` 1680 | fermi_2d.cpp:158 (SEQ), kepler_compute.cpp:1099 (SEQ) | **SER** returning `bool` | on `false` Fermi2D falls back to software (SEQ guest R/W, after the Serial: exact) |
| `UsesNvidiaDlss`, `SupportsGameDlssNetwork`, `CanProbeGameDlssOutput` 1687-1694, h:143 | kepler_compute.cpp:654,661,685 | ANY | |
| `ProbeGameDlssOutput` 1696 | kepler_compute.cpp:734 | **SER** returning `optional` | |
| `AccelerateTemporalUpscale` 1702 | kepler_compute.cpp:1094 | **SER** returning `bool` | |
| `AccessAccelerateDMA()` 1710 → `AccelerateDMA::BufferCopy/BufferClear/ImageToBuffer/BufferToImage` 1879-1938 | maxwell_dma.cpp:126,204,277 (SEQ) | **SER** per call returning `bool` (phase 4; `EDEN_NX2_MT_DMA=packet` is phase 7) | `BufferCopy` collects + submits list copies 1886-1891 → committer, correct |
| `AccelerateInlineToMemory(addr,size,bytes)` 1714-1786 | engine_upload.cpp:46,59,77 (I2M), kepler_compute.cpp:309 (inline QMD), `TryNx2IndirectReset` 322 | **SEQ** guest write + routing (§3.5): I5 barrier if `Nx2MayHoldTexture`/command-list page; `gpu_written` page → `CaptureUploadRanges` + **CMD** `InlineUpload{device_addr,size,staging}`; else `Nx2NoteListHostWrite` + `AccumulateInvalidation` (SEQ) ; `query_cache.InvalidateRegion` 1771 → CMD `InvalidateQueryRegion` only when `Nx2AnyPage(g_nx2_query_pages)` | |
| `LoadDiskResources` 1821 | boot (before split) | ANY (pre-MT) | |
| `InitializeChannel(ch)` 2821 | `GPU::InitChannel` gpu.cpp:91-95 ← nvhost_gpu.cpp:226,259,419 (service thread) | XREQ as today (locks) + **mirror creation deferred to first `BindChannel`** | `state_tracker.SetupTables(channel)` writes the real engine's `dirty.tables`; the mirror copies them at ChannelSync |
| `BindChannel(ch)` 2833 | `Impl::BindChannel` gpu.cpp:77-85 ← control/scheduler.cpp:23-28 (deferred, SEQ) | **SER** + `ChannelSync` | committer runs the old body (binds all caches + `state_tracker.ChangeChannel` → mirror flags, §4.1), then (re)creates/copies the channel's mirrors; the sequencer keeps its own `ChannelSetupCaches` view (§3.1, ChannelSetupCaches row) |
| `ReleaseChannel` 2847 | (UNIMPLEMENTED upstream) | SER | |
| `AccelerateDisplay` 1788 | present/layer.cpp:95 (inside `Composite`) | committer only | |
| `LockCaches/UnlockCaches` vk_rasterizer.h:115-121 | kepler_compute.cpp:353,357 (SEQ, QMD signal) | **SEQ no-op under MT** | otherwise the sequencer would hold both locks across a QMD and stall the committer |
| private `PrepareDraw`, `TryNx2IndirectReset`, `FlushWork`, `SubmitNx2ListCopies`, `UpdateDynamicStates`, `HandleTransformFeedback`, `Update*` | internal | split per §2 | `UpdateDynamicStates` 2018 calls `pipeline_cache.CurrentGraphicsPipeline()` again: **replace by the packet's pipeline** (`commit_pipeline` member) — on the committer that call would mutate sequencer-owned `graphics_key/current_pipeline` |

### 1.2 Other GPU-level entry points that reach caches

| Entry | Caller (thread) | MT class |
|---|---|---|
| `Impl::TickWork` gpu.cpp:263-275 (runs `sync_requests`: `OnCPURead` flush 341-346, `InvalidateRegion(preserve)` 357-360, `RequestComposite` (no fences) 394-399, `GetAppletCaptureBuffer` 445-449, `WaitForIdle` 255-259) | `PrepareDraw` SCOPE_EXIT, `DrawTexture` 842-844 (SEQ), `GPUTickCommand` thunk gpu_thread.cpp:139 (SEQ) | **CMD** `TickWork` (committer pops the requests). The sequencer must never call `Impl::TickWork` under MT; it emits the CMD only when `HasSyncRequests()` |
| `Impl::OnCommandListEnd` gpu.cpp:232-235 | dma_pusher.cpp:106-110 thunk | **CMD** (with FlushCommands) |
| `Impl::InvalidateGPUCache` gpu.cpp:120-230 | via rasterizer | see table |
| `GPU::RequestComposite` → syncpoint guest action | runs on the thread doing `IncrementHost` = fence signal = committer (or fencing thread) | unchanged; `renderer->Composite` touches scheduler → must be committer/fencing thread, never SEQ (SEQ never signals fences under MT) |
| `ThreadManager` split routing gpu_thread.cpp:136-166: `SubmitList` (front parse), `GPUTickCommand` → Defer TickWork, `RunCommand` → `DeferFunction` (+`Drain` if `before_later_reads`), `FlushRegionCommand` → Defer `FlushRegion`, `InvalidateRegionCommand` → Defer `OnCacheInvalidation`, `SignalCommand` Defer | front | the deferred thunks run on SEQ and route per table; **`DeferFunction` thunks (generic closures: gmmu Map/Unmap, gsv `RunInOrder` event signals and `done` flags nx2_gsv.h:623,751,920,1383) → SER** (they may touch page tables + caches and some signal "GPU finished" which must mean committed) |
| `Scheduler::BindChannel` control/scheduler.cpp:20-32 | front → Defer | SER (`BindChannel`) |
| `GPU::InitChannel/InitAddressSpace/BindRenderer` | service/boot | XREQ (as today) |

### 1.3 Caches: entry points reachable from outside the rasterizer

| Cache entry | Caller | MT owner |
|---|---|---|
| `PipelineCache::CurrentGraphicsPipeline` vk_pipeline_cache.cpp:735-752 (`RefreshStages` shader_cache.cpp:101, `graphics_key.state.Refresh` fixed_pipeline_state.cpp:105, `Next`, `BuiltPipeline` 1288-1308) | PrepareDraw 375, UpdateDynamicStates 2018 | **SEQ** (fast path: hashes, key, `current_pipeline->Next`, `graphics_cache.find`). `CurrentGraphicsPipelineSlowPath` 1188-1211 on a *miss that creates* (`try_emplace` new, or `TakeBackgroundPipeline`/`CreateGraphicsPipeline`) → **SER** `CreateGraphicsPipeline` (sequencer blocked: environments read guest memory/regs at the draw point, `DescriptorPool::Allocator`, `descriptor_buffer_ring.CanAllocate`, `render_pass_cache` are touched). A hit in `graphics_cache` that only adds a transition is SEQ. |
| `PipelineCache::CurrentComputePipeline` 754-793 (+`NullPointerSlotFilled` 795-815 guest reads, `ShaderCache::ComputeShader` shader_cache.cpp:207-227 guest reads) | DispatchCompute 1104 | SER mode: committer (old body). Packet mode: **SEQ** lookup; create/rebuild → SER |
| `PipelineCache::IsIndirectResetShader` 726-733 | TryNx2IndirectReset 264 | SEQ (reads `shader_infos`, real regs) |
| `PipelineCache::TickFrame` 1213-1222 / `EvictUnusedPipelines` 1224-1265 | RasterizerVulkan::TickFrame 1655 | split: `++frame_count` and `MarkUsed`/`LastUse` are sequencer-owned → run `PipelineCache::TickFrame` as a **SEQ step requested by the committer** (committer sets `pipeline_tick_pending` atomic; SEQ runs it at its next packet boundary). Eviction never fires for Switch 2 titles (1235) and needs 9000 idle frames, so in-flight packets can't reference an evicted pipeline. `evicted_pipelines` freeing uses `scheduler.IsFree` (atomic master semaphore read: ANY) |
| `PipelineCache::InvalidateRegion(s)/OnCacheInvalidation` (ShaderCache, own `invalidation_mutex`) | CPU traps, SEQ FlushCaching, committer InvalidateGPUCache | ANY |
| `PipelineCache::LoadDiskResources` | boot | ANY |
| `TextureCache::*` (all) | rasterizer, `FenceManager` (`CommitAsyncFlushes`, `PopAsyncFlushes`, `ShouldWaitAsyncFlushes` fence_manager.h), present (`TryFindFramebufferImageView`), CPU traps (`WriteMemory`), DMA (`DmaImageId`, `DmaBufferImageCopy`), query runtime | committer (structure), CPU traps `WriteMemory` (locked, flag-only), SEQ only through the I5 barrier (`WaitCommitted` then locked `WriteMemory`) |
| `BufferCache::*` | rasterizer, fence manager (`AccumulateFlushes`, `CommitAsyncFlushes`, `PopAsyncFlushes`, `SignalOrdering`), query runtime (`ObtainBuffer`), CPU traps (`OnCPUWrite`, `WriteMemory`), front (`Nx2ReadListCopy`, `IsRegionGpuModified` via `MustFlushRegion`) | structure (slots, page_table, FindBuffer/CreateBuffer/DeleteBuffer, downloads, list copies, LRU) committer; tracker + `gpu_modified_ranges` shared under `buffer_cache.mutex` (= plan's `tracker_mutex` in phase 4); capture methods SEQ |
| `QueryCache` (`CounterEnable/Report/Reset`, `NotifySegment`, `NotifyWFI`, `AccelerateHostConditionalRendering`, `InvalidateRegion`, `FlushRegion`, async flushes) | rasterizer, fence manager | committer only |
| `StateTracker` vk_state_tracker.h | rasterizer `Update*`, `Scheduler::AllocateNewContext`/`InvalidateState` (vk_scheduler.cpp:319,334,489), `BlitImageHelper` | committer only; `flags` points at the committer mirror (§4.1) |
| `Scheduler` | everything recording | committer only (`OwnsScheduler`) |

---

## 2. Guest-memory reads reachable from the draw/compute paths, and how they are captured

### 2.1 Sequencer `CaptureDraw` order (replaces `PrepareDraw` on SEQ)

Mirrors `PrepareDraw` vk_rasterizer.cpp:332-506 + `ConfigureImpl` vk_graphics_pipeline.cpp:387-880
step by step. Locks: the sequencer takes **only `buffer_cache.mutex`** for steps 6-9 (one
acquisition per draw, same as the plan's `tracker_mutex`), never `texture_cache.mutex`.

1. `gpu_memory->FlushCaching()` (vk_rasterizer.cpp:360) → `InnerInvalidation` (SEQ variant, §1.1).
   `FlushWork()` (359) is **not** called here (committer).
2. `pipeline_cache.CurrentGraphicsPipeline()` (375) on the real engine: consumes the real
   engine's `Shaders`, `VertexInput` (non-dynamic only), `ViewportSwizzles`, `Blending`
   (non-EDS3) flags (§4.1). Null → `report_failure("no pipeline")`, `submitted=false`, return.
   Miss that creates → **SER** `CreateGraphicsPipeline`, then retry the lookup.
3. `if (!is_indexed && TryNx2IndirectReset())` (412) stays SEQ; its `gpu_memory->FlushRegion`
   (297) when `IsMemoryDirty` → SER (download); its `AccelerateInlineToMemory` calls are SEQ
   guest writes (§3.5).
4. `mesh_draw`, `EDEN_NX2_NO_MESH*` (417-431) SEQ.
5. Forced-slow checks (→ whole draw **SER**, old `PrepareDraw` on committer):
   `regs.transform_feedback_enabled`, `plan.flags & (UsesTextureBuffers|UsesImageBuffers)`,
   `current_draw_indirect` (only via DrawIndirect: already SER), `EDEN_NATIVE_DRAW_CAPTURE`
   active (465-505), any SSBO whose GPU range is not one contiguous device range
   (`!gpu_memory->IsContinuousRange(gpu_addr,size)` → multi-range path), any vertex/index range
   that `MaxContinuousRange` truncates differently from its device span, plan build failure.
6. Handles (plan order = `config_stage` order 420-511: per enabled stage 0..4: texture-buffer
   descs, image-buffer descs, texture descs, image descs; `count` elements each):
   - plain: `gpu_memory->Read<u32>(cbufs[cbuf_index].address + cbuf_offset + (index << size_shift))`
     (468) from the **real** `maxwell3d->state.shader_stages[stage].const_buffers`;
   - secondary: two reads 461-465 (`shift_left`, `secondary_shift_left`);
   - `handle_program != ~0U`: `VideoCommon::EvaluateTextureHandle` shader_handle_program.h:16-34
     (reads a u64 pointer from the cbuf, then u32 at pointer) — SEQ;
   - `TexturePair(raw, via_header_index)` with `via_header_index = regs.sampler_binding == ViaHeaderBinding` (415).
   Captured: `u32 handle` per view (`views[i].index`), `u32 sampler_handle` per texture.
7. TIC/TSC (replaces `DescriptorTable::Read` descriptor_table.h:60-87, called from
   `TextureCache::VisitImageView` 998-1028 and `GetSamplerId` 769-782; also
   `PeekUnchanged` 43-57 in `TryFillImageViewsClean` 578-615 and `TryReuseGraphicsBindings`
   618-697): table index = `Tegra::Texture::TextureHeaderIndex(handle, regs.tex_header.limit,
   gpu_memory->UsesNativeTiling())` (textures/texture.h:172); if `index > limit` capture
   "null"; else copy 32 bytes from `regs.tex_header.Address() + index*32` (same one-page
   `GetPointer` fast path as descriptor_table.h:70-76). Samplers: `index > tsc_limit` → null, else
   32 bytes at `regs.tex_sampler.Address() + index*32`, `tsc_limit = linked ? tic_limit :
   regs.tex_sampler.limit` (texture_cache.h:809-811). → `TicCapture{index, raw}`,
   `TscCapture{index, raw}`.
8. UBOs (replaces `BufferCache::BindHostGraphicsUniformBuffer` buffer_cache.h:1162-1270): for each
   enabled stage, each bit of `pipeline->enabled_uniform_buffer_masks[stage]`, in
   `ForEachEnabledBit` order, `CaptureUniform(stage, index, used_size)`:
   - bank-17 root-table hack 1166-1183: if `index == 17 && real.root_tables.loaded &&
     seq_cb[stage][17].size == 0` → stage `min(size, 256)` bytes of `real.root_tables.tables[0]`
     zero-padded to `size` → `UboCapture{Staged}`;
   - `dev = seq_cb[stage][index].device_addr`, `size = min(seq_cb.size, used_size)`;
     `has_host = dev != 0 && (dev >> CACHING_PAGEBITS) < page_table.size()` (FindBuffer never
     returns NULL otherwise, buffer_cache.h:2101-2106);
     `needs_align = has_host && dev % runtime.GetUniformBufferAlignment() != 0` (buffers start
     CACHING_PAGESIZE-aligned, so `buffer.Offset(dev) % a == dev % a`);
     `fast = needs_align || (has_host && size <= skip_cache_size && !memory_tracker.IsRegionGpuModified(dev,size))`
     (1202-1204; `skip_cache_size` read relaxed from `channel_state->uniform_buffer_skip_cache_size`,
     written by `TickFrame` 167 on the committer — the decision is made once, here);
   - fast → copy `size` bytes (`device_memory.ReadBlockUnsafe` / `GetSpan`, 1237-1238 / reuse memo
     uniform_stream_reuse.h `BindSmallUniformReusing`) into **SeqStagingRing** with the per
     (stage,index) memo `{bytes, ring ref, size}` reused while the ring allocation is live →
     `UboCapture{Staged, staging VkBuffer, staging device address, offset, size}`;
   - cached → `UboCapture{Cached, dev, size}` + `CaptureUploadRanges(dev, size)` (step 10).
9. SSBOs (replaces `BindGraphicsStorageBuffer` 583-611 → `MemoStorageBufferBinding` 2746-2785 →
   `StorageBufferBinding` 2675-2687 / `StorageBufferBindingFromDescriptor` 2689-2744): read the
   two u64 words at `cbufs[cbuf_index].address + cbuf_offset` (`Read<u64>` 2678/2683), size
   derivation (`GetMemoryLayoutSize`, `MaxContinuousRange`, `GpuToCpuAddress`) on SEQ (page table
   only); also the driver limit check 587-597 (`total_graphics_storage_buffers`) → done on the
   committer (it's channel_state bookkeeping), so capture every descriptor. → `SsboCapture{stage,
   index, is_written, size, device_addr, **gpu_addr**}` (add `gpu_addr`: needed by
   `BindMultiRangeStorage` keys and by verify). For `is_written`: `CaptureWrittenRange(dev,size)`
   now (§6.4) — the tracker/`gpu_modified_ranges` bit must exist before any later capture.
   For every SSBO: `CaptureUploadRanges(dev,size)` (the old `SynchronizeBuffer` 1354).
10. Vertex/index (replaces `UpdateIndexBuffer` 1579-1663, `UpdateVertexBuffers/UpdateVertexBuffer`
    1666-1755, `BindHostIndexBuffer` 945-970, `BindHostVertexBuffers` 1040-1130):
    - index: `gpu_addr_begin = draw_state.index_buffer.StartAddress()`, `size = min(EndAddress -
      Start, (count+first)*FormatSize)`; translate; `IndexCapture{dev, size, format, first,
      count}`; `CaptureUploadRanges(dev,size)`. Inline index (`!inline_index_draw_indexes.empty()`
      1589-1605, 951-961): copy the bytes into the packet tail (`inline_index_bytes`; >64 KiB →
      SeqStagingRing ref) — never read `draw_state.inline_index_draw_indexes` on the committer;
    - vertex: for every stream 0..31 (not only dirty ones — the committer's flags decide
      rebinds): `array.Address()`, end = native ? `+ regs.NativeVertexStreamSize(i)` : `limit.Address()+1`
      (1683-1691, keep the 32-bit throw), translate, `MaxContinuousRange` when `!IsWithinGPUAddressRange(end) || size >= 64 MiB`
      (1730-1732) → `VertexCapture{index, stride, dev, size}`; `CaptureUploadRanges(dev,size)`
      for enabled ones. (Only streams whose capture changed vs. the previous packet need to be
      sent: keep a SEQ-side last-capture array and send a 32-bit `changed_mask`; the committer
      keeps the last values.)
    - quad/quad-strip (`BindQuadIndexBuffer`), uint8 conversion, vertex repack: GPU-side, read no
      guest memory → committer only.
11. `CaptureUploadRanges(dev,size)` = `memory_tracker.ForEachUploadRange` minus
    `gpu_modified_ranges.ForEachInRange` (exact copy of `SynchronizeBuffer` 2331-2350), copying
    each sub-range's bytes with `device_memory.ReadBlockUnsafe` into SeqStagingRing →
    `StagedUpload{dev, size, staging_ref}`; ranges ≥256 KiB also recorded for
    `uploaded_buffer_ranges` (2435-2437) via the packet flag `cold_hint`. Dedup: the same
    `[dev,size)` captured twice in a packet is staged once (tracker bits are cleared by the first).
12. Draw params: copy `DrawManager::State` minus the vector (topology, draw_mode, draw_indexed,
    draw_element_count, base_index, vertex_buffer, index_buffer, base_instance, instance_count,
    drawn_instances) + `instance_count` arg + `is_indexed`; `zpass_enable =
    regs.zpass_pixel_count_enable` (459) is in the mirror already, kept for verify.
    `mt_pending_flags` (new, §4.1) → packet `flags`.
13. Emit pending register Deltas (RegWriteSet), CbBind/RootTable pseudo-deltas, then the packet.

### 2.2 Complete list of guest reads on the old draw path and their MT handling

| # | Read (file:line) | Old thread | Phase-4 handling |
|---|---|---|---|
| G1 | `TryNx2IndirectReset` descriptor/pointers vk_rasterizer.cpp:285,300 | back | SEQ (step 3) |
| G2 | debug traces 511-587, 616-689, 465-505 | back | SEQ before capture; draw capture debug → SER |
| G3 | texture handles vk_graphics_pipeline.cpp:461-468 | back | SEQ step 6 |
| G4 | `EvaluateTextureHandle` shader_handle_program.h:22,33-34 | back | SEQ step 6 |
| G5 | TIC entries `DescriptorTable::Read` descriptor_table.h:72-76 via `VisitImageView` texture_cache.h:1006 | back | SEQ step 7 → `VisitImageViewCaptured` |
| G6 | TIC `PeekUnchanged` descriptor_table.h:51-55 via `TryFillImageViewsClean` 587 / `TryReuseGraphicsBindings` 643 | back/helper | committer compares captured bytes: `PeekUnchangedCaptured(index, entry)`; BackHelper off under MT |
| G7 | TSC `DescriptorTable::Read` via `GetSamplerId` 775; `PeekUnchanged` 670 | back | SEQ step 7 → `GetSamplerIdCaptured` |
| G8 | `IsValidEntry` util.cpp:837-851 | back | page-table only; committer (inside `VisitImageViewCaptured`) |
| G9 | image creation/upload: `CreateImageView`→`FindImage`/`InsertImage`/`JoinImages` (ASTC scans 2825-2842), `PrepareImageView`→`PrepareImage`→`RefreshContents` 1829→`UploadImageContents` 2229-2272 (`SwizzleImage`/`UnswizzleImage`/`ReadBlock` 2147-2263), sparse/async decode 2469-2640 | back | **committer** (I1 exception), protected by the I5 barrier (§3.5) |
| G10 | render-target lookup `FindColorBuffer`/`FindDepthBuffer` 3133-3164 (+ image creation) in `UpdateRenderTargets` 920-984 | back | committer (mirror regs, G9 rules) |
| G11 | UBO fast path `ReadBlockUnsafe` buffer_cache.h:1238, `BindSmallUniformReusing` uniform_stream_reuse.h | back | SEQ step 8 → SeqStagingRing |
| G12 | UBO/SSBO/vertex/index/texture-buffer/xfb/indirect `SynchronizeBuffer` 2309-2384 → `MappedUploadMemory` `ReadBlockUnsafe` 2432 | back | SEQ `CaptureUploadRanges` + committer `ApplyStagedUpload`; `SynchronizeBuffer` becomes a no-op on the committer while committing a packet (`capture_sync_bypass` flag) |
| G13 | SSBO descriptor words `Read<u64>` 2678,2683,2759,2763 | back | SEQ step 9 |
| G14 | inline index bytes (engine vector) 957-960 | back | packet tail |
| G15 | `root_tables` (engine state, not memory) 1166-1183 | back | SEQ step 8 |
| G16 | `virtual_ranges.Query` multi-range 1285 (page table) | back | forced SER in phase 4 (step 5) |
| G17 | `GetTextureBufferBinding` 2829-2846 (page table) / texture-buffer `SynchronizeBuffer` 1378 | back | SER (UsesTextureBuffers/UsesImageBuffers) |
| G18 | xfb `UpdateTransformFeedbackBuffer` 1835-1850, `BindHostTransformFeedbackBuffers` 1401-1431 | back | SER |
| G19 | pipeline creation environments: `GraphicsEnvironment` ctor shader_environment.cpp:624-711 (sph, root table 1 bytes 652-656, mesh header 703), `ReadInstruction` 157-168, `SetCachedSize` 248-258, `ReadCbufValue` 714-723, `ReadTextureInfo` 384-392 (ReadTextureType/PixelFormat 751-770), `IsCbufStorageBufferPointer` 182-190, native lowering 467-600 | back | `RefreshStages` (SEQ: `MakeShaderInfo` on new programs) and `CreateGraphicsPipeline` (SER). Translation is synchronous (`main_pools`, vk_pipeline_cache.cpp:1493-1497); async workers only build VkPipelines from SPIR-V; the serialization thread uses the env's recorded `cbuf_values`/code → no lazy guest reads after the Serial returns |
| G20 | `BuiltPipeline` 1288-1308 (regs/draw_state) | back | SEQ |
| G21 | query cache `CounterReport` direct writes (not reads) query_cache.h:273-327; `HostConditionalRenderingCompareValue` vk_query_cache.cpp:1488 reads guest | back | CMD / SER (§1.1) |
| G22 | query streamer `dependant_address` vk_query_cache.cpp:1227 | back | committer (reads memory written by GPU-ordered prefix sums; see R12) |
| G23 | `TextureCache::PublishComputeCollisionImage` 532-554 (download + `SwizzleImage` = guest **write**), handle read vk_rasterizer.cpp:1132 | back | compute: SER (packet mode: dispatches with `native_key == 0x6e0ae200ee9aa220` forced SER) |
| G24 | Clear: none (regs only) | | CMD |
| G25 | `BufferCache::UpdateDrawIndirect` 1758-1777 + indirect `SynchronizeBuffer` 1133-1143 | back | DrawIndirect SER |
| G26 | `GraphicsPipeline::ConfigureImpl` `regs.surface_clip`/`rt[0]` for render area 823-826 | back | mirror regs (committer) |

### 2.3 What forces a Serial draw in phase 4

`ForceSlowPath` in the plan: xfb enabled; texture/image buffers; multi-range SSBO; mesh *indirect*
(DrawIndirect is SER anyway); `EDEN_NATIVE_DRAW_CAPTURE`; pipeline miss (only the creation is
SER, the draw then continues as a packet); plan not buildable (e.g. `desc.count != 1` for SSBO,
ASSERT 427). Count each reason in MT_STATS.

### 2.4 Clear (CMD)

SEQ: trace, `FlushCaching`, early-out. Committer: `FlushWork()`, then the old body from
`std::scoped_lock lock{texture_cache.mutex}` (909) to the end against the mirror. No guest reads.

### 2.5 DrawIndirect / DrawTexture (SER)

The whole old body runs on the committer, including its `gpu_memory->FlushCaching()` (360,
via `PrepareDraw`) and `gpu.TickWork()`: allowed because SEQ is blocked (the accumulator and the
sync-request queue are idle). The Serial prologue copies non-register engine state (§3.3) and
sets the mirror's `Shaders` flag (§4.1).

### 2.6 DispatchCompute

* `serial` (default): SER around the old body; the exception (if any) is caught on the committer,
  stored in the Serial result, and **rethrown on the sequencer** after the wait, so
  `DispatchNativeCompute`'s `catch` 598-617 behaves as today.
* `packet`: SEQ: `FlushCaching`, `CurrentComputePipeline` (create/rebuild → SER), null pipeline →
  same throw as 1106 on SEQ; capture: `launch_description` (whole `LaunchParams`), `regs.code_loc`,
  `regs.tic/tsc` (+ `linked_tsc`), handles from QMD cbufs (vk_compute_pipeline.cpp:194-225,
  `const_buffer_config[i].Address()`), TIC/TSC (compute tables: limits `regs.tic.limit`,
  `linked_tsc ? tic_limit : regs.tsc.limit`, texture_cache.h:797-803), compute UBOs
  (`UpdateComputeUniformBuffers` 1853-1868 translation + `BindHostComputeUniformBuffers` 1434-1486
  fast/cached decision), SSBOs (`BindComputeStorageBuffer` 650-690: `native_pointer =
  IsNativeMode()`, `mapping_sized = IsDlssDispatch()`), indirect address
  (`GetIndirectComputeAddress`, 12 bytes: `CaptureUploadRanges` + flag "DiscardWrite"),
  collision-map handle (1132). Texture/image buffers → SER. Configure failure on the committer
  (descriptor ring full, vk_compute_pipeline.cpp:317) can't be reported back: log + count
  (`native_execution_failed` has no reader, macro.h:256). Committer: `Nx2CollectListCopies(true)`
  … `SubmitNx2ListCopies()` around the dispatch exactly as 1111-1114/1175/1192.

---

## 3. What the committer reads beyond registers, and how it gets it

### 3.1 Table

| State (owner today) | Read on committer by | Delivery |
|---|---|---|
| `Maxwell3D::regs` | everything | Deltas (RegWriteSet bits set in `ProcessDirtyRegisters` maxwell_3d.cpp:370-380, `ApplyState` 344-368, `ProcessCBMultiData` 815 (`const_buffer.offset`), `ProcessFirmwareCall4` 628-634 (`shadow_scratch[0]`), `ProcessMacroBind` (`load_mme.start_address_ptr`), DrawManager writes none) → `MirrorApplier` (reg_delta.h:101-117) sets mirror flags from mirror `dirty.tables` |
| `draw_manager.draw_state` (+`inline_index_draw_indexes`) | `MakeDrawParams` vk_rasterizer.cpp:170-191, `UpdateDynamicStates` 1943, `UpdatePrimitiveRestartEnable` 2398, `UpdateDepthBiasEnable` 2512, `FixedPipelineState::Refresh` (SEQ only), buffer cache 502-507, 950-968, 1582-1606, query runtime `View3DRegs` vk_query_cache.cpp:976 | packet header copy → committer writes it into `mirror.draw_manager.draw_state` before resolving; vector replaced by packet bytes (committer keeps a scratch `std::vector<u8>` in the mirror) |
| `draw_manager.indirect_state`, `draw_texture_state` | DrawIndirect 766, DrawTexture 855 | Serial prologue copies from the real engine |
| `Maxwell3D::state.shader_stages[].const_buffers` | `BindGraphicsStorageBuffer` 606 (replaced), `ConfigureImpl` 437 (replaced), `GraphicsEnvironment::ReadCbufValue` (SER), debug | CbBind delta also updates `mirror.state` (`enabled,address,size`); Serial prologue copies all |
| `root_tables` | `BindHostGraphicsUniformBuffer` 1166 (replaced by staged UBO) | RootTable deltas (+ Serial copy) |
| `dirty.flags` | §4 | mirror's own flags; draw-manager-originated flags via packet `flags` |
| `dirty.tables` | `MirrorApplier` | copied once after `SetupDirtyFlags` + `StateTracker::SetupTables` (vk_state_tracker.cpp:237-258) at ChannelSync |
| `engine_state` (HLE hint), `replace_table`, `execute_on`, `GetNativeMacroEngine().native_turing` | `FixedPipelineState::Refresh` 172 (SEQ), GraphicsEnvironment (SER), `UpdateVertexBuffer` 1686 (replaced by capture), `CreateGraphicsPipeline` 1337 (SER) | SEQ-only, or Serial copy; `native_turing` copied at ChannelSync and on change (rare) |
| `shadow_state`, macro state (`macro_engine`, `executing_macro`, `upload_state`, `front` state) | not read by renderer code | never mirrored |
| `KeplerCompute::launch_description` (written directly by `ProcessLaunch` kepler_compute.cpp:1134-1139 `ReadBlockUnsafe` and `DispatchNativeCompute` 548-571) | `ComputePipeline::Configure`, buffer cache 678-689 / 1857-1864, texture cache 797, pipeline cache 761/796, vk_rasterizer.cpp:1121 | ComputePacket copy / Serial copy |
| `KeplerCompute::regs` (code_loc written directly 560-561; tic/tsc via CallMethod) | pipeline cache, texture cache 797-803 | ComputeDeltas + packet copy of `code_loc/tic/tsc`; Serial copy of all |
| `KeplerCompute::IsNativeMode/IsDlssDispatch/GetIndirectComputeAddress` (QMD-derived) | buffer cache 689, vk_rasterizer.cpp:1106,1157 | packet flags / Serial |
| QMD reads (`native_qmd.Signal` descriptor `ReadBlockUnsafe` kepler_compute.cpp:368) | SEQ only | n/a |
| `ChannelState`/`ChannelSetupCaches::{maxwell3d,kepler_compute,gpu_memory,program_id,channel_state}` channel_state_cache.inc:61-64 | all caches | one object per cache: bound on the committer by the Serial `BindChannel`; the sequencer reads only `gpu_memory`/real engines it already has (`ChannelSetupCaches` fields read on SEQ: `RasterizerVulkan::maxwell3d/gpu_memory` in capture code, `PipelineCache::maxwell3d/gpu_memory`, `BufferCache::gpu_memory/channel_state` for memo + `uniform_buffer_skip_cache_size`) → keep them; they only change in the Serial `BindChannel`, while SEQ is blocked |
| `program_id` | `UpdateDepthBias` 2178 | same channel object |
| `Settings::values.resolution_info` | rescale paths | ANY |

### 3.2 `ThreadEnginePtr` placement (engine_ptr.h:31-85)

Replace `Tegra::Engines::Maxwell3D* maxwell3d` / `KeplerCompute* kepler_compute` in
`ChannelSetupCaches` (control/channel_state_cache.h:84-85) and `GraphicsPipeline::maxwell3d`
(vk_graphics_pipeline.h:172) with `MT::ThreadEnginePtr<...>`. The committer installs
`ScopedEngineOverride<Maxwell3D>{real, mirror}` and `<KeplerCompute>` for the bound channel for its
whole loop (re-installed at ChannelSync). Serial items therefore run old code against the mirror.
`QueryCacheRuntime::Bind3DEngine` (vk_query_cache.cpp:1365) stores a raw pointer → store the
`ThreadEnginePtr` too (it's read in `View3DRegs` on the committer).

### 3.3 Mirror equality requirement for Serial

The Serial prologue (committer, SEQ blocked) does `SyncNonRegState(real, mirror)`: `draw_manager`
(all three structs + vector), `state`, `root_tables`, `engine_state`, `execute_on`, compute
`launch_description` + regs not covered by deltas, and asserts under MT_VERIFY that
`mirror.regs == real.regs` (memcmp; log first differing register). Flags are **not** copied.

### 3.4 Buffer state split (phase 4, everything under `buffer_cache.mutex`)

| Item | Capture (SEQ) | Commit (committer) |
|---|---|---|
| tracker CPU bits | `ForEachUploadRange` clears (CaptureUploadRanges) | — (`SynchronizeBuffer` bypassed) |
| tracker GPU bits, `gpu_modified_ranges.Add`, `Nx2MarkGpuWritten`, `MarkVertexBuffersStale` | `CaptureWrittenRange` | re-applied idempotently in `CommitWrittenRange` (a fence pop or download between capture and commit may have cleared them) |
| `pending_download_ranges.Subtract`, `setWriteTick`, `QueueFenceDownload`, `Nx2NoteListWrite` | — | `CommitWrittenRange` (must follow the fences recorded before this draw, as `MarkWrittenBuffer` 1890-1911 does today) |
| `channel_state->uniform_buffers` | SEQ keeps its own `SeqCbTable` | from CbBind deltas |
| vertex/index/ssbo bindings, buffer ids, LRU | — | committer |

---

## 4. `dirty.flags` users and direct engine-pointer uses

### 4.1 Every read/modify of `dirty.flags`

Engine side (real engine, SEQ): `Maxwell3D` ctor `dirty.flags.flip()` maxwell_3d.cpp:50;
`ApplyState` 364-365; `ProcessDirtyRegisters` 378-379; `DrawManager::DrawIndexSmall`
draw_manager.cpp:251 and `DrawEnd` InlineIndex 266 (`IndexBuffer = true`) → **also set
`MT pending flag IndexBuffer`** (new `Maxwell3D::mt_pending_flags` u32, cleared at capture,
shipped as `DrawPacketHeader::Flags::ForceIndexDirty`); the committer sets
`mirror.dirty.flags[Dirty::IndexBuffer]` from it.

Renderer/caches (must run on the committer mirror, except the four SEQ consumers):

| Site | Flags | Thread under MT |
|---|---|---|
| vk_rasterizer.cpp:438-445 `PrepareDraw` repack | VertexBuffers, VertexInput, VertexBuffer0+n (set) | committer |
| vk_rasterizer.cpp:1942-1947 `UpdateDynamicStates` | DepthBiasEnable, PrimitiveRestartEnable (set) | committer |
| vk_rasterizer.cpp:2058 `UpdateViewportsState` | Scissors (set) | committer |
| vk_rasterizer.cpp:2760-2808 `UpdateVertexInput` | VertexInput, VertexBuffers (read), VertexInput/VertexAttribute0+n/VertexBinding0+n (clear) | committer |
| vk_state_tracker.h:87-310 all `Touch*`, `Invalidate*`, `Exchange` via `flags` ptr; `ChangeChannel` vk_state_tracker.cpp:260-266 (`flags = &channel.maxwell_3d.dirty.flags; flags->set()`) | many | committer; **`ChangeChannel` must point at the channel's mirror** (`MT::MirrorOf(channel)->dirty.flags`) |
| vk_scheduler.cpp:319,334 `InvalidateStateEnableFlag`, 489 `InvalidateCommandBufferState` | StateEnable, invalidation set | committer (scheduler owner) |
| buffer_cache.h:1028-1035 `RefreshRepackedVertexBuffers` | VertexBuffer0+n (set) | committer |
| buffer_cache.h:1050-1074, 1094-1113 `BindHostVertexBuffers` | VertexBuffer0+n (read/clear) | committer |
| buffer_cache.h:1584-1588 `UpdateIndexBuffer` | IndexBuffer (read/clear) | committer |
| buffer_cache.h:1667-1680 `UpdateVertexBuffers/UpdateVertexBuffer` | VertexBuffers, VertexBuffer0+n | committer |
| buffer_cache.h:2660-2668 `DeleteBuffer` | IndexBuffer, VertexBuffers, VertexBuffer0+n (set) | committer (FindBuffer/CreateBuffer/GC only there) |
| texture_cache.h:825-866 `RescaleRenderTargets` | RenderTargets, RenderTargetControl, ColorBuffer0+n, ZetaBuffer | committer |
| texture_cache.h:922-983 `UpdateRenderTargets` | RenderTargets (read), RescaleViewports, RescaleScissors, DepthBiasGlobal (set) | committer |
| texture_cache.h:2399-2405 `InvalidateScale` | RenderTargets, ZetaBuffer, ColorBuffer0+n | committer (also reached from `FillImageViews` blacklist → `ScaleDown`) |
| texture_cache.h:3605-3610 `DeleteImage` | RenderTargets, ZetaBuffer, ColorBuffer0+n | committer — **assert role**: `UnmapMemory` reaches it only via SER; any other thread would dirty the real engine and the committer would keep a dead RT view |
| fixed_pipeline_state.cpp:193-214 (VertexInput, non-dynamic clears), 233-234 (ViewportSwizzles), 252-253 (Blending) | | **SEQ** (real engine; `graphics_key` is sequencer-owned). Under MT with dynamic vertex input, also clear the real `VertexInput` after Refresh (the committer clears its own in `UpdateVertexInput`), else SEQ recomputes `attribute_types` every draw |
| shader_cache.cpp:102-106 `RefreshStages` | Shaders | **SEQ**; Serial paths that run `CurrentGraphicsPipeline` on the committer (DrawIndirect/DrawTexture/compute SER) must first set `mirror.dirty.flags[Shaders] = true` |

`StateTracker::flags` initial `&default_flags` (vk_state_tracker.cpp:271) unchanged.

### 4.2 Direct `maxwell3d->` / `kepler_compute->` / `gpu_memory->` uses

renderer_vulkan/vk_rasterizer.cpp — maxwell3d: 263, 279, 287, 340, 365-370, 385-389, 434-438,
459, 466, 483, 501, 598, 611-613, 617-627, 650-669, 702, 709, 721, 766, 854-855, 890, 896, 900,
997, 1943-1944, 2058, 2398, 2512, 2760; kepler_compute: 1106, 1115, 1117, 1121, 1157;
gpu_memory: 276-277, 281, 285, 293, 296-297, 300, 307, 313, 360, 474, 476, 487, 611, 622, 632,
672, 675, 896, 898, 1101, 1115, 1132, 1672, 1716-1735. Classification: everything in
`TryNx2IndirectReset`, traces, `Draw` pre-checks, `PrepareDraw` up to the pipeline lookup → SEQ;
`DrawTexture`/`DrawIndirect`/compute → committer via SER; `UpdateDynamicStates`, Clear body,
`UpdateViewportsState`, `UpdateVertexInput`, `HandleTransformFeedback`, `report_failure` after
Configure → committer mirror; `AccelerateConditionalRendering` 1672 `FlushCaching` → SEQ part
before the SER; `AccelerateInlineToMemory` → SEQ.

renderer_vulkan/vk_graphics_pipeline.cpp — `maxwell3d->regs` 414, `maxwell3d->state` 437,
`*gpu_memory` 442, `gpu_memory->Read` 461-468 (all inside `ConfigureImpl`, split §6.6).

renderer_vulkan/vk_compute_pipeline.cpp — `kepler_compute.launch_description` 191,
`gpu_memory.Read` 219-224 (parameters, not members): SER or ComputePacket split.

renderer_vulkan/vk_pipeline_cache.cpp — maxwell3d 730, 741, 1297, 1303, 1337-1338;
kepler_compute 761, 796, 1534-1536; gpu_memory 808-810, 1536. SEQ (graphics fast path), SER
(creation), compute per §2.6.

renderer_vulkan/vk_query_cache.cpp — `View3DRegs` 746-747, 974-993, 1170-1173 (committer, mirror);
`device_memory.GetPointer` 1227, 1488 (committer / SER).

shader_cache.cpp — maxwell3d 102-151, 253-266; kepler_compute 208-221; gpu_memory 131, 160, 210.
shader_environment.cpp — 630-711, 714-770 (env construction/translation: SEQ in RefreshStages, SER
in creation).

buffer_cache/buffer_cache.h — maxwell3d 502, 606, 950, 1028, 1050, 1075, 1094, 1118, 1166-1167,
1402, 1408, 1582-1584, 1667-1668, 1680-1687, 1826, 1836, 2660; kepler_compute 678-689, 1857;
gpu_memory 173, 322-323, 392, 417, 458, 1285, 1320, 1624, 1628, 1637, 1654, 1704, 1711, 1722,
1730-1731, 1745, 1760, 1839, 1860, 2678, 2683, 2695, 2713, 2721, 2726, 2758-2777, 2797-2821, 2831.
Under MT the translation/descriptor-reading ones (417 ObtainBuffer is SER-only, 458, 1624-1663,
1704-1755, 1760, 1839, 1860, 2678-2785, 2831) run on SEQ (capture) or SER; the committer uses the
`*Captured` replacements (§6.5).

texture_cache/texture_cache.h — maxwell3d 809-815, 825-983, 2399, 3134-3164, 3605, 4000;
kepler_compute 797-803; gpu_memory 187, 323, 544, 551, 580, 587, 628, 643, 670, 775, 1001-1008,
1124, 1646, 1651, 1694, 1938, 2020, 2147-2274, 2481-2482, 2606, 2668-2671, 2695, 2782, 2825-2842,
2867, 3355-3357. Committer (mirror) except the TIC/TSC reads (580-670, 775, 1006) which get
captured variants.

query_cache/query_cache.h — `gpu_memory` 245-256, 404; `maxwell3d` 230, 442: committer (mirror).

control/channel_state_cache.inc:61-64 sets them (`BindToChannel`): SER.

---

## 5. gpu_split interaction and `settled`

### 5.1 New state in `GpuSplit` (gpu_split.h)

```cpp
alignas(128) std::atomic<u64> settled{};       // front ring position whose effects are committed
std::atomic<bool> mt_active{};                  // committer running (EDEN_NX2_MT, Switch 2, split active)
std::atomic<bool> committer_running{};
std::atomic<bool> committer_quiet{true};       // committer ran its idle hook and has nothing queued
alignas(64) std::atomic<bool> commit_hook_requested{};
std::atomic<u32> commit_wake{};                 // futex word the committer sleeps on
static inline thread_local bool on_committer_thread{};
void StoreSettled(u64 position);               // max-store + wake front (front_waiting/front_wake)
void WaitSettled(u64 target);                  // WaitConsumed's twin over `settled`
```

### 5.2 Changes per function

| Function (gpu_split.h / .cpp) | Change |
|---|---|
| `OwnsScheduler()` h:99-101 | `mt_active ? on_committer_thread : (BackRunning() ? on_back_thread : on_gpu_thread)` |
| `OnBackThread()` users that mean "scheduler owner": `Scheduler::DeferFlush` vk_scheduler.cpp:83, `InnerFence::Wait` vk_fence_manager.cpp:47-48, `BufferCacheRuntime::WaitSubmittedTick` vk_buffer_cache.cpp:467-468, parallel swizzle textures/decoders.cpp:108 | new `GpuSplit::OnSchedulerOwner()` = `OwnsScheduler()`; `WaitSubmitted` callers test `!OnSchedulerOwner()` |
| `CheckOwner` cpp:220-254 / `g_back_busy`, `g_back_thread_id` | add committer id/busy; owner = committer under MT |
| `SetIdleHook` h:103-106 | stored as before; **called by the committer** under MT; `BackLoop` must skip it when `mt_active` (cpp ~764-769 and 799-803) |
| `RequestIdleHook` h:112-116 | under MT: `commit_hook_requested=true; commit_wake++ ; notify` |
| `BackRunning()` h:108-110 used by `Scheduler::WaitSubmitted` vk_scheduler.cpp:109 and `WaitConsumed` | add `CommitterRunning()`; `WaitSubmitted` exits on `!CommitterRunning()` under MT |
| `BackQuiet()` h:627-630 | `(back_quiet && (!mt_active || committer_quiet)) || !back_running` |
| `Drain` cpp:488-517 | fast exit only if `consumed == write_position && settled >= write_position && BackQuiet()`; else `Publish(); WaitConsumed(write_position); WaitSettled(write_position);` then the idle-hook handshake against the committer (`RequestIdleHook` → spin on `committer_quiet`) |
| `WaitForPosition` cpp:519-533 | `wait_target.store(position)`; `Publish(); WaitSettled(position)` (sequencer emits `Mark(read_position)` when it passes `wait_target`, closes the batch) |
| `WaitForWriters` cpp:535-551 | same as WaitForPosition with `target=min(last_writer_position,write_position)` — the list copies (`Nx2CopyListPages`) and GPU-written marks of compute/DMA are made on the committer, so **settled** is the right bound |
| `WaitForSpace`/`ReserveSlow`/`consumed_cache` | unchanged: ring space is the sequencer's `consumed` |
| `NotePendingWrite` pruning h:308-342 (`done = consumed`) | `done = SettledPosition()` (writes noted are fence/query/compute writes → committer) |
| `KnownPendingWords` h:382-401, `PendingAddSum` h:441-461 | `consumed` → `settled` (adds are applied by fence callbacks on the committer) |
| `ConsumedPosition()` h:300 users: puller.cpp:65 (stats) → settled; kepler_compute.cpp:1235 inline-QMD shadow → **keep consumed** (the inline QMD is written by `AccelerateInlineToMemory` on SEQ) |
| `Run` h:269-279 | unchanged (Defer + Drain); Drain now waits settled |
| `BackLoop` cpp:724-822 | MT: role `Sequencer`; skip idle hook; after the inner loop and when `read_position >= wait_target`, call `pipeline.CloseBatchAndMark(read_position)`; if the DrawStream has nothing unread by the committer and no Serial outstanding, `settled` CAS-max to `read_position` directly; `StoreConsumed` as today |
| `MaybeStart` cpp:258-316 | after the back thread: if `MT::Enabled() && MT::Resolvers()==0` (phase 4 forces K=0) create `Nx2DrawPipeline` + committer thread (needs the rasterizer: `RasterizerVulkan::StartMt()` called by the sequencer on its first Execute, since only it knows the rasterizer — or pass through `SetIdleHook`'s argument path: add `SetMtHooks(start, stop, ctx)` set by `RasterizerVulkan`'s ctor next to `SetIdleHook` vk_rasterizer.cpp:245-247) |
| `Stop` cpp:330-360 | before joining the back: drain the DrawStream, stop the committer (`commit_wake`, join), reset `settled`, `mt_active=false`; `RasterizerVulkan` dtor 250-254 clears the hooks |
| `BackHelper::Enabled` cpp:135-147 | `false` when `mt_active` |

### 5.3 `settled` protocol

* Producer of positions: the sequencer. Every DrawStream entry kind carries no position except
  `Mark{ring_position}`; the sequencer emits a Mark (a) at the end of each executed ring chunk
  (`read_position == available`), (b) when `read_position >= wait_target`, (c) right after any
  Serial/Command that the front may be waiting on (`Run` drains anyway → (a)).
* Committer: on `Mark{p}` → `StoreSettled(p)` (max-store, seq_cst; wake front if
  `front_waiting`). Also on idle (stream empty) it re-stores the last Mark.
* Sequencer shortcut: when it reaches (a) and `stream.ConsumerPosition(committer) ==
  stream.WritePosition()` and `serial_pending == 0`, it CAS-maxes `settled` itself.
* Invariant: `settled <= consumed <= write_position`. `Drain` returns only when both reach
  `write_position` and the committer is quiet.

---

## 6. Code skeleton

### 6.1 Roles (`video_core/mt/role.h`, new header-only — or inside vk_mt_pipeline.h)

```cpp
namespace VideoCommon::MT {
enum class Role : u8 { None, Front, Sequencer, Committer, Resolver };
inline thread_local Role g_role = Role::None;
[[nodiscard]] inline bool OnSequencer() noexcept { return g_role == Role::Sequencer; }
[[nodiscard]] inline bool OnCommitter() noexcept { return g_role == Role::Committer; }
inline std::atomic<bool> g_active{};            // set by StartMt, read relaxed
[[nodiscard]] inline bool Active() noexcept { return g_active.load(std::memory_order_relaxed); }
#define MT_CHECK_ROLE(r) /* EDEN_NX2_MT_CHECK: ASSERT(g_role == r) */
}
```

### 6.2 `renderer_vulkan/vk_mt_pipeline.h`

```cpp
namespace Vulkan::MT {
using namespace VideoCommon::MT;

struct CommandHeader {           // payload of StreamEntry::Command / Serial
    void (*run)(RasterizerVulkan&, const u64* payload); // trampoline, committer side
    u32 payload_words;
    u32 kind;                     // CommandKind, for stats/verify
};
enum class CommandKind : u32 {
    Clear, Query, ResetCounter, SignalFence, SyncOperation, SignalSyncPoint, SignalReference,
    ReleaseFences, WaitForIdle, FragmentBarrier, FlushCommandsAndListEnd, TickWork, InlineUpload,
    InvalidateQueryRegion, Generic, // Commands
    SerialGeneric, CreateGraphicsPipeline, CreateComputePipeline, BindChannel, // Serials
};

struct SerialCell {                 // lives on the sequencer stack while it waits
    std::atomic<u32> done{0};
    std::exception_ptr error;       // compute rethrow (§2.6)
    alignas(8) std::array<u8, 64> result{};
};

class Nx2DrawPipeline {
public:
    Nx2DrawPipeline(RasterizerVulkan& rasterizer, const Device& device,
                    MemoryAllocator& allocator);
    ~Nx2DrawPipeline();             // Stop()

    void Start();                   // creates SeqStagingRing, committer consumer, GPUCommit thread
    void Stop();                    // drain, stop, join

    // ---- sequencer side ----
    /// RasterizerVulkan::Draw under MT. Returns false when the draw was skipped at capture.
    bool CaptureDraw(bool is_indexed, u32 instance_count);
    void CaptureCompute();          // EDEN_NX2_MT_COMPUTE=packet
    /// Async ordered: copies `func` (trivially copyable, <= 512 B) into the stream.
    template <typename F> void EmitCommand(CommandKind kind, const F& func);
    /// Blocks the sequencer until the committer ran `func`; returns its result.
    template <typename F> auto EmitSerial(CommandKind kind, F&& func) -> std::invoke_result_t<F&>;
    void EmitDeltas();              // RegWriteSet (3D + compute) → Deltas entries
    void EmitCbBind(const CbBindDelta& delta, std::optional<DAddr> device_addr);
    void EmitRootTable(const RootTableDelta& delta);
    void CloseBatchAndMark(u64 ring_position);
    void WaitCommitted();           // until the committer consumed everything emitted so far (I5, lockstep)
    void WaitCommitted(u64 seq);    // until packet `seq` committed
    [[nodiscard]] u64 NextSeq() const noexcept { return next_seq; }

    // ---- committer side ----
    void CommitterLoop(std::stop_token stop);

    // ---- any ----
    [[nodiscard]] u64 CommittedSeq() const noexcept { return committed_seq.load(std::memory_order_acquire); }
    SeqStagingRing& Staging() noexcept { return staging; }

private:
    void CommitDraw(const DrawPacketHeader& header, const u64* payload);
    void CommitCompute(const ComputePacketHeader& header, const u64* payload);
    void RunSerial(const u64* entry);       // SyncNonRegState + run + signal
    void SyncNonRegState();                 // §3.3, real → mirror, SEQ is blocked
    void ApplyChannelSync(const u64* entry);
    void OnIdle();                          // idle hook, settled re-store, futex wait

    RasterizerVulkan& rasterizer;
    SpmcStream stream;                      // 16 MiB of u64 words (plan)
    u32 committer_consumer{};
    SeqStagingRing staging;
    // sequencer-owned
    u64 next_seq = 1;
    u32 batch_draws = 0;
    bool batch_open = false;
    RegWriteSet regs3d_written, compute_written;
    std::array<VertexCapture, 32> last_vertex{};   // send changed only
    std::unordered_map<const GraphicsPipeline*, std::unique_ptr<CapturePlan>> plans;
    std::array<std::array<SeqCbEntry, 18>, 5> seq_cb{};   // {enabled, gpu_addr, device_addr, size}
    UniformStagingMemo ubo_memo;            // per (stage,slot) bytes + staging ref
    std::atomic<u32> serial_pending{0};
    // committer-owned
    std::vector<u8> inline_index_scratch;
    std::jthread committer;
    // shared
    alignas(64) std::atomic<u64> committed_seq{0};
    alignas(64) std::atomic<u64> emitted_seq{0};
    std::atomic<u32> commit_wait_word{0};   // WaitCommitted futex
};
}
```

`DrawPacket` stream layout (one `StreamEntry::DrawPacket`, `aux` = draw header words): the plan's
`DrawPacketHeader` (draw_packet.h) **plus** these fields (add them): `GraphicsPipeline* pipeline`
(SEQ-resolved), `u32 draw_mode`, `u32 base_index`, `IndexBuffer index_regs` (address, limit,
format, first, count — `MakeDrawParams` + `UpdateIndexBuffer`), `u32 instance_arg`,
`u32 vertex_changed_mask`, `u16 num_handles`, `u16 num_sampler_handles`, `u8 cold_upload`; then
arrays in this order: `u32 handles[num_handles]`, `u32 sampler_handles[]`, `TicCapture[]` (one
per handle, `index == ~0u` = null view), `TscCapture[]` (one per sampler handle), `UboCapture[]`
(BindHostStageBuffers order), `SsboCapture[]` (+`gpu_addr`), `VertexCapture[popcount(changed)]`,
`IndexCapture`, `StagedUpload[]`, inline index bytes (padded to u64). Size check:
`num_tic*40 + ...` must fit `EDEN_NX2_MT` stream max entry (assert ≤ 64 KiB; bigger → SER).

Committer `CommitDraw` order (= old PrepareDraw tail + ConfigureImpl, under both cache locks):

1. `FlushWork()` (vk_rasterizer.cpp:1826; old position 359).
2. `std::scoped_lock{buffer_cache.mutex, texture_cache.mutex}`; `buffer_cache.capture_sync_bypass = true` (scope).
3. write `header` draw state into `mirror.draw_manager.draw_state`; ForceIndexDirty → mirror flag.
4. `ApplyStagedUpload` for every StagedUpload (`ApplyStagedUpload` §6.5), `uploaded_buffer_ranges` hint.
5. `pipeline->SetEngine(mirror, gpu_memory)` (no-op with ThreadEnginePtr), repack
   `SetVertexRepackLayout(mirror.regs, ...)` + flags (438-446).
6. `pipeline->ConfigureCaptured(is_indexed, ResolveInputs{...})` (§6.6) → false: `report_failure("configure failed")` (mirror regs) and return.
7. `UpdateDynamicStates()` with `commit_pipeline = header.pipeline` (2018 fix).
8. `query_cache.NotifySegment(true); HandleTransformFeedback(); query_cache.CounterEnable(ZPass, mirror.regs.zpass_pixel_count_enable);`
9. record the draw exactly as 720-761 (`MakeDrawParams(mirror.draw_manager.draw_state, ...)`, mesh branch).
10. `committed_seq.store(header.seq)`; wake `WaitCommitted` waiters.

`CommitterLoop` skeleton:

```cpp
void Nx2DrawPipeline::CommitterLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("GPUCommit");
    g_role = Role::Committer;
    GpuSplit::MarkCommitterThread();               // on_committer_thread = true
    system.RegisterHostThread();
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    auto overrides = InstallMirrorOverrides();     // ScopedEngineOverride 3D + compute (bound channel)
    while (!stop.stop_requested()) {
        const u64* entry = stream.Peek(committer_consumer);
        if (entry == nullptr) { OnIdle(); continue; }
        split.committer_quiet.store(false, std::memory_order_seq_cst);
        const u64 head = entry[0];
        switch (static_cast<StreamEntry>(SpmcStream::HeaderKind(head))) {
        case StreamEntry::Deltas:        MirrorApplier<Maxwell3D>::ApplyPairs(Mirror3D(), entry + 1, aux); break;
        case StreamEntry::ComputeDeltas: MirrorApplier<KeplerCompute>::ApplyPairs(MirrorCompute(), entry + 1, aux); break;
        case StreamEntry::CbBind:        ApplyCbBind(entry); break;     // mirror.state + BindGraphicsUniformBufferTranslated
        case StreamEntry::RootTable:     ApplyRootTable(entry); break;
        case StreamEntry::DrawPacket:    CommitDraw(Header<DrawPacketHeader>(entry), Payload(entry)); break;
        case StreamEntry::ComputePacket: CommitCompute(Header<ComputePacketHeader>(entry), Payload(entry)); break;
        case StreamEntry::Command:       RunCommand(entry); break;
        case StreamEntry::Serial:        RunSerial(entry); break;      // signals SerialCell::done
        case StreamEntry::BatchEnd:      break;                        // K=0: wake granularity only
        case StreamEntry::Mark:          split.StoreSettled(entry[1]); break;
        case StreamEntry::ChannelSync:   ApplyChannelSync(entry); overrides = InstallMirrorOverrides(); break;
        default: break;
        }
        stream.Advance(committer_consumer, entry);
        if (split.commit_hook_requested.load(std::memory_order_relaxed)) [[unlikely]] {
            split.commit_hook_requested.store(false, std::memory_order_seq_cst);
            scheduler.FlushDeferred();             // the old idle hook
        }
        if (EDEN_NX2_MT_LOCKSTEP) committed_seq.store(...), notify;
    }
}
```

`OnIdle`: spin `EDEN_NX2_BACK_SPIN`-like count on `stream.Peek`; then
`scheduler.FlushDeferred()` once (if not quiet), `committer_quiet=true`, re-store settled,
`stream.WaitForData(consumer, stop || hook_requested)`.

`EmitSerial` (SEQ): `EmitDeltas(); CloseBatch(); SerialCell cell; stream.Push(Serial, {&cell,
trampoline<F>, copy of F}); while (!cell.done) spin→futex; if (cell.error) rethrow; return
result`. Lockstep and `WaitCommitted()` use `emitted_seq`/`committed_seq`.

### 6.3 `renderer_vulkan/vk_mt_staging.h` — `SeqStagingRing`

```cpp
class SeqStagingRing {
public:
    static constexpr u64 Size = 64_MiB;           // host-visible Upload memory (plan: not VRAM)
    struct Ref { VkBuffer buffer; VkDeviceAddress device_address; u64 offset; u64 tag; u8* host; };
    SeqStagingRing(const Device&, MemoryAllocator&);  // committer creates it (allocator not thread-safe)
    /// Sequencer. Blocks (spin, then WaitCommitted + committer's tick release) when full.
    [[nodiscard]] Ref Allocate(u64 size, u64 align, u64 seq);
    /// Committer, at Scheduler submit of the command buffer that consumed seqs <= `seq`:
    void NoteSubmitted(u64 seq, u64 tick);
    /// Committer (or fencing thread): frees every region whose tick is free.
    void Reclaim(const Scheduler& scheduler);
    /// UBO reuse memo: the region tagged `tag` is still live; extends its lifetime to `seq`.
    bool Retain(u64 tag, u64 seq);
private:
    vk::Buffer buffer;                            // UNIFORM | STORAGE | TRANSFER_SRC | INDEX | VERTEX | SHADER_DEVICE_ADDRESS
    VkDeviceAddress base_address{};
    std::span<u8> mapped;
    std::atomic<u64> head{0};                     // sequencer
    std::atomic<u64> released{0};                 // committer
    struct Pending { u64 end; u64 max_ref_seq; u64 tick; };
    std::deque<Pending> pending;                  // committer only, under a spinlock for NoteSubmitted
};
```

Lifetime: a region is free once (a) every packet referencing it (`max_ref_seq`) is committed and
(b) the command buffer that recorded those copies/binds (`tick`) is free. The committer records
`{seq_end, CurrentTick()}` at each `Scheduler::Flush`/`SubmitExecution` (hook in `FlushWork` /
`FlushCommands`); the reuse memo bumps `max_ref_seq`.

### 6.4 `renderer_vulkan/vk_mt_resolve.h`

```cpp
struct ExclusiveAccess {};   // committer, both cache mutexes held (phase 4: the only mode)
struct ResolverAccess {};    // phase 5

struct ResolveInputs {       // views into the packet
    std::span<const u32> handles, sampler_handles;
    std::span<const TicCapture> tics;
    std::span<const TscCapture> tscs;
    std::span<const UboCapture> ubos;
    std::span<const SsboCapture> ssbos;
    std::span<const VertexCapture> vertices; u32 vertex_changed_mask;
    const IndexCapture* index; std::span<const u8> inline_index;
};

struct ResolvedDraw {        // phase 5 fills it in resolvers; phase 4 = verify/stats record
    u64 seq; u32 status; GraphicsPipeline* pipeline;
    u64 buffer_epoch, pipeline_epoch;
    std::array<u32, 4> rescaling; std::array<f32, 2> render_area; bool uses_render_area;
    boost::container::static_vector<VideoCommon::ImageViewId, 240> views;
    boost::container::static_vector<VideoCommon::SamplerId, 192> samplers;
    struct ImageRef { VideoCommon::ImageId id; u32 version; bool written, blacklist; };
    boost::container::static_vector<ImageRef, 48> images;
    u32 descriptor_offset, descriptor_count;  // arena span (phase 5)
};

template <typename Access>
bool ResolveDraw(RasterizerVulkan& r, const DrawPacketHeader& h, const ResolveInputs& in,
                 ResolvedDraw& out);           // phase 4: thin wrapper over ConfigureCaptured
bool ApplyResolvedDraw(RasterizerVulkan& r, const ResolvedDraw& d); // phase 5; phase 4 unused
```

### 6.5 New cache methods

BufferCache (`buffer_cache/buffer_cache_base.h` decl, `buffer_cache.h` def):

| Method | Thread | Logic taken from |
|---|---|---|
| `template<F> void CaptureUploadRanges(DAddr, u64 size, F&& emit)` | SEQ (mutex held) | `SynchronizeBuffer` 2313-2350: `HasPendingCpuUpload` early-out, `memory_tracker.ForEachUploadRange` minus `gpu_modified_ranges.ForEachInRange`; `emit(start, size)` copies bytes |
| `void CaptureWrittenRange(DAddr, u32 size)` | SEQ | `MarkWrittenBuffer` 1903-1908 minus buffer-dependent parts: tracker `MarkRegionAsGpuModified`, `MarkVertexBuffersStale`, `gpu_modified_ranges.Add`, `Nx2MarkGpuWritten` |
| `void CommitWrittenRange(BufferId, DAddr, u32 size, bool needs_sync)` | committer | `MarkWrittenBuffer` 1890-1911 entirely (idempotent re-adds + `pending_download_ranges.Subtract`, `setWriteTick`, `QueueFenceDownload`, `Nx2NoteListWrite`) — used instead of `MarkWrittenBuffer` in `BindHostGraphicsStorageBuffers` 1359-1361 while `capture_sync_bypass` |
| `UboCapture CaptureUniform(size_t stage, u32 index, u32 used_size, const SeqCbEntry&, SeqStagingRing&, F&& stage_upload)` | SEQ | 1162-1241 decision logic (§2.1 step 8) |
| `void BindHostGraphicsUniformBufferCaptured(size_t stage, u32 index, u32 binding_index, const UboCapture&)` | committer | 1162-1270: Staged → `channel_state->fast_bound_uniform_buffers` bookkeeping 1223-1224 + `runtime.RebindStreamUniform({buffer, device_address, offset}, size)`; Cached → `TouchBuffer`, (no `SynchronizeBuffer`), 1246-1269 |
| `void BindHostStageBuffersCaptured(size_t stage, std::span<const UboCapture>& cursor)` | committer | `BindHostStageBuffers` 517-525 |
| `void BindGraphicsUniformBufferTranslated(size_t stage, u32 index, std::optional<DAddr>, u32 size)` | committer | 456-469 without `GpuToCpuAddress`; `DisableGraphicsUniformBuffer` unchanged |
| `bool BindGraphicsStorageBufferCaptured(size_t stage, size_t ssbo_index, const SsboCapture&)` | committer | 583-611 (driver-limit bookkeeping kept) with the captured Binding instead of `MemoStorageBufferBinding` |
| `void SetCapturedIndexBuffer(const IndexCapture&, std::span<const u8> inline_bytes)` | committer | `UpdateIndexBuffer` 1579-1663 after the dirty check: inline branch 1589-1605 unchanged; else `FindBuffer(dev,size)` |
| `void SetCapturedVertexBuffer(u32 index, const VertexCapture&)` | committer | `UpdateVertexBuffer` 1679-1755 after the dirty check, from `FindBuffer(dev,size)` on (memo branch dropped under MT) |
| `bool ApplyStagedUpload(const StagedUpload&, const SeqStagingRing::Ref&)` | committer | `MappedUploadMemory` 2423-2444 with the staging already filled: `FindBuffer`, `BufferCopy{src=ref.offset, dst=buffer.Offset(dev)}`, `CanReorderUpload`, `runtime.CopyBuffer(buffer, staging_vk_buffer, ...)`; returns `false` if the target buffer changed its range (impossible within one packet) |
| `bool capture_sync_bypass` | committer | makes `SynchronizeBuffer` return `true` immediately (and `MarkWrittenBuffer` → `CommitWrittenRange`) |
| `CaptureInlineUpload(DAddr, size, bytes)` / `ApplyInlineUpload` | SEQ / committer | `InlineMemory` 2447-2462 (the `IsRegionGpuModified` test on SEQ) / `InlineMemoryImplementation` 2465-2489 minus `SynchronizeBuffer` |
| `TryResolveRange`, `buffer_epoch` | phase 5 | — |

`BufferCacheRuntime` (vk_buffer_cache.h): `RebindStreamUniform` already exists (202-204);
add `CopyBuffer(Buffer& dst, VkBuffer src, copies, barrier, can_reorder)` overload taking a raw
VkBuffer (the SeqStagingRing), or wrap it as a `StagingBufferRef`.

TextureCache (`texture_cache/texture_cache_base.h` decl, `texture_cache.h` def):

| Method | Thread | Logic taken from |
|---|---|---|
| `ImageViewId VisitImageViewCaptured(u32 handle_index, const TICEntry* captured, bool compute)` | committer | `VisitImageView` 998-1028 with `table.Read` replaced by `table.ReadCaptured(index, *captured)` (same `invalidated` / `read_descriptors` / `descriptors[index]` bookkeeping as descriptor_table.h:60-87, no memory read); `captured == nullptr` → NULL view |
| `void FillImageViewsCaptured(std::span<ImageViewInOut>, std::span<const TicCapture>, bool compute, bool blacklist)` | committer | `FillImageViews` 556-575 (same retry loop; re-visits reuse the captured bytes) |
| `SamplerId GetSamplerIdCaptured(u32 index, const TSCEntry* captured, bool compute)` | committer | `GetSamplerId` 769-782 with `ReadCaptured` |
| `bool TryReuseGraphicsBindingsCaptured(views, memo_views, sampler_handles, memo_samplers, tics, tscs, blacklist, touch_tick)` | committer | 618-697 with `PeekUnchanged(gpu_memory, i)` → `table.PeekUnchangedCaptured(i, tics[k])` (`!invalidated && read bit && entry == descriptors[i]`) |
| `DescriptorTable::ReadCaptured(u32 index, const T&)`, `PeekUnchangedCaptured` | committer | descriptor_table.h:43-87 |
| `TryResolveViewCaptured`, `TryResolveSamplerCaptured`, versions, `texture_page_count` | phase 5 / I5 | — |

PipelineCache: `GraphicsPipeline* LookupGraphicsPipeline(bool& needs_create)` (SEQ, 735-752
without creating: on `graphics_cache` miss set `needs_create`), `GraphicsPipeline*
CreateCurrentGraphicsPipeline()` (SER: `CurrentGraphicsPipelineSlowPath` 1188-1211), and the
split `TickFrame`.

### 6.6 `ConfigureImpl` split (vk_graphics_pipeline.cpp:386-880)

Add to `GraphicsPipeline` (vk_graphics_pipeline.h):

```cpp
// SEQ: immutable after construction (stage_infos, masks, num_*); builds the CapturePlan once.
[[nodiscard]] const VideoCommon::MT::CapturePlan& CapturePlanFor(); // lazily, SEQ only
[[nodiscard]] const std::array<u32,5>& EnabledUniformBufferMasks() const { return enabled_uniform_buffer_masks; }
[[nodiscard]] const auto& UniformBufferSizes() const { return uniform_buffer_sizes; }
// committer: ConfigureImpl with captured inputs
bool ConfigureCaptured(bool is_indexed, const MT::ResolveInputs& in) { return configure_captured_func(this, is_indexed, in); }
template <typename Spec> bool ConfigureCapturedImpl(bool is_indexed, const MT::ResolveInputs& in);
bool (*configure_captured_func)(GraphicsPipeline*, bool, const MT::ResolveInputs&){};
```

`ConfigureCapturedImpl<Spec>` = `ConfigureImpl<Spec>` with these substitutions (everything else,
including `ConfigureDraw` 882-1001, unchanged):

| ConfigureImpl lines | Captured version |
|---|---|
| 409 `SynchronizeDescriptors(false)` | same (mirror regs) |
| 412 `SetUniformBuffersState` | same |
| 420-433 storage buffer part | `UnbindGraphicsStorageBuffers(stage)` + `BindGraphicsStorageBufferCaptured(stage, i, in.ssbos[k++])` |
| 437-511 handle reads | `views.push_back({.index = in.handles[v], .blacklist = desc.is_written (images) / false})`; samplers: `use_memo` → push `in.sampler_handles[s]`; else `GetSamplerIdCaptured(in.sampler_handles[s], tsc_ptr(s), false)` |
| 536-592 BackHelper fork | removed under MT (`forked=false`) |
| 633-664 memo | `TryReuseGraphicsBindingsCaptured(..., in.tics, in.tscs, ...)`; miss → `GetSamplerIdCaptured` |
| 665-669 `FillImageViews` | `FillImageViewsCaptured(views, in.tics, false, Spec::has_images)` |
| 671-729 texture-buffer binding | unreachable (forced SER) — `ASSERT(!Spec::has_texture_buffers && !Spec::has_image_buffers)` |
| 752-756 `UpdateGraphicsBuffers` + `BindHostGeometryBuffers` | `UpdateGraphicsBuffersCaptured(is_indexed, in)` = 1549-1566 with `SetCapturedIndexBuffer`, `SetCapturedVertexBuffer` (only for changed streams; flags still gate rebinds), `UpdateUniformBuffers` (FindBuffer on translated bindings: unchanged), `UpdateStorageBuffers` → `ResolveStorageBinding` (memo ok: `FindBuffer` only), `UpdateTextureBuffers` (none) ; then `BindHostGeometryBuffers` with `SynchronizeBuffer` bypassed and inline index bytes from `in.inline_index` (945-962) |
| 782 `BindHostStageBuffers(stage)` | `BindHostStageBuffersCaptured(stage, ubo_cursor)` |
| 794-795 `PushImageDescriptors` | same (marks written images, rescaling) |
| 865-874 | same |

`configure_captured_func` is built next to `configure_func` (vk_graphics_pipeline.cpp:362 /
`ConfigureFunc` ~250-264, `MakeConfigureSpecFunc` h:148-150).

Compute (packet mode): `ComputePipeline::ConfigureCaptured(KeplerCompute& mirror, Scheduler&,
BufferCache&, TextureCache&, const ResolveInputs&)` = vk_compute_pipeline.cpp:171-330 with the
same substitutions (`BindComputeStorageBufferCaptured`, `GetSamplerIdCaptured(…, true)`,
`FillImageViewsCaptured(…, compute=true)`, compute UBO captured bind, `capture_sync_bypass`).

---

## 7. Risks specific to this code base, and handling

| # | Risk | Handling |
|---|---|---|
| R1 | **Lazy guest reads on the committer through `SynchronizeBuffer`** (every Bind* path: 952, 1069, 1099, 1137, 1243, 1326, 1354, 1378, 1419, 1476, 1501, 1525, `InlineMemoryImplementation` 2473, `DMACopy` 345-346, `ObtainCPUBuffer` 433) — newer CPU bytes would reach older draws and break I1 | `capture_sync_bypass` during packet commits; every other committer call site is inside a Serial (memory == stream point) or a Command that is self-contained (`ApplyInlineUpload`). MT_CHECK asserts `!OnCommitter() || capture_sync_bypass || in_serial` inside `SynchronizeBuffer`/`MappedUploadMemory` |
| R2 | **Pipeline creation reads cbufs/TICs/code** (G19) and `ShaderInfo`/`graphics_key`/`current_pipeline` are mutated by `CurrentGraphicsPipeline` (incl. the stray call in `UpdateDynamicStates` 2018) | lookup SEQ, creation SER, committer never calls `CurrentGraphicsPipeline` outside a Serial; Serial paths set mirror `Shaders` dirty first (§4.1). Async VkPipeline builds read no guest memory; `TakeBackgroundPipeline` (prebuilt from disk) is creation → SER |
| R3 | **Compute exceptions are control flow** (`DispatchCompute` throws, `DispatchNativeCompute` catches 598-617, `UpscaleInsteadOfNativeProgram`) | default `serial` + rethrow on SEQ; packet mode only for dispatches whose pipeline exists and is not a stand-in/collision program |
| R4 | **Nx2 list copies** (`Nx2NoteListWrite` 1914-1951, `Nx2CopyListPages` 1954-2007, front `Nx2ReadListCopy` 2010-2059): copies are recorded on the committer after the writing dispatch; the front's `IsGpuWrittenCommands` → `WaitForWriters` must cover them; `Nx2NoteListHostWrite` on SEQ (from `AccelerateInlineToMemory` 1760/1764 and `WriteMemory` 204) would bump `writes` *before* the committer bumps it for an earlier GPU write → a later copy is taken as current while guest order is GPU-then-host → stale command words | `WaitForWriters`/`WaitForPosition`/`Drain` wait on `settled`; SEQ guest writes over a command-list page (`Tegra::Nx2AnyCommandListPage`) take the I5 barrier (`WaitCommitted()`) before writing and calling `Nx2NoteListHostWrite`. Draw-written list pages (`draw_written`, 1937-1941) are detected at commit → unchanged behavior |
| R5 | **I5 / sequencer guest writes over texture pages**: `ProcessCBMultiData` → `WriteBlockCached` memory_manager.cpp:548-551 (~every draw), I2M engine_upload.cpp:46/59/77, inline QMD kepler_compute.cpp:309, `TryNx2IndirectReset` 322, `StampQueryResult` maxwell_3d.cpp:636-644, DMA/Fermi2D software paths (`WriteBlock` → `InvalidateRegion`) | before any SEQ guest write: `if (Tegra::Nx2MayHoldTexture(dev,size) || Nx2AnyCommandListPage(dev,size)) pipeline.WaitCommitted();` then write, then (texture pages) `texture_cache.WriteMemory` synchronously under the texture mutex (instead of deferring to FlushCaching). Hook points: `MemoryManager::WriteBlockImpl` (single choke point for `WriteBlock*`), `RasterizerVulkan::AccelerateInlineToMemory`. Count barriers/s (MT_STATS); `EDEN_NX2_MT_STRICT_TEX` = barrier on every SEQ write. `Nx2MayHoldTexture` bits never clear → conservative; phase 2's `texture_page_count` (mt/page_counter.h) refines it |
| R6 | **Inline index draws**: `draw_state.inline_index_draw_indexes` is cleared on SEQ right after `ProcessDraw` (draw_manager.cpp:268) | bytes copied into the packet; `ForceIndexDirty` flag; committer uses its scratch vector (never the real one) |
| R7 | **Immediate payload queries** `CounterReport` 312-325 write guest memory at report time; on the committer they land after later SEQ captures and after the front's `NotePendingWrite(Position)` drops them | run that sub-case on SEQ (guest write + `Nx2SemaphoreWaiters::OnWrite`, no streamer slot) with the I5 check; everything else CMD. Verify with `EDEN_NX2_MT_VERIFY` that the payload/timestamp words match |
| R8 | **`gpu_modified_ranges` races** between capture-time marks and committer pops/downloads (`PopAsyncBuffers` 861-890, `DownloadBufferMemory` 2543-2600, `WriteMemory` 205-208, `InlineMemoryImplementation` 2467-2468) | §3.4: re-add in `CommitWrittenRange`; additionally keep an `inflight_written` RangeSet (added by `CaptureWrittenRange`, removed by `CommitWrittenRange`) and have download/pop paths not clear tracker GPU bits / `gpu_modified_ranges` inside it |
| R9 | **Rescaling** state is committer-only (`is_rescaling`, `Rescaled` flags, `ScaleUp/Down`, `InvalidateScale` dirtying RT flags, `RescaleViewports/Scissors` 936-937, `UpdateViewportsState` 2086-2087, `UpdateScissorsState` 2133-2136, Clear 925-928, DrawTexture 860-861) | nothing captured; blacklist (`views[i].blacklist`) comes from the plan. Make sure `InvalidateScale`/`DeleteImage` run only on the committer (ThreadEnginePtr resolves to the mirror there; assert role) |
| R10 | **Indirect reset** (`TryNx2IndirectReset` 258-330): reads after a possible download, then writes records via `AccelerateInlineToMemory` | SEQ, download SER, writes through R5. It must run after the SEQ pipeline lookup (it reads `shader_infos`) — same order as today |
| R11 | **Conditional rendering** `ProcessQueryCondition` 680-732: return value gates `execute_on` synchronously; HCR state is recorded into the scheduler | SER (counted); `FlushCaching` (1672) on SEQ before it |
| R12 | Query streamer prefix sums read guest memory (`dependant_address` vk_query_cache.cpp:1227) on the committer | written by GPU-side ops / committer-ordered; acceptable; verify counts |
| R13 | **`TickWork` sync requests** (composite without fences, `OnCPURead`, applet capture) were run by whoever ran `TickWork` | only the committer pops them (CMD); SEQ checks `HasSyncRequests()` (new atomic) to avoid an entry per draw |
| R14 | **Generic `DeferFunction` closures** (gmmu Map/Unmap, gsv events, `done` flags meaning "GPU finished") | SER: they run after everything earlier is committed (exactly the guest's expectation that unmaps follow the work). `before_later_reads` still drains the front |
| R15 | **`InvalidateGPUCache` on two threads** (SEQ via engines/puller, committer via `SignalFence`) | both under the cache mutexes; `OnCacheInvalidations` texture part touches only flags; shader cache has its own mutex. The `GatherGPUDirtyMemory` lock (core/gpu_dirty_memory_manager.h:51) serializes gathers |
| R16 | **`OwnsScheduler`-gated download in `BufferCache::WriteMemory`** 222-233 (keep page GPU data) | SEQ is not the owner → takes the "mark CPU-modified" path; equivalent to today's CPU-thread behavior. The committer still downloads on its own WriteMemory calls |
| R17 | **Channel switches**: `ChannelSetupCaches` pointers, `StateTracker::ChangeChannel`, buffer cache `channel_state` memos (`IndexBufferMemo`, `StorageBufferMemo` compare `channel_state`) | `BindChannel` SER + ChannelSync (drains); memos are touched by one thread each (SSBO memo on SEQ, index/vertex memo disabled under MT) |
| R18 | **Ring sizes**: one packet can hold 240 views + 192 samplers (`MAX_IMAGE_VIEWS` vk_rasterizer.h:176-178) → ~17 KiB TIC/TSC captures; 16 MiB stream ≈ 1000+ worst-case packets | assert/fallback to SER if a packet exceeds 64 KiB; `WaitForSpace` on the stream counts as "ring wait" in MT_STATS |
| R19 | **`uniform_buffer_skip_cache_size`** read on SEQ, written by committer `TickFrame` 167 | relaxed atomic_ref read; the decision travels in the capture |
| R20 | **SeqStagingRing pressure**: UBO staging per draw (MKW ~55-70k draws/s × few KB) | reuse memo (uniform_stream_reuse.h semantics keyed on bytes) + 64 MiB ring; when full the SEQ waits for `committed_seq` + tick release (counted) |
| R21 | **`LockCaches` on SEQ** (kepler QMD 353-357) and `PrepareDraw` early lock 351-356 | no-ops on SEQ under MT (SEQ only takes `buffer_cache.mutex` around capture) |
| R22 | **`DescriptorTable` sync on the committer uses mirror limits** while SEQ used real limits for `TextureHeaderIndex` | identical values by construction (mirror regs == real regs at the packet); VERIFY asserts `table.current_limit == regs.tex_header.limit` |
| R23 | **Vertex/index memos** (`EDEN_NX2_VERTEX_MEMO`, `EDEN_NX2_INDEX_MEMO`, both off by default) depend on `LayoutGeneration` read where translation happens | disabled under MT (translation moved to SEQ) |
| R24 | Fermi2D/DMA return values decide software fallbacks on SEQ | SER returning bool (phase 4) |
| R25 | `Scheduler::DeferFlush` + idle hook semantics: fences/list copies flushed late until the owner idles; the fencing thread and front `WaitSubmitted` must wake the committer | `RequestIdleHook` → committer under MT (§5.2) |
| R26 | `GpuSplit::Stop` with a live committer and pending SerialCells on a dying sequencer | Stop drains: sequencer finishes its ring, closes the stream with a final Mark, then the committer is joined before `back_running=false` |

---

## 8. Verification switches (phase 4)

* `EDEN_NX2_MT_LOCKSTEP=1`: after emitting each DrawPacket/ComputePacket/Command the SEQ calls
  `WaitCommitted(seq)`.
* `EDEN_NX2_MT_VERIFY=1` (implies LOCKSTEP): at each packet commit (SEQ blocked) compare (a) mirror
  regs vs real regs (first differing register logged), draw_state, `state.shader_stages`,
  `root_tables`; (b) every captured byte re-read from guest memory: handles (via the same reads),
  TIC/TSC 32-byte entries, staged UBO bytes, SSBO descriptor words, StagedUpload bytes (only
  where the tracker shows no CPU write since capture); (c) `ResolvedDraw` facts vs. an old-path
  dry computation of view ids / sampler ids via `VisitImageView`/`GetSamplerId` on a *scratch*
  table copy; log "MT verify: ..." every 10 s.
* `EDEN_NX2_MT_CHECK=1`: role asserts in `MemoryManager::Read*` (`!OnCommitter() || allowed`),
  `Scheduler::Record/Flush` (`OnCommitter()`), `BufferCache::FindBuffer` (`!OnSequencer()`),
  `TextureCache` structure mutators (`!OnSequencer() || in_i5_barrier`).
* `EDEN_NX2_MT_STATS=1`: SEQ busy/wait, committer busy/wait, Serial count by kind, I5 barriers,
  staging waits, stream waits, settled lag (`write_position - settled`), forced-SER reasons.

## 9. Suggested implementation order

1. `GpuSplit` additions (§5) with `mt_active=false` everywhere (no behavior change).
2. Committer thread + stream + Commands for the trivial entries (WaitForIdle, FragmentBarrier,
   ResetCounter, Query, SignalSyncPoint/Reference/Fence, ReleaseFences, FlushCommands, TickWork) and
   **everything else SER** (Draw included): validates roles, settled, idle hook, ThreadEnginePtr,
   mirror sync, StateTracker flags. Must run MKW correctly with LOCKSTEP off.
3. Clear as CMD; DrawPacket capture + `ConfigureCaptured` + staged uploads; VERIFY on.
4. I5 barrier + command-list barrier; immediate payload query on SEQ.
5. ComputePacket behind `EDEN_NX2_MT_COMPUTE=packet`.
