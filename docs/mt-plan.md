# Multithreaded GPU back (branch mkw-mt) — master plan

Goal: Mario Kart World (title 1) split screen at 60 fps (5800X 8C/16T, RTX 3070 Ti 8 GB).
The split back thread (GPUBack) is the CPU bottleneck: 8.5-10 us/draw at 1x, 15-18 at 2x;
single screen ~55-70k draws/s, split screen ~2x.

Measured, design around: fork/join per draw (-5 fps, ~0.5 us join vs ~1 us work); BackHelper
(texture half of a draw on a helper) -2..-10 fps; pinning neutral; guest CPU write traps take
the cache locks (~26 ms/s); VRAM ~7.3/8 GB at 2x (keep new rings in host memory).

Back profile (% of back): Execute 84; Maxwell3D::ProcessMethodCall 60 of which Draw 56:
ConfigureImpl 41 [BindHostStageBuffers 10.5 (UBO memcmp/memcpy 8.5), VisitImageView 9.6,
UpdateGraphicsBuffers 4.4, BindHostGeometryBuffers 4.3, PushImageDescriptors 3.4,
UpdateRenderTargets 2.2, own 3.9], FlushCaching 4.5, CurrentGraphicsPipeline 4, locks 2.9;
KeplerCompute dispatch 11-12; CallMethod 4-7; Execute own 4.

## Threads (pipeline, not fork/join; batches of ~16 draws)

| Thread | Owns | Per draw |
|---|---|---|
| Front GPU (unchanged) | pushbuffer, puller, front macros, FrontState | — |
| Sequencer (GPUBack, GpuSplit::BackLoop) | real Maxwell3D/KeplerCompute/2D/DMA/I2M engines; all guest memory reads/writes by engines; MemoryManager::FlushCaching; buffer memory tracker (with traps, under tracker_mutex); ShaderCache::RefreshStages; capture plans; SeqStagingRing producer | methods, FlushCaching, capture (handles, TIC/TSC bytes, UBO bytes, SSBO addresses, translation, eager staging of CPU-modified bound ranges), emit deltas + packet |
| Resolver xK (EDEN_NX2_MT_RESOLVERS, default 2) | own Maxwell3D mirror, descriptor arena, descriptor-buffer blocks | pipeline key/lookup, TIC->view, TSC->sampler, buffer range lookup, descriptor payload -> ResolvedDraw (no locks) |
| Committer (GPUCommit) | Scheduler (only recorder), own mirror (exact dirty flags), cache structure, query cache, fence manager, pipeline creation, compute execution, idle hook | validate ResolvedDraw, apply staged uploads + intents, UpdateRenderTargets, CheckFeedbackLoop, query segment, UpdateDynamicStates, RequestRenderpass, bind elision, record, FlushWork, TickWork |
| VulkanWorker, fencing thread | unchanged | |

## Invariants (assert with a thread-local role)
- I1 guest memory is read only on the sequencer (exception: committer texture uploads/image creation, protected by I5).
- I2 only the committer touches the Scheduler, StagingBufferPool, FenceManager, QueryCache, DescriptorBufferRing (except per-resolver blocks).
- I3 only the committer changes cache structure (slot insert/erase, page_table, view/sampler maps, aliases, LRU, rescale, framebuffers). Resolvers take no cache lock.
- I4 every resolver result is validated at commit (per-image version, slot generation, buffer_epoch, pipeline_epoch); mismatch -> committer re-resolves with ExclusiveAccess.
- I5 texture-page write barrier: before the sequencer writes guest memory overlapping a registered image it waits for the committer (WaitCommitted), writes, invalidates synchronously (texture_page_count per 64 KiB page, precise check under texture mutex).
- I6 guest/front-observable effects (fences, syncpoints, query releases, downloads, list copies) run as ordered Commands on the committer; the front waits on `settled`, not `consumed`.

## Stream / batches
DrawStream: SPMC ring (16 MiB u64 words), every consumer reads all of it with its own cursor;
producer reclaims to the slowest. Entries: Deltas (method|value<<32 pairs), CbBind, RootTable,
DrawPacket, ComputePacket, Command (async ordered), Serial (sequencer blocks until run),
BatchEnd(i), Mark(ring_position), ChannelSync. Batch = up to N draws (EDEN_NX2_MT_BATCH default
16, adaptive 4-64), closed early when the GpuSplit ring runs empty, when the front sets
wait_target, on Serial, or when space is low. Batch i -> resolver i % K. Results: ResolvedDraw
slot ring (4096, by draw_seq & mask) + resolver arena; one atomic publish per batch; committer
spins briefly then futex.

## Register capture
RegWriteSet (4096-bit bitmap) set in Maxwell3D::ProcessDirtyRegisters (covers ConsumeSinkImpl,
CallMethod, inline index), ApplyState, ProcessCBMultiData (const_buffer.offset),
ProcessFirmwareCall4, any direct regs write found by the mirror check. Before each packet/command
the sequencer emits Deltas for set bits and clears. Pseudo-deltas: CbBind{stage,slot,enabled,
gpu_addr,size} from ProcessCBBind; RootTable; DrawState copy in the packet. Each consumer owns
full Maxwell3D/KeplerCompute mirrors (same constructors), dirty.tables copied after
SetupDirtyFlags, regs/state copied at start and ChannelSync; applying a delta = ApplyState
(store + set flags from dirty.tables). Resolvers force-dirty buffer/vertex/index flags at each
batch start. ThreadEnginePtr<T> (thread-local override proxy) replaces ChannelSetupCaches::
maxwell3d/kepler_compute and GraphicsPipeline::maxwell3d; StateTracker::flags points at the
committer mirror under MT.

## DrawPacket (sequencer, in RasterizerVulkan::Draw when MT active)
1 FlushCaching (MT: OnCacheInvalidations marks only buffer tracker + shader cache; textures via I5)
2 TryNx2IndirectReset stays here
3 RefreshStages(hashes) on real engine; plan_cache[hashes] -> immutable CapturePlan from
  Shader::Info: handle reads in exact ConfigureImpl view order (texture-buffer, image-buffer,
  texture, image descriptors with cbuf_index/offset/size_shift/count/secondary/shift_left/
  handle_program/is_written/blacklist), enabled UBO masks/sizes, SSBO descriptors, flags.
4 handles via gpu_memory->Read<u32> from shader_stages const buffers (or EvaluateTextureHandle)
5 TIC/TSC 32 B entries copied (replaces DescriptorTable::Read)
6 UBO: bank-17 root-table hack; fast path (<= skip_cache_size, !IsRegionGpuModified) -> stage in
  SeqStagingRing with the reuse memo from uniform_stream_reuse.h -> {VkBuffer, devaddr, offset,
  size}; cached path -> {DAddr, size} + stage CPU-modified parts
7 SSBO: read address/size words, translate; written ones marked GPU-modified in tracker now
8 vertex/index ranges translated; for each bound range ForEachUploadRange: copy CPU-modified
  bytes into SeqStagingRing, clear bits, StagedUpload{DAddr,size,ref} (tracker_mutex once/draw)
9 forced slow path: vertex repack, quad/uint8 index conversion, transform feedback, indirect,
  mesh, texture/image buffers, rare stuff
10 emit deltas, then packet
Packet ~0.6-1.5 KB: header {seq, ring_position, flags, draw params, instances, plan*,
hashes[6], zpass_enable, capture_write_gen} + TicCapture[] TscCapture[] UboCapture[]
SsboCapture[] VertexCapture[] IndexCapture StagedUpload[] inline index bytes.
ComputePacket analog (launch_description copy, compute deltas, plan, handles/TIC/TSC from QMD
cbufs, UBO staging, SSBO with GPU-modified marking, staged uploads, indirect bytes).

## Resolver (ResolveDraw<ResolverAccess>, vk_mt_resolve.h)
Apply all deltas to mirror; for own batches under EpochGuard: FixedPipelineState::Refresh ->
L1 -> lock-free pipeline map (miss=slow); views via L1 then SingleWriterMap<TICEntry,
ImageViewId>, check image version + atomic flags (CpuModified/HasAliases/sparse/Dead/blacklisted
Rescaled -> slow); samplers via map, create under sampler_create_mutex on miss; buffers via
atomic_ref page_table loads (miss=slow), record buffer_epoch; descriptor payload in
ConfigureImpl order (BindHostStageBuffers order, then PushImageDescriptorsResolved collecting
written images as intents) into resolver DescriptorArena / descriptor-buffer block; vertex/index
bindings; output ResolvedDraw{seq,status,pipeline,epochs,descriptor ref,rescaling,render_area,
vertex/index,view ids,image refs{id,version,written,blacklist},buffer write intents}.

## Committer
Apply deltas to mirror in order. Per draw packet: wait batch, validate (I4) else
ResolveDraw<ExclusiveAccess> under texture+buffer mutex (VisitImageViewCaptured, FindBuffer with
creation, CurrentGraphicsPipelineSlowPath); ApplyStagedUpload (FindBuffer + CopyBuffer,
CanReorderUpload) before anything else; intents (MarkModification, MarkWrittenBuffer commit side,
LRU touch dedup per frame, scale_rating=0 for blacklisted); UpdateRenderTargets(false),
CheckFeedbackLoop; query NotifySegment/CounterEnable; UpdateDynamicStates; RequestRenderpass,
is_built wait, UpdateRescaling, UpdateGraphicsPipeline, UpdateDescriptorBufferChunk, elision vs
true order; record; FlushWork, TickWork; publish settled + arena release seq.
Commands (Clear, Query, ResetCounter, SignalFence/SyncOperation/SignalSyncPoint/SignalReference/
ReleaseFences, WaitForIdle, FragmentBarrier, TiledCacheBarrier, InvalidateGPUCache,
FlushCommands, TickFrame, InlineUpload, ComputePacket) run existing code on the mirror.
Serial (DrawIndirect, DrawTexture, AccelerateSurfaceCopy, AccelerateDMA (phase 4), FlushRegion/
downloads, BindChannel, anything returning a value): old code while sequencer blocked.

## Ordering table
CB/I2M guest writes: sequencer in order, readers captured before. Buffer uploads: staged at
capture, applied in order at commit. GPU-modified: tracker bit at capture, download ranges at
commit. Textures: committer only + I5 (+ seq_write_gen hazard counter; EDEN_NX2_MT_STRICT_TEX).
Render targets/passes/feedback/rescale: committer. Query cache: committer (conditional render
stays sequencer, Serial if flush needed). Fences/semaphores: Commands. Compute->draw:
ComputePackets on committer in order with existing barriers; Nx2CollectListCopies/
SubmitNx2ListCopies around dispatch on committer. I2M: sequencer writes; if range GPU-modified
stage + InlineUpload command else mark CPU-modified. DMA/Fermi2D: Serial (phase 4), packetized
(phase 7). GPU-data reads: Serial from sequencer; GpuSplit::Run-style post from CPU threads.
Channel switch: drain + ChannelSync.

## Lifetimes
Only committer records -> worker order = guest order. DescriptorArena per resolver (ring of
DescriptorUpdateEntry 2^18) released by worker-side `executed` store. DescriptorBufferRing
AllocateBlock(64 KiB per batch) under block_mutex, stamped with tick by committer.
SeqStagingRing: host-visible Upload memory 64 MiB, seq-tagged, freed by committer {seq,tick}
records at SubmitExecution; reuse bumps max_ref_seq. EBR: global epoch, per-resolver announce,
committer retire lists -> then DelayedDestructionRing.

## Front interaction
`settled` atomic (committer stores ring_position of last finished item; sequencer CAS-max to
consumed when stream empty). Drain/WaitForPosition/WaitForWriters wait settled. Batch closed +
Mark on wait_target. BackQuiet requires committer quiet. OwnsScheduler -> committer. Idle hook
on committer. BackHelper disabled under MT.

## Concurrent caches
Primitives (src/video_core/mt/): StableSlotVector (SlotVector-compatible, chunked 1024, never
relocates, per-slot generation, EBR free), SingleWriterMap (open addressing, release publish,
tombstones, EBR resize), AtomicEnumFlags (image flags), spmc_stream, arena, epoch, engine_ptr,
reg_delta, draw_packet, capture_plan, mt_switches.
Texture cache: slot vectors -> StableSlotVector; ImageBase atomic flags + version (bump on
CpuModified, alias change, scale, delete (Dead), sparse remap) + last_touch_frame; view/sampler
map twins; TryResolveViewCaptured/TryResolveSamplerCaptured (resolvers),
VisitImageViewCaptured/GetSamplerIdCaptured (committer); texture_page_count.
Buffer cache: tracker under tracker_mutex (sequencer, traps, committer downloads); structure
committer under mutex; page_table release stores via atomic_ref; buffer_epoch; CaptureUploadRanges,
TryResolveRange, ApplyStagedUpload, CommitWrittenRange.
Pipeline cache: RefreshStages on sequencer; SingleWriterMap<KeyRef, GraphicsPipeline*> twin;
pipeline_epoch; immutable ResolveInfo accessor; compute pipelines committer only.
Descriptors: DescriptorArena per resolver; DescriptorBufferRing::AllocateBlock.
Query cache, fence manager: committer only.

## Phases (each compiles; EDEN_NX2_MT unset = today's path)
0 counters (TICs/UBOs/SSBOs per draw, texture-page writes/s, plan hit rate), GPU timestamps
  (EDEN_NX2_GPU_TIMESTAMPS), staging/descriptor/semaphore wait timers.
1 primitives + EDEN_NX2_MT_SELFTEST stress tests from GpuSplit::MaybeStart.
2 behaviour-neutral DS changes (StableSlotVector, atomic flags, versions, epochs, map twins,
  ThreadEnginePtr, settable StateTracker flags).
3 register mirrors without threads (EDEN_NX2_MT_MIRROR=N): mirror memcmp every N draws, log first
  differing register.
4 capture + committer thread, K=0 (EDEN_NX2_MT=1, EDEN_NX2_MT_RESOLVERS=0, EDEN_NX2_MT_COMPUTE=
  serial|packet) + EDEN_NX2_MT_LOCKSTEP + EDEN_NX2_MT_VERIFY.
5 resolvers + lock-free reads (EDEN_NX2_MT_RESOLVERS=N, EDEN_NX2_MT_SHADOW), 5b per-pass POD replay
  (EDEN_NX2_MT_REPLAY).
6 lock scoping (EDEN_NX2_MT_TRAPLOCK).
7 sequencer diet (EDEN_NX2_MT_UBO_GEN, SPLIT_STATE re-test, merged FlushCaching+staging,
  DMA/Fermi2D packets EDEN_NX2_MT_DMA=packet).
8 optional optimistic reads / compute resolution in resolvers.

## Verification
MT_LOCKSTEP (sequencer waits per packet), MT_VERIFY (old path run for real on committer and
compared field by field: descriptor payload, pipeline, view/sampler ids, vertex/index binds,
draw params, rescaling/render area; captured bytes re-read and compared; mirror vs real regs;
logs "MT verify: ..." every 10 s), MT_SHADOW, MT_STATS (stage busy/wait, batch sizes, slow paths
by reason, re-resolves, I5 barriers, hazards, ring waits, settled lag), MT_CHECK thread-role
asserts in MemoryManager reads, MT_POISON.

Rules: never edit any CMakeLists.txt; new code header-only; keep EDEN_NX2_* switches;
commit as Damfortx <damfortx@gmail.com>; never push.
