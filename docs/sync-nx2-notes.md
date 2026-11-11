# Sync with upstream nx2 (branch mkw-sync, 2026-11-10)

`git merge origin/nx2` into mkw-c11 (merge base e736dfde9e, upstream 234 commits ahead, ours
218). 37 files conflicted (~118 hunks). Most hunks were upstream commits we had already
cherry-picked (Ocarina lowering, async translation, background pipelines, split owner checks):
those took whichever side was the superset. The non-trivial resolutions:

## Storage buffers (buffer_cache.h / buffer_cache_base.h / buffer_cache_mt.h)

- Upstream 4ac9e703fe (OoT vegetation: pointers loaded from a table, `indirect_offset`) and
  405e5f7ac8 (native graphics pointers are bare, sized from the mapping) are ported into our
  `MemoStorageBufferBinding` / `StorageBufferBindingFromDescriptor` split. New `table_pointer`
  argument on all three (and on `MtStorageBinding`). Semantics:
  - size: table && !written -> min(mapping, 64 MiB); `mapping_sized` -> min(mapping, 8 MiB);
    else the NVN size word;
  - the size word is not read for read-only table pointers nor for mapping-sized ones;
  - MaxContinuousRange clamp for `native_pointer || mapping_sized || table_pointer`.
  - graphics: native_pointer = mapping_sized = `native_turing` (upstream 405e5f7ac8);
    compute: native_pointer = IsNativeMode (clamp), mapping_sized = IsDlssDispatch (upstream
    8820bb79aa semantic); table_pointer = `desc.indirect_offset != ~0U`.
- Upstream's thread_local StorageBufferBinding cache (c8415d0e80, MapSerial) is NOT taken: our
  per-slot memo (EDEN_NX2_SSBO_MEMO, LayoutGeneration) covers the same; memo flags gain bit 8
  for table pointers. `map_serial` is kept in MemoryManager (upstream code bumps it).
- MT sequencer (vk_mt_pipeline.h, graphics and compute capture) mirrors the same: holder read
  through SeqRead, table/native flags, size word only when read. c11_front.h: a written SSBO
  behind a table marks the program FlagUnknown (writer ranges unknown).
- holder reads in BindGraphics/ComputeStorageBuffer and shader_handle_program.h go through
  MT::ReadCaptured (falls back to memory when not captured).
- Upstream fe2cd8be2b (don't download GPU data over newer CPU writes): `WriteDownloadedMemory`
  taken at every download site, keeping our `MT::BumpWriteGeneration` before each (also added
  to upstream's new mapped-buffer download path). `ClaimCpuWrites` takes the tracker scope and
  runs at the start of our batched `RasterizerVulkan::OnCacheInvalidations` (the MT sequencer
  path, SeqCacheInvalidations, does not claim first).
- QueueFenceDownload (ours, with the preflush switches) kept; upstream QueueGpuWrite left
  unused (equivalent to our pages-only path).
- WriteMemory keep-page path: our Nx2KeepPageGpuData modes kept (MKW default None unchanged);
  in Download mode upstream 575d0a5bfd's inline upload applies (EDEN_NX2_INLINE_PAGE_WRITES,
  default g_nx2_ampere) with its IsRegionRegistered guard; mode Inline (=2) always inlines.
- Uniform buffers: upstream native bank re-upload (031e07436c/5c7da2713a/8b3aff59e4,
  EDEN_NX2_BANK_REUPLOAD, default '1' once per frame per 64 KiB native bank) placed before our
  MT_LITE_UBO capture logic, under MT::TrackerScope. Applies to MKW too (native tiling);
  EDEN_NX2_BANK_REUPLOAD=0 restores the c11 behaviour if it costs MKW time.

## GPU split / engines

- gpu_split.h/.cpp: ours kept (MT-aware OwnsScheduler, BackQuiet, Drain, Run/RunFlush with
  DrainReason). CheckOwner (Linux): MT role check first, then upstream's stack-trace report.
- dma_pusher.cpp: upstream's "entry parsed from memory differs from its submit-time copy"
  diagnostic kept, skipped for GPUPARAM calls (gpuparam_extra >= 0). Our RunFlush/DrainReason
  calls and FRONT_BATCH/MT_LITE_FCALL paths kept.
- Upstream 66b9ff826f (ReleaseFront when the split stops): kept, but the trigger in
  Maxwell3D::CallMethod/CallMultiMethod and KeplerCompute::CallMethod also requires
  `!GpuSplit::Get().Active()`, since under the MT back threads other than the back run engines.
- puller.cpp: our site stats/async fences kept; upstream's periodic ReleaseFences while a
  channel waits under delayed fences added before our suspension report.
- kepler_compute.h: our MtDispatchState kept plus upstream's Ocarina movie-network state
  (native_upscale_input, network_skip_remaining, RememberNativeUpscaleInput). These live on the
  engine that runs ProcessLaunch, so no MT mirroring was added.
- gpu.cpp MapInOrder: both our maps_queued/maps_done counters (WaitForQueuedMaps) and
  upstream's pending-map list (IsMapPending) in one function.
- memory_manager: our range cache (layout_generation) kept; upstream's duplicate
  MaxContinuousRangeUncached declaration dropped. Upstream's 4096-entry cache not taken (ours is
  512 entries; OoT may miss more).

## Vulkan

- vk_rasterizer.cpp PrepareDraw: upstream's draw diagnostics (diag_outcome, DiagRecordDraw,
  frame dump, EDEN_NX2_DIAG_SAMPLES query) moved after our ConfigureDrawPipeline (which records
  the dynamic state). A duplicated `record_read_barrier` and duplicated
  `GraphicsPipeline::RunBuild` from the auto-merge removed.
- vk_pipeline_cache.cpp: CACHE_VERSION = 31 (upstream; caches retranslate once). Our async
  translation / RunOnCommitter paths kept; `graphics_miss = "async-build"` set where a draw is
  skipped waiting for a translation.
- vk_compute_pipeline.h: upstream ShaderHash/WrittenImages/SampledImages plus our GetInfo /
  UniformSizes.

## Shader recompiler

- lower_to_maxwell.h: upstream side everywhere (supersets of our cherry-picks: SUATOM/3D SURED,
  F2I cbuf abs/neg, 2D array gathers with shadow_boundary, more SHF forms).
- global_memory_to_storage_buffer_pass.cpp: upstream's TrackPointerAddend/TrackAddend chain with
  our `exhausted` sets on cached_track.

## Not taken

- origin/fix/oot-map-change (53 commits past nx2): not an OoT map-change fix but an integration
  branch (MCP server, driver shader cache settings, compute precompile, timeline/hazard
  diagnostics, MSVC fixes, several commits we already cherry-picked). Not self-contained, left
  out.

## Follow-ups found after the merge

- Upstream de4ed9d9d7 raised compute storage buffers to 64 (NUM_COMPUTE_STORAGE_BUFFERS):
  our `compute_ssbo_memo` array is sized to it now (was NUM_STORAGE_BUFFERS = 32).
