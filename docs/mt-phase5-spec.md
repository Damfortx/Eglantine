# MT phase 5 / 5b — K parallel resolvers with lock-free cache reads, per-pass POD replay

Implementation spec for phase 5 of `docs/mt-plan.md` (`EDEN_NX2_MT_RESOLVERS=K`, `EDEN_NX2_MT_SHADOW`)
and phase 5b (`EDEN_NX2_MT_REPLAY`). With `EDEN_NX2_MT` unset, or `EDEN_NX2_MT_RESOLVERS=0`, every
path must stay byte-for-byte what phase 4 does.

Snapshot: HEAD `05d55b84cc` (phase 4 steps 1-2) plus the uncommitted phase-4 work in progress
(`vk_mt_pipeline.h` draw/compute packets, `buffer_cache_mt.h`, `mt/capture.h`, `ReadCaptured` in
`descriptor_table.h` / `shader_handle_program.h` / `vk_graphics_pipeline.cpp` / `buffer_cache.h`).
Another agent is still editing these files, so line numbers will drift. **Use the function names
as anchors.** The numbers given are from this snapshot. Paths are relative to `src/video_core/`.

Rules carried over: never edit any `CMakeLists.txt`; new code goes in headers; keep the
`EDEN_NX2_*` switches; commit as Daniel Medina; never push.

---

## 0. What phase 4 actually built, and what phase 5 starts from

The phase-4 implementation differs from `mt-phase4-spec.md` §6.4-6.6 in one way that shapes
phase 5. **No `ConfigureCaptured`/`ResolveInputs` split was written. The committer runs the
unchanged old code against a capture overlay.**

* SEQ: `Nx2DrawPipeline::SeqDraw` (vk_mt_pipeline.h:1322). It runs `FlushCaching`, then
  `pipeline_cache.CurrentGraphicsPipeline()` **on SEQ**, then `TryNx2IndirectReset`, then
  `MeshDrawAllowed`, then `CaptureDraw` (:1021), then `EmitDraw` (:1260).
* `CaptureDraw` records every guest read that `ConfigureImpl` will repeat (`SeqRead<T>`,
  `SeqReadDescriptor`): handles, `EvaluateTextureHandle` reads, TIC/TSC 32-byte entries, and SSBO
  descriptor words. They are stored as a sorted, merged **overlay** (`OverlayEntry{gpu_addr,size,
  offset}` into a blob, `mt/capture.h`). It also stores `UboEntry{stage,index,staged,size,
  device_addr,offset}`, with the bytes of staged UBOs in the blob, and `UploadEntry`, which holds
  the CPU-modified ranges taken out of the tracker by `BufferCache::MtTakeUploads`
  (buffer_cache_mt.h). Written SSBOs are marked through `MtCaptureWrittenRange`.
* Committer: `Nx2DrawPipeline::CommitDraw` (:1359) writes the draw state into the mirror and calls
  `FlushWork`. Under both cache mutexes it then calls `MtApplyUploads`, sets
  `MT::t_capture = &view`, and runs **`RasterizerVulkan::ConfigureDrawPipeline`**
  (vk_rasterizer.cpp:523: `SetEngine`, vertex repack, `pipeline->Configure` → `ConfigureImpl`,
  `UpdateDynamicStates`, query segment) followed by `RecordDraw` (:556). The cache read sites
  consult `t_capture` first: `DescriptorTable::Read/PeekUnchanged`, `ReadCaptured<T>` in
  `ConfigureImpl`'s `read_handle`, `EvaluateTextureHandle`, `StorageBufferBinding`,
  `MemoStorageBufferBinding`, the `BindHostGraphicsUniformBuffer` captured-decision branch, and
  `SynchronizeBuffer`, which returns `true` while a capture is active.

Consequences for phase 5:

1. **The committer fallback (`ResolveDraw<ExclusiveAccess>`) already exists.** It is
   `CommitDraw`'s overlay path. Phase 5 adds the resolver result as an accelerator in front of it.
   A packet that fails validation or resolution simply runs the phase-4 path. The packet therefore
   keeps every phase-4 field.
2. **The pipeline lookup is on SEQ** (`SeqDraw` → `CurrentGraphicsPipeline`), and a miss creates
   the pipeline through `RunOnCommitter` (vk_pipeline_cache.cpp:1188
   `CurrentGraphicsPipelineSlowPath`). The packet carries `GraphicsPipeline*`. §7.3 explains why
   that stays the default.
3. A resolver reads handles and TIC/TSC bytes from **the same overlay**. It installs its own
   thread-local `t_capture` (thread_local, so this is safe per thread) and runs the same
   `read_handle` logic against its mirror's `state.shader_stages[].const_buffers`. **No new
   texture capture is needed.** Phase 5 does add translated buffer ranges (§4.3), because
   translation reads the GMMU page table, and Serial Map/Unmap closures change that table while
   resolvers run.
4. Phase 2 left several pieces unwired: no `EpochManager` instance exists, and no
   `SetEpochManager` call is made, so `StableSlotVector::erase` destroys the object and reuses the
   index at once. There is no pipeline twin map or `pipeline_epoch`, and no `TryResolve*`. Phase 5
   builds them (§13 steps S1-S5).

---

## 1. Switches

| Switch | Default | Meaning |
|---|---|---|
| `EDEN_NX2_MT_RESOLVERS=K` | 2 (`mt_switches.h::Resolvers`) | K resolver threads; 0 = phase 4 (committer resolves everything) |
| `EDEN_NX2_MT_BATCH=N` | 16 | draws per batch (max; closed early, §4.1) |
| `EDEN_NX2_MT_SHADOW=1` | off | resolvers run; the committer also runs the phase-4 path, compares field by field, and **records the phase-4 result**. Implies nothing about lockstep |
| `EDEN_NX2_MT_PIPE=seq\|resolver` | `seq` | where `FixedPipelineState::Refresh` + key lookup run (§7.3) |
| `EDEN_NX2_MT_RDESC=1` | on if descriptor buffers are in use | resolvers write descriptor-buffer bytes (`AllocateBlock`, §9.2); off = the committer writes them from the arena payload |
| `EDEN_NX2_MT_RUBO=1` | on | resolvers stream small UBOs into a per-resolver ring (§9.3); off = streamed UBO entries are left as patch records for the committer |
| `EDEN_NX2_MT_EBR` | on with MT (`DeferredFree`) | attaches the `EpochManager` to slot vectors / twin maps |
| `EDEN_NX2_MT_STATS=1` | off | adds the §11 counters to the 10 s log |
| `EDEN_NX2_MT_REPLAY=1` | off (5b) | per-render-pass POD replay (§12) |
| `EDEN_NX2_MT_RCHECK=1` | off | resolver-side asserts: role, guard held, no mutex taken except `sampler_create_mutex` |

Add these to `mt/mt_switches.h` (or `mt/role.h`, next to `Lockstep`/`Verify`), in the same
`Tegra::Nx2EnvMode`/`Nx2Switch` style.

---

## 2. Threads and data flow

```
front GPU ──ring──▶ SEQ (GPUBack) ──SpmcStream (16 MiB)──┬──▶ Committer (GPUCommit)  consumer 0
                                                          ├──▶ Resolver 0 (GPUResolve0) consumer 1
                                                          └──▶ Resolver K-1            consumer K
Resolver r ──ResolvedDraw slot ring + per-resolver arenas──▶ Committer (per-batch publish)
Committer ──scheduler.Record──▶ VulkanWorker ──▶ GPU
VulkanWorker ──(executed position)──▶ DescriptorArena[r].Release
Committer ──tick stamps──▶ DescriptorBufferRing blocks / ResolverUniformRing  (freed on IsFree(tick))
```

* Every consumer reads **every** entry. Resolvers apply all Deltas, CbBind and RootTable entries to
  their own mirror, but resolve only the DrawPackets of batches with `batch % K == r`. They skip
  Command, Serial, ComputePacket and Mark entries, apart from the barrier handling in §3.3.
* Producer reclaim is to the slowest consumer (`SpmcStream::SlowestCursor`), so a stalled resolver
  stalls SEQ when the ring is full. Ring waits are counted (`stream_full_ns`).
* Ordering stays as in phase 4: only the committer records, so worker order equals guest order.

---

## 3. Resolver thread lifecycle

### 3.1 Objects (new header `renderer_vulkan/vk_mt_resolve.h`, included by vk_mt_pipeline.h)

```cpp
namespace Vulkan::MT {

struct ResolverMirrors {                 // one per channel the resolver has seen
    Tegra::Engines::Maxwell3D* real3d{};
    std::unique_ptr<Tegra::Engines::Maxwell3D> mirror3d;  // same ctor as the committer's (vk_mt_pipeline.h BindCurrentChannel)
    TextureCacheChannelInfo* tex_channel{};               // captured at ChannelSync (deque: stable)
    BufferCacheChannelInfo* buf_channel{};                // read-only use: none in phase 5 (kept for asserts)
};

class Resolver {
public:
    Resolver(Nx2DrawPipeline& pipeline, RasterizerVulkan& rasterizer, u32 index);
    ~Resolver();                         // Stop()
    void Start();                        // consumer = stream.AddConsumer() BEFORE SEQ emits (SEQ quiescent)
    void Stop();                         // request_stop + WakeAll + join + RemoveConsumer
    [[nodiscard]] u64 PublishedBatch() const noexcept { return published.load(std::memory_order_acquire); }

private:
    void Loop(std::stop_token stop);
    void OnEntry(const u64* entry);
    void ResolveBatch(u64 batch, const u64* first_packet);   // §4
    bool ResolveOne(const DrawHeader&, Spans&, ResolvedDraw& out);   // ResolveDraw<ResolverAccess>
    void ChannelBarrier(const u64* entry);                   // §3.3

    Nx2DrawPipeline& pipeline;
    RasterizerVulkan& r;
    const u32 index;
    u32 consumer{};
    int epoch_reader = -1;                                   // EpochManager::RegisterReader
    std::vector<std::unique_ptr<ResolverMirrors>> mirrors;
    ResolverMirrors* current{};
    // Engine overrides live in the thread's g_engine_overrides (thread_local), set in Loop.
    DescriptorArena arena;                                   // §9.1
    ResolverUniformRing ubo_ring;                            // §9.3
    DescriptorBlockCursor desc_block;                        // §9.2
    VideoCommon::MT::BumpArena scratch{1 << 20};             // per-batch scratch (views, temp vectors)
    ResolveL1 l1;                                            // §7.1 small direct-mapped caches
    PipelineMirrorKey pipe_key;                              // §7.3 (EDEN_NX2_MT_PIPE=resolver)
    std::jthread thread;
    alignas(64) std::atomic<u64> published{0};               // last batch whose results are complete
    alignas(64) std::atomic<u64> busy_ns{}, wait_ns{}, draws{}, slow{};
};
}
```

`Nx2DrawPipeline` gains `std::vector<std::unique_ptr<MT::Resolver>> resolvers;`,
`MT::ResolvedRing resolved;` (§4.2) and `VideoCommon::MT::EpochManager epochs;`. The epoch manager
may instead live on the rasterizer, which outlives the caches' use of it (see S1).

### 3.2 Start / Stop order

`Nx2DrawPipeline::Start` (vk_mt_pipeline.h:431):

1. Create the committer consumer, as today.
2. `if (Resolvers() > 0)` create K `Resolver`s. Each calls `stream.AddConsumer()` **before** the
   committer thread starts, while SEQ is not yet the sequencer (`Start` is called by the front,
   before the back thread runs), so every consumer begins at write position 0.
3. Resolver `Loop` prologue:
   * `Common::SetCurrentThreadName(fmt::format("GPUResolve{}", index))`;
     `Tegra::Nx2ApplyTimerSlack()`; `g_role = Role::Resolver`; `ThreadPriority::High`, not
     Critical: the committer and SEQ have priority. Then `SetCurrentThreadToPerformanceCores`.
   * `epoch_reader = epochs.RegisterReader()`, and ASSERT it is ≥ 0.
   * `BindChannel()`: the resolver's own mirrors of `rasterizer.maxwell3d.Real()`, built exactly as
     `Nx2DrawPipeline::BindCurrentChannel` builds them (vk_mt_pipeline.h:624): copy regs, state,
     root_tables, `dirty.tables`, draw_state and `native_turing`, then `dirty.flags.set()`. Then
     install `g_engine_overrides<Maxwell3D>.pairs[n] = {real3d, mirror}` and do the same for
     KeplerCompute, for safety. **It must not touch `StateTracker`, `real3d->mt_*` or
     `mt_record`; only the committer does.** Copying from the real engine is legal only while SEQ
     is quiescent, which holds at Start (before the back loop runs) and at a channel barrier.
   * Set the ready flag. `Start` waits for every resolver and the committer.
4. The `Stop` order matters. First, SEQ is already stopped (`GpuSplit::Stop` joins the back
   before calling `MtBackInterface::Stop`). Next, the committer drains: it consumes to
   `WritePosition` and waits for the resolver batches it needs. After that, stop the resolvers
   (`request_stop`, `stream.WakeAll()`, `join`, `RemoveConsumer`). Last, stop the committer. A
   resolver in `WaitForData` wakes through `WakeAll`. One stuck on arena or block space must
   also check `stop_requested()` in its wait loops.
5. `UnregisterReader` in the resolver epilogue. The committer's last action is
   `epochs.Collect()`, which runs everything still retired.

### 3.3 Channel switch barrier (`ChannelSync`)

Phase 4 rebinds mirrors inside `RunSerial` after the Serial `BindChannel` returns
(vk_mt_pipeline.h:861-896), while SEQ is still blocked. Resolvers need the same quiescent moment.

* SEQ: `RasterizerVulkan::BindChannel`'s `MT_SERIAL(Channel, ...)` (vk_rasterizer.cpp ~2972)
  becomes `mt->SerialBarrier(Kind::Channel, fn)`. It closes the batch, emits
  `Serial{..., barrier_acks*}` and waits until `done && acks == K`.
* Committer: runs the Serial as today, then `BindCurrentChannel()` as today, then
  `done->store(1)`.
* Resolver: on a Serial entry whose `aux` carries the `Barrier` bit, spin until
  `done->load() == 1`. The committer has run it and SEQ is still blocked, because SEQ waits for
  the acks. Then `BindChannel()` (copy from the real engine again), then `acks->fetch_add(1)` and
  notify.
* Other Serials need no resolver action. They run old code against the real engines with the
  committer's flags (`RunSerial`), and resolvers never read the real engines outside a barrier.
  Serial `UnmapMemory` / `ModifyGPUMemory` change the GMMU page table and the cache structure
  while resolvers run. That is safe because resolvers never translate addresses (§4.3) and read
  cache structure only through the lock-free paths.

---

## 4. Batches, slots, publish

### 4.1 Batch assignment (SEQ)

* `Nx2DrawPipeline` SEQ state: `u64 batch_id = 0; u32 batch_draws = 0; bool batch_open = false;`.
* `EmitDraw` (vk_mt_pipeline.h:1260): if `!batch_open`, then `batch_open = true` and
  `header.batch = batch_id`. After `Commit`, `++batch_draws`. If `batch_draws == BatchSize()`,
  call `CloseBatch()`.
* `CloseBatch()`: if `batch_open`, push `StreamEntry::BatchEnd{batch_id, draws}`,
  `++batch_id`, `batch_draws = 0`, `batch_open = false`.
* **CloseBatch also before** every `SerialRaw` (the committer could otherwise wait on a resolver
  for a batch that the blocked SEQ will never close), every `SeqMark` (the front's waits must not
  hang on an open batch), `WaitCommitted` (I5 barrier, lockstep), every Command whose kind may
  make the committer wait for the GPU or the front (Fence, Query, FlushCommands, TickWork), and
  `EmitCompute`. Simplest rule: close at every non-draw entry except Deltas, CbBind and
  RootTable. With these closes the plan's adaptive sizing comes for free: an idle ring closes
  small batches.
* `header.batch` goes into `DrawHeader` (new `u64 batch`). The committer derives the resolver as
  `batch % K`.
* **Back-pressure**: before emitting draw `seq`, SEQ checks
  `seq - committed_seq < ResolvedRing::Slots - 2 * BatchSize()`. If that fails it calls
  `WaitCommitted()`, counted as `ring_wait`. `committed_seq` is a new committer atomic, stored
  after each draw commit (§10).

### 4.2 `ResolvedDraw` slot ring and arenas

```cpp
namespace Vulkan::MT {
template <class T> struct ArenaSpan { u32 offset; u32 count; };   // into Resolver::scratch-like ResultArena

enum class ResolveStatus : u8 { Ok, Slow, Skip /*resolver did not run: K changed / shadow-off */ };
enum SlowReason : u8 {  // MT_STATS index
    SlowNone, ViewMapMiss, ViewNotClean, ViewInvalidEntry, ViewLazyHandle, SamplerMapMiss,
    SamplerVariantMiss, BufferPageMiss, BufferBounds, BufferSparse, MultiRangeSsbo, OverlayMiss,
    UboMiss, IndexConversion, PipelineMiss, ArenaFull, BlockFull, TextureBuffers, Other, SlowReasonCount };

struct ViewRefFull { u32 view, view_gen, image, image_gen, image_version; u8 written, blacklist, null, pad; };  // 24 B
struct SamplerRef  { u32 sampler, gen; };
struct BufferRef   { u32 buffer, gen; u32 offset, size; u8 kind /*Ubo,Ssbo,Vertex,Index*/, written, stage, slot; DAddr device_addr; };
struct VertexBind  { u32 buffer, gen; u32 offset, size; u32 stride; u32 index; };

struct alignas(64) ResolvedDraw {          // fixed 256 B slot
    u64 seq;                               // written last-but-one; the batch publish releases it
    u64 batch;
    ResolveStatus status; SlowReason reason; u8 resolver; u8 flags; // flags: UsesRenderArea, HasWrittenImages, NeedsSyncBufferWrites
    u32 channel_gen;                       // ResolverMirrors bind generation (== committer's)
    GraphicsPipeline* pipeline;            // == header.pipeline (PIPE=seq) or resolved (PIPE=resolver)
    u64 buffer_epoch;                      // BufferCache::buffer_epoch at resolve start
    u64 pipeline_epoch;                    // PipelineCache::pipeline_epoch (PIPE=resolver)
    std::array<u32, Shader::Backend::SPIRV::NUM_TEXTURE_AND_IMAGE_SCALING_WORDS> rescaling;
    std::array<f32, 2> render_area;
    // descriptor payload
    u32 desc_arena_offset;                 // DescriptorArena entry index (ring position & mask)
    u32 desc_count;                        // == pipeline->num_descriptor_entries
    u64 desc_arena_end;                    // RingArena end_position for Release
    u32 desc_block;                        // descriptor-buffer block id (~0u = none, §9.2)
    u32 desc_block_offset;                 // byte offset inside the block's chunk
    u32 desc_chunk;                        // DescriptorBufferRing chunk index for UpdateDescriptorBufferChunk
    // variable parts (ResultArena of the resolver)
    ArenaSpan<ViewRefFull> views;          // ConfigureImpl `views` order (for CheckFeedbackLoop + validation)
    ArenaSpan<SamplerRef> samplers;
    ArenaSpan<BufferRef> buffers;          // UBO(cached)/SSBO in payload order + index + vertices
    ArenaSpan<VertexBind> vertices;        // enabled streams
    u32 index_buffer_ref;                  // index into buffers, ~0u = none
    u32 ubo_ring_bytes;                    // stats
};
static_assert(sizeof(ResolvedDraw) <= 256);

class ResolvedRing {                        // shared by all resolvers, indexed by draw seq
public:
    static constexpr u32 Slots = 4096;
    ResolvedDraw& Slot(u64 seq) noexcept { return slots[seq & (Slots - 1)]; }
private:
    std::unique_ptr<ResolvedDraw[]> slots{std::make_unique<ResolvedDraw[]>(Slots)};
};

/// Per resolver: views/samplers/buffers arrays of its in-flight batches. Single producer
/// (resolver), single releaser (committer after it committed the batch).
using ResultArena = VideoCommon::MT::RingArena<u64>;   // 1 Mi words = 8 MiB per resolver
}
```

* The resolver writes slot `seq & 4095` for each draw of its batch. Back-pressure (§4.1)
  guarantees the slot's previous occupant (`seq - 4096`) was committed.
* Variable data goes into the resolver's `ResultArena`, allocated once per batch from a
  conservative upper bound. The bound is the sum over draws of `num_image_elements + num_textures
  + 2*(18*5 + ssbo) + 33` refs. If the allocation fails, the remaining draws get
  `status = Slow, reason = ArenaFull`. **Never block while holding the EpochGuard** (R16).
* **Publish**: after the last draw of batch `b`, the resolver does a
  `release` store of `published = b` and `published.notify_all()` (C++20 atomic wait). This is
  one atomic per batch, as the plan specifies.
* **Committer wait** in `CommitDraw` for header `{seq, batch}` with `K > 0`:
  `Resolver& rv = *resolvers[batch % K]; if (rv.PublishedBatch() < batch) WaitBatch(rv, batch);`.
  Spin about 2k pauses, counting `commit_wait_ns`, then `published.wait(seen)`. Inside the wait
  loop, run `PressureCheck()` (§9.4) so a resolver blocked on arena or block space can make
  progress. Then read `resolved.Slot(seq)`; `ASSERT(slot.seq == seq)`.
* **Release**: after committing the last draw of batch `b` (the `BatchEnd` entry on the
  committer), call `rv.result_arena.Release(batch_end_pos)`. The `DescriptorArena` release is
  worker-side (§9.1).

### 4.3 Packet v2 additions (SEQ, `CaptureDraw` pass 2)

Phase 4 translates on SEQ but does not ship the results (only the uploads). Add to `DrawHeader`
`u32 num_ssbo, num_geometry; u64 batch; u64 layout_gen; u32 tic_valid_words;` and these arrays
after `uploads` and before the blob. Words count by 8 bytes, as today.

```cpp
struct SsboEntry {      // 24 B, CaptureDraw's local SsboCapture made POD and shipped
    u8 stage; u8 index /*ssbo_index within stage*/; u8 is_written; u8 skipped /*gpu_addr==0 or size==0*/;
    u32 size; u64 device_addr; u64 gpu_addr;
};
struct GeometryEntry {  // 16 B
    u8 kind /*0 index, 1 vertex*/; u8 stream; u16 pad; u32 size; u64 device_addr;
};
// + u64 tic_valid[tic_valid_words]: bit i = IsValidEntry(gpu_memory, TIC i of this draw, in views order)
```

* SSBO: SEQ already calls `MtStorageBinding`. Ship `binding.device_addr/size/gpu_addr`. Ship
  `skipped=1` where `ConfigureImpl` would bind `NULL_BINDING` (`gpu_addr == 0`), so the
  `ssbo_index` order matches `BindGraphicsStorageBuffer`.
* Geometry: index range (`device_addr`, `size` as in pass 2) and every enabled vertex stream
  (`MaxContinuousRange`-clamped size). Disabled streams are omitted. An enabled stream that
  fails to translate gets `device_addr = 0`; the resolver binds the null buffer, as
  `UpdateVertexBuffer` does.
* `tic_valid`: `IsValidEntry` (texture_cache/util.cpp:837) is a GMMU lookup, so it is computed on
  SEQ. Memoize it on SEQ per `(TIC 32 bytes, gpu_memory->LayoutGeneration())`: a 256-entry
  direct-mapped memo by `hash(raw)`, which keeps the cost to about one hash per view. This makes
  the resolver's map lookup equivalent to `VisitImageView`'s `is_new` branch (§7.1).
* `layout_gen = gpu_memory->LayoutGeneration()` at capture, for verify and stats.

The overlay, UboEntries and uploads stay unchanged, because the fallback path needs them.

---

## 5. `ResolveDraw<ExclusiveAccess>` → `ResolveDraw<ResolverAccess>`

`ExclusiveAccess` is the phase-4 `CommitDraw` body (overlay + `ConfigureDrawPipeline`).
`ResolverAccess` is the new `Resolver::ResolveOne`. The table walks `ConfigureImpl`
(vk_graphics_pipeline.cpp:387-880) and `ConfigureDrawPipeline` (vk_rasterizer.cpp:523) in order.

| Step (old code) | ResolverAccess (resolver thread) | Committer (after validation) |
|---|---|---|
| `SetEngine(maxwell3d, gpu_memory)` (rasterizer:526) | — (never touches the shared pipeline object) | as today, **but store `maxwell3d.Real()`**, see R1 |
| `SetVertexRepackLayout` + dirty flags (rasterizer:527-538) | — | as today (runtime state, mirror flags) |
| `tex_memo` mode/use_memo (405-416) | — (no memo: the resolver has its own L1, §7.1) | `tex_memo` untouched by resolved draws; ExclusiveAccess draws keep using it |
| `texture_cache.SynchronizeDescriptors(false)` (420) | — (limits read from the resolver mirror) | **still called per draw** (keeps `texture_bindings_serial` exact, so `CheckFeedbackLoop` caching is unchanged) |
| `buffer_cache.SetUniformBuffersState` (423) | — (masks from `ResolveInfo`) | called (channel bookkeeping for later ExclusiveAccess draws) |
| `config_stage` part 1: `UnbindGraphicsStorageBuffers`, `BindGraphicsStorageBuffer` (440-450) | from `SsboEntry[]`: `TryResolveRange` (§7.2), descriptor storage entry, `BufferRef{written}`; driver-limit check replicated with a per-draw local counter (`total_graphics_storage_buffers` semantics, buffer_cache.h:583-611); multi-range (`!IsContinuousRange` was already forced Serial on SEQ for read-only) → `Slow/MultiRangeSsbo` if written and sparse-preferring | `UnbindGraphicsStorageBuffers` + `channel_state->storage_buffers[stage][i] = Binding{...}` written from the refs (so ExclusiveAccess draws and `MarkWrittenBuffer` see consistent state) |
| `read_handle` / `EvaluateTextureHandle` (456-495) | same code on the resolver mirror with `t_capture` = the packet overlay; `overlay_misses != 0` → `Slow/OverlayMiss` | — |
| `GetSamplerId` (507-512) / memo sampler handles | `TryResolveSamplerCaptured` (§7.1) | — |
| BackHelper fork (536-592) | — (BackHelper disabled under MT, phase 4) | — |
| `TryReuseGraphicsBindings` memo (633-664) | — | — |
| `FillImageViews` (665-669) incl. blacklist `ScaleDown` | `TryResolveViewCaptured` per view; `blacklist && Rescaled` → `Slow/ViewNotClean` (ScaleDown is a structure change) | intents: LRU touch (dedup by `last_touch_frame`), `scale_rating = 0` for blacklisted views |
| texture-buffer binding (671-729) | unreachable: SEQ forces Serial (`CaptureDraw` reason 2); ASSERT `!has_texture_buffers` in `ResolveInfo`, else `Slow/TextureBuffers` | — |
| `UnbindGraphicsTextureBuffers` (731-747) | — | called as today |
| xfb outside-render-pass (749-751) | — (xfb forced Serial) | — |
| `UpdateGraphicsBuffers` + `BindHostGeometryBuffers` (753-757) | `UpdateIndexBuffer`/`UpdateVertexBuffer` equivalents: `TryResolveRange` per `GeometryEntry` → `VertexBind[]`, index `BufferRef`; `UpdateUniformBuffers`/`UpdateStorageBuffers` equivalents fold into the UBO/SSBO resolution | `ApplyResolvedGeometry`: writes `channel_state->index_buffer` / `vertex_buffers[i]` = `Binding{device_addr, size, buffer_id}` **without FindBuffer**, then runs `BindHostGeometryBuffers(is_indexed)` unchanged (dirty-flag-gated binds, uint8/quad conversion, repack) with `SynchronizeBuffer` bypassed (`t_capture`-like flag `mt_resolved_commit`) |
| `guest_descriptor_queue.Acquire` (760) | `DescriptorArena::Allocate(num_descriptor_entries)` (§9.1) | — |
| `BindHostStageBuffers(stage)` (782) | per stage: UBO entries in `ForEachEnabledBit` order: bank-17 root-table hack from the **resolver mirror's** `root_tables`; `UboEntry.staged` → `ResolverUniformRing` (§9.3); cached → `TryResolveRange`, `AddBuffer(buffer.Handle(), buffer.DeviceAddress(), offset, size)`; then SSBO entries; then (none) texture buffers | intents: `TouchBuffer`, `Buffer::MarkUsage(offset,size)` for cached UBOs/SSBOs; `fast_bound_uniform_buffers` bookkeeping (Vulkan: no persistent bindings → only the bitmask, kept for parity) |
| `PushImageDescriptors` (794) | `PushImageDescriptorsResolved` (§8) | intents: `MarkModification(image)` for written images |
| render area (813-820) | from the resolver mirror (`surface_clip`, `rt[0]`) | — |
| `any_buffer_uploaded` → `PostCopyBarrier` (847-850) | — | as today (uploads are applied by the committer) |
| `UpdateRenderTargets(false)`, `CheckFeedbackLoop(views)` (871-873) | — | as today; `views` rebuilt from `ResolvedDraw.views` into a `small_vector<ImageViewInOut,160>` |
| `IsBuilt() && !pipeline` (875) | — | as today |
| `ConfigureDraw` (882-1001) | descriptor-buffer write when `EDEN_NX2_MT_RDESC` (§9.2) | `ConfigureDrawResolved` (§10.3) |
| `UpdateDynamicStates`, `NotifySegment`, `HandleTransformFeedback`, `CounterEnable` (rasterizer:545-552) | — | as today |
| `RecordDraw` | — | as today (5b: replay encoder) |

---

## 6. Per-resolver Maxwell3D mirrors, overrides and forced dirty flags

* **Delta application**: each resolver applies every `Deltas`, `CbBind` and `RootTable` entry
  with exactly the committer's code (`RunEntry` cases, vk_mt_pipeline.h:801-845). Factor the
  three cases into `ApplyMirrorEntry(ChannelMirrors&, const u64*)` shared by both. `ComputeDeltas`
  apply to the resolver's compute mirror. It is unused in phase 5 but kept in sync for phase 8.
* Draw state: the resolver writes `header.draw_state` into its mirror's
  `draw_manager.draw_state`, as `CommitDraw` does (vk_mt_pipeline.h:1376-1393). It uses the
  topology (for `FixedPipelineState::Refresh` when `PIPE=resolver`) and nothing else; the
  inline-index vector is not copied.
* `native_turing` from the header → `mirror.GetNativeMacroEngine().native_turing`.
* **Overrides**: `ScopedEngineOverride`-equivalent pairs `{real3d, resolver_mirror}` in the
  resolver's `g_engine_overrides<Maxwell3D>` (thread_local). Any cache helper that reads
  `maxwell3d->` on the resolver thread then sees the resolver mirror and never a shared
  engine. The resolver code itself always passes its mirror explicitly; the override is a safety
  net that `EDEN_NX2_MT_RCHECK` asserts: a `ThreadEnginePtr::Get()` on a resolver with no matching
  pair means some shared state leaked.
* **R1 (bug to fix first)**: `ConfigureDrawPipeline` calls `pipeline->SetEngine(maxwell3d,
  gpu_memory)`. On the committer, `ThreadEnginePtr::operator T*()` returns the **committer
  mirror**, so `GraphicsPipeline::maxwell3d` stores the mirror pointer. That only works on the
  committer by accident (no override matches, so `Get()` returns the stored mirror). A resolver
  dereferencing `pipeline->maxwell3d` would read the committer's mirror (a data race). Fix:
  `pipeline->SetEngine(maxwell3d.Real(), gpu_memory)` (and the same in `ComputePipeline`).
  Resolvers must still never use `pipeline->maxwell3d`.
* **Forced dirty set at batch start** (resolver mirror only). The resolver consumes mirror flags
  only through `FixedPipelineState::Refresh` (`PIPE=resolver`: VertexInput, ViewportSwizzles,
  Blending, fixed_pipeline_state.cpp:193-253). Its `pipe_key` was last refreshed at a draw of an
  earlier batch it owned, while the flags changed in between. At the start of each owned batch,
  set `VertexInput, VertexAttribute0..31, VertexBinding0..31, ViewportSwizzles, Blending,
  BlendState0..7, Shaders, VertexBuffers, VertexBuffer0..31, IndexBuffer` to true.
  Buffer/vertex/index flags are not used by resolvers (they resolve every binding every draw; the
  committer elides). They are forced anyway, so that a future helper reading them gets the
  conservative answer. Cost: one `bitset |= mask` per batch.
* Mirror-only flags never feed elision on the committer. The committer keeps its own mirror and
  exact flags (phase 4).

---

## 7. Lock-free read paths

General protocol for every resolver read of a cache object:

```
EpochGuard g{epochs, reader};        // per batch, §11
id   = map/page_table load (acquire)
gen  = slots.Generation(id) (acquire); if !(gen & 1) → miss
obj  = slots.UncheckedAt(id)         // memory valid until guard ends (EBR)
read only IMMUTABLE fields or atomics of obj
if slots.Generation(id) != gen → retry once, then miss     // seqlock-style re-check
record {id, gen, version...} for commit validation (I4)
```

### 7.1 Texture cache (`texture_cache/texture_cache.h`, new section "MT resolver reads", decl in `texture_cache_base.h`)

```cpp
struct ResolvedView {
    ImageViewId view; u32 view_gen;
    ImageId image; u32 image_gen; u32 image_version;
    const ImageView* object;          // valid while the EpochGuard is held
    bool is_buffer;
};
enum class ViewResolve : u8 { Ok, Null, Miss, NotClean, Invalid };

/// Resolver thread, EpochGuard held, no lock. `channel` captured at ChannelSync.
ViewResolve TryResolveViewCaptured(const TextureCacheChannelInfo& channel, const TICEntry& entry,
                                   bool valid_entry, bool blacklist, ResolvedView& out) const noexcept;
enum class SamplerResolve : u8 { Ok, Null, Miss };
SamplerResolve TryResolveSamplerCaptured(const TextureCacheChannelInfo& channel, const TSCEntry& entry,
                                         SamplerId& out, u32& gen) const noexcept;
/// Resolver: creates the sampler under sampler_create_mutex (§7.1.3).
SamplerId CreateSamplerFromResolver(TextureCacheChannelInfo& channel, const TSCEntry& entry);
std::mutex sampler_create_mutex;
```

#### 7.1.1 View path (replaces `VisitImageView`, texture_cache.h:998, plus `PrepareImageView` :3859)

1. `index = TextureHeaderIndex(handle, mirror.regs.tex_header.limit, gpu_memory->UsesNativeTiling())`.
   This is the same expression `VisitImageView` uses with `table.current_limit`. `SynchronizeDescriptors`
   sets `current_limit` from the same register (the committer's mirror equals the resolver's at
   this packet). If `index > limit` → `Null`.
2. TIC bytes: `t_capture->Read(tex_header.Address() + index*32, &entry, 32)`. If it fails →
   `Slow/OverlayMiss`.
3. `valid_entry = tic_valid bit` (§4.3). If not set → `Null` (old: `IsValidEntry` false →
   `NULL_IMAGE_VIEW_ID`, texture_cache.h:1022).
4. Resolver L1: a direct-mapped 512-entry cache `hash(entry) → {entry, view, view_gen, image,
   image_gen}`. On a hit, go to step 6 with the cached ids, but **re-validate the generations**.
5. `channel.image_views_mt.Find(entry)`. `nullopt` → `Miss` (the committer will `CreateImageView`,
   a structure change). `NULL_IMAGE_VIEW_ID` → `Null`. The map holds only ids created from valid
   entries; `CreateImageView` may return NULL, and the twin stores it, which matches old semantics.
6. `view_gen = slot_image_views.Generation(view)`. Even → `Miss`. Then
   `ImageView& v = slot_image_views.UncheckedAt(view)`. If `v.IsBuffer()` → `NotClean` (texture
   buffers never reach resolvers). Then `image = v.image_id`, `image_gen =
   slot_images.Generation(image)` (even → `Miss`), and `Image& img = slot_images.UncheckedAt(image)`.
7. `version = img.version.load(acquire)`, then `flags = img.flags.Load(acquire)`. **NotClean** if any
   of `CpuModified | HasAliases | Dead | UnwrittenSparse | SparseTiles(dirty) | SparseLayers` is
   set, or `blacklist && Rescaled`. These conditions mirror `TryFillImageViewsClean`
   (texture_cache.h:578) and `TryReuseGraphicsBindings` (:618): only an image that
   `PrepareImage` (:3805) would leave untouched apart from the LRU is resolver-clean. Sparse
   images are excluded conservatively, because `PrepareImage`'s `is_modification` sparse work
   applies to written images.
8. Re-check `slot_image_views.Generation(view) == view_gen && slot_images.Generation(image) ==
   image_gen && img.version == version`. If any fails → retry once from step 5, else `Miss`.
9. Out: `{view, view_gen, image, image_gen, version, &v}`.

Commit validation (§10.1) re-checks `view_gen`, `image_gen` and `version`. Every flag that makes
an image unclean is set together with `BumpVersion`: `WriteMemory`, `MarkLayersWritten`,
`MarkRangeWritten`, `UnmapGPUMemory`, sparse binds, `ScaleUp/Down`, `NoteAliasesChanged`, the
`DeleteImage` Dead bit, and `PrepareImage`'s sparse replacement. That is phase 2's list. **Audit
before enabling (S5)**: grep every `flags |=` on images for `CpuModified`, `HasAliases`, `Dead`,
`Rescaled`, `UnwrittenSparse`, `Remapped`, `SparseTiles`, `SparseLayers` and add `BumpVersion`
where it is missing. Known candidates: `RefreshContents`, which clears CpuModified (clean
direction, harmless); `JoinImages` alias creation via `AddImageAlias`, already done;
`CheckAliasState`; `TrackImage`/`UntrackImage`, which are irrelevant.

#### 7.1.2 Sampler path (replaces `GetSamplerId` :769 and `FindSampler` :3132)

1. `tsc_limit = via_header ? tic_limit : tex_sampler.limit` from the mirror. If `index > tsc_limit` →
   `Null`.
2. TSC bytes from the overlay (`tex_sampler.Address() + index*32`). A miss → `Slow/OverlayMiss`.
3. All-zero entry → `NULL_SAMPLER_ID` (`FindSampler`'s first test).
4. L1: 256 entries `hash(entry) → {entry, id}`.
5. `channel.samplers_mt.Find(entry)` → `id`. `gen = slot_samplers.Generation(id)`; samplers are
   never erased, but the check is kept. On `nullopt`, apply §7.1.3.

#### 7.1.3 Sampler creation from resolvers (`sampler_create_mutex`)

Samplers are created rarely and never deleted, so resolvers create them instead of falling back.
This requires every writer of `slot_samplers`, `channel_state->samplers` and `samplers_mt` to
hold `sampler_create_mutex`:

* `FindSampler` (committer and Serial paths): wrap the `try_emplace` / `insert` / `InsertOrAssign`
  in `std::scoped_lock lk{sampler_create_mutex}`. It is uncontended in practice.
* `CreateSamplerFromResolver`: take the lock, then `samplers_mt.Find` again (double-checked),
  then `try_emplace` into `channel.samplers`, then `slot_samplers.insert(runtime, config)`, then
  `samplers_mt.InsertOrAssign`. `vkCreateSampler` is thread-safe with respect to the device.
  `CustomBorderColorBudget::TryAcquire` mutates shared budget state and is now serialized by the
  mutex. The constructor uses `runtime` (const device/settings reads only; verify it never touches
  the scheduler).
* `StableSlotVector::insert` and `SingleWriterMap::InsertOrAssign` assume a single writer. Serializing
  every writer by the mutex satisfies that (release publication is unchanged).
* The `Sampler` constructor must not use the per-thread engine pointers.

#### 7.1.4 Lazily created Vulkan handles

`PushImageDescriptors` (pipeline_helper.h:359) calls two **mutating** accessors:

* `ImageView::StorageView(type, format)` (vk_texture_cache.cpp:4112) creates `typeless_storage_view`
  or `storage_views->{signeds,unsigneds}[type]` on first use (`std::optional::emplace` + `vk::ImageView`
  assignment).
* `Sampler::HandleFor(view, is_depth)` (:4389) calls `Find` and then `Emplace` (variants
  `std::vector<Variant>`, `MAX_VARIANTS = 32`, `push_back` → reallocation).

Changes (S2):

* `ImageView`: add `std::array<std::atomic<VkImageView>, 2 * Shader::NUM_TEXTURE_TYPES + 1>
  published_storage{}` (index: typeless = 0, unsigned `1+type`, signed `1+NUM+type`). `StorageView`
  stores the created handle into it (release) after creating it. It runs only on the committer:
  the resolver never creates views. Add
  `[[nodiscard]] VkImageView TryStorageView(type, format) const noexcept` (acquire load; NULL if
  not created; `!image_handle` → NULL with result "Ok-null", the old return value). The resolver
  treats NULL from a not-yet-created view as `Slow/ViewLazyHandle`. The committer's ExclusiveAccess
  path then creates it, and later draws resolve.
* `Sampler`: replace `std::vector<Variant> variants` with `std::array<Variant, MAX_VARIANTS>
  variants; std::atomic<u32> num_variants{0};`. `Find` reads `num_variants` (acquire) and scans.
  `Emplace` runs under `sampler_create_mutex`: re-`Find`, construct at `variants[n]`, then
  `num_variants.store(n+1, release)`. `Handle()` (front variant) is unchanged. Resolvers call
  `MakeKey` (const, reads immutable view fields) and `Find`. On a miss, they take
  `sampler_create_mutex` and `Emplace`, so creating the variant on the resolver is allowed too.
  The fallback when `MAX_VARIANTS` is reached is unchanged.
* `ImageView::Handle(type)` (`*image_views[type]`) is set only in the constructor. Immutable,
  safe.

### 7.2 Buffer cache: `TryResolveRange` (`buffer_cache_mt.h`)

```cpp
struct ResolvedRange { BufferId id; u32 gen; u32 offset; VkBuffer handle; VkDeviceAddress base; };
enum class RangeResolve : u8 { Ok, Null, Miss };
/// Resolver: FindBuffer without creation, synchronization or LRU. Lock-free.
RangeResolve TryResolveRange(DAddr device_addr, u32 size, bool sparse_compatible,
                             ResolvedRange& out) const noexcept {
    if (device_addr == 0) return RangeResolve::Null;                     // FindBuffer → NULL_BUFFER_ID
    const u64 page = device_addr >> CACHING_PAGEBITS;
    if (page >= page_table.size()) return RangeResolve::Null;
    const BufferId id = std::atomic_ref<const BufferId>{page_table[page]}.load(std::memory_order_acquire);
    if (!id) return RangeResolve::Miss;
    const u32 gen = slot_buffers.Generation(id);
    if ((gen & 1) == 0) return RangeResolve::Miss;
    const Buffer& buffer = slot_buffers.UncheckedAt(id);
    if (!buffer.IsInBounds(device_addr, size)) return RangeResolve::Miss;   // CreateBuffer would join/grow
    if constexpr (requires { buffer.IsSparseCompatible(); })
        if (sparse_compatible && !buffer.IsSparseCompatible()) return RangeResolve::Miss;
    out = {id, gen, buffer.Offset(device_addr), buffer.Handle(), buffer.DeviceAddress()};
    if (slot_buffers.Generation(id) != gen) return RangeResolve::Miss;    // re-check
    return RangeResolve::Ok;
}
std::atomic<u64>& BufferEpoch() noexcept { return buffer_epoch; }
```

* `page_table` is already written with release stores (`ChangeRegister`, buffer_cache.h:2303).
* `buffer_epoch` is bumped in `DeleteBuffer` (:2646), and through it in `JoinOverlap` /
  `CreateBuffer` joins. Recording it at resolve start gives the commit fast path: if the epoch is
  unchanged, skip the per-buffer generation checks. Otherwise check `slot_buffers.IsCurrent(id,
  gen)` per `BufferRef`. **The per-buffer check is the real criterion.** The GC deletes buffers
  every frame (`RunGarbageCollector`), so a global-epoch-only scheme would cause re-resolve
  storms (R13). The inline-index buffer erase (`UpdateIndexBuffer`, buffer_cache.h:~1625) also
  bumps the generation (StableSlotVector::erase).
* `Buffer::IsInBounds`, `Offset`, `Handle`, `DeviceAddress`: `cpu_addr`/`size_bytes`/the VkBuffer
  are immutable for the buffer's lifetime (a resize creates a new buffer and deletes the old one).
  Confirm in `vk_buffer_cache.h` that `Buffer` has no in-place resize (S5 audit item).
* `FindBuffer`'s `SynchronizeBufferWrites` (accurate GPU fences, buffer_cache.h:2158): if
  `Settings::IsGPUFenceBehaviorAccurate()`, set `ResolvedDraw.flags |= NeedsSyncBufferWrites`.
  The committer then calls `SynchronizeBufferWrites(slot_buffers[id])` for each `BufferRef`.
* Null-buffer semantics: `RangeResolve::Null` → `NULL_BUFFER_ID` (slot 0, inserted at
  construction, buffer_cache.h:38). The descriptor uses the null buffer exactly as the old path
  binds it. Check `UpdateVertexBuffer`, `UpdateIndexBuffer` and `BindHostGraphicsUniformBuffer`
  for their `NULL_BUFFER_ID` handling (they skip FindBuffer for a zero address).
* Uniform cached path: the descriptor is `AddBuffer(buffer.Handle(), buffer.DeviceAddress(),
  offset, size)`, which is what `runtime.BindUniformBuffer` → `BindBuffer` does
  (vk_buffer_cache.h:246, 275-281), including the `handle == null` → whole-size branch at :278.
  Copy that function's logic verbatim into a static
  `BufferCacheRuntime::MakeBufferDescriptor(const Buffer&, u32 offset, u32 size, bool use_descriptor_buffer)`
  shared by both, so they can't drift.

### 7.3 Pipeline cache (`vk_pipeline_cache.h/.cpp`)

**Default `EDEN_NX2_MT_PIPE=seq`**: the packet carries `GraphicsPipeline*` from SEQ's
`CurrentGraphicsPipeline`, as in phase 4. Resolvers read only the pipeline's **`ResolveInfo`**:

```cpp
struct GraphicsPipeline::ResolveInfo {   // built at the end of the ctor, immutable afterwards
    const std::array<Shader::Info, NUM_STAGES>* stage_infos;
    std::array<u32, 5> enabled_uniform_buffer_masks;
    const VideoCommon::UniformBufferSizes* uniform_buffer_sizes;
    u32 num_descriptor_entries, num_image_elements, num_textures;
    bool uses_descriptor_buffer, uses_push_descriptor, has_texture_buffers, has_image_buffers, has_images;
    std::array<bool, 5> enabled_stages;       // Spec::enabled_stages of configure_func's Spec
    const DescriptorBufferLayout* descriptor_buffer_layout;   // §9.2
    bool is_mesh;
};
[[nodiscard]] const ResolveInfo& GetResolveInfo() const noexcept { return resolve_info; }
```

* `enabled_stages` / `has_*` come from the `FindSpec` result. Make `ConfigureFunc`
  (vk_graphics_pipeline.cpp:261) also return the Spec's flags (a `SpecFlags` struct beside the
  function pointer). The resolver must visit exactly the stages `ConfigureImpl<Spec>` visits.
  Visiting a disabled stage would add descriptor entries.
* Members that the resolver **must not touch**: `transitions`/`transition_keys` (SEQ, mutated by
  `AddTransition` in `CurrentGraphicsPipelineSlowPath`), `last_use_frame` (SEQ writes `MarkUsed`;
  the committer reads `LastUse` in `EvictUnusedPipelines` → make it `std::atomic<u64>` relaxed,
  R21), `tex_memo`, `last_descriptor_*`, `memo_last_handles`, `descriptor_allocator`,
  `build_task`/`build_*`, `maxwell3d`/`gpu_memory`. `is_built` may be read (atomic).

**Optional `EDEN_NX2_MT_PIPE=resolver`** (step S10; default off): moves `FixedPipelineState::Refresh`
and the key lookup off SEQ.

* `PipelineCache` gains `MT::SingleWriterMap<GraphicsPipelineCacheKey, GraphicsPipeline*,
  KeyHash, KeyEqual> graphics_cache_mt` (the key is trivially copyable with unique object
  representations; `Value = GraphicsPipeline*` is trivially copyable). Maintenance: insert in
  `CurrentGraphicsPipelineSlowPath` after creation, on SEQ, the single writer of
  `graphics_cache`. Erase in `EvictUnusedPipelines` (committer!). **Two writers**: move eviction
  to SEQ, as phase-4 spec §1.3 already prescribes (`pipeline_tick_pending` → SEQ runs
  `EvictUnusedPipelines` at a packet boundary), and retire evicted pipelines through EBR
  **and** `evicted_pipelines`' tick wait: `epochs.Retire([p = release()] { push to evicted list })`.
* `std::atomic<u64> pipeline_epoch` bumped per eviction run, recorded in `ResolvedDraw`, and
  validated at commit.
* Resolver: `pipe_key.unique_hashes = header.hashes` (SEQ still runs `RefreshStages`; it needs
  the stage infos for capture), then `pipe_key.state.Refresh(mirror, dynamic_features)`, with the
  forced flags of §6 at batch start, then L1 (`last_key → pipeline`), then
  `graphics_cache_mt.Find(pipe_key)`.
* **The miss problem.** On a lookup miss, `CreateGraphicsPipeline` builds `GraphicsEnvironment`s
  that read guest memory at the draw point: code, SPH, cbuf values via `ReadCbufValue`, TIC types,
  root-table bytes (phase-4 spec G19). When the resolver sees the miss, SEQ has moved on, so
  creating then is **not exact (I1)**. Therefore:
  * SEQ keeps a `known_hashes` set (`std::unordered_set` of the 6-hash tuple) of hash tuples that
    already have at least one pipeline. A tuple not in the set → SEQ does the full
    `CurrentGraphicsPipeline()`, including the Serial creation, as today.
  * A tuple in the set → SEQ skips `graphics_key.state.Refresh` and ships the hashes. A resolver
    miss → `Slow/PipelineMiss`. The committer then creates "late" (`CreateGraphicsPipeline` with
    current memory), counted `late_pipeline_create`. Under `MT_VERIFY` (lockstep, so memory is
    still exact) the late environment's `cbuf_values` and code hashes are compared with a
    lockstep re-run.
  * Allowed only when every stage's `Shader::Info` reports no cbuf-value or texture-type
    dependency. Concretely, `GraphicsEnvironment` recorded no `cbuf_values` / `texture_types`
    when the program was first translated (store a `pure_translation` bit per `ShaderInfo` on
    SEQ in `RefreshStages`/`MakeShaderInfo`). Otherwise that hash tuple stays in SEQ mode.
  * `BuiltPipeline` (vk_pipeline_cache.cpp:1288; async shaders: reads `regs.zeta_enable`,
    `draw_element_count`) runs on the resolver against its mirror. A null result →
    `status = Skip` and the committer skips the draw. Phase 4 skips on SEQ (`ReportDrawFailure`).
  * SEQ still needs `IsMeshPipeline` (`MeshDrawAllowed`) and the capture plan. Both derive from
    `shader_infos[0]` and the stage infos by hash, not from the pipeline. `CaptureDraw` must take
    the stage infos and masks from the `ShaderInfo`s of the hashes (`pipeline_cache` per-hash
    infos), not from `GraphicsPipeline` (S10 refactor).
* Shadow mode compares the resolver's `pipe_key` and pointer with SEQ's for every draw while
  `PIPE=seq` (`pipe_mismatch` counter). Enable `PIPE=resolver` only after zero mismatches over a
  full demo run.

---

## 8. `PushImageDescriptorsResolved` (pipeline_helper.h, next to `PushImageDescriptors`)

```cpp
/// Resolver: PushImageDescriptors from resolved views/samplers into `out`, without touching
/// the texture cache's mutable state. Returns false (slow) when a handle must be created first.
struct ResolvedImageInputs {
    std::span<const VideoCommon::MT::ResolvedView> views;   // ConfigureImpl `views` order
    std::span<Sampler* const> samplers;                     // texture order
};
template <class Sink>  // Sink: AddSampledImage(VkImageView, VkSampler), AddImage(VkImageView)
[[nodiscard]] inline bool PushImageDescriptorsResolved(
        const TextureCache& texture_cache, Sink& out, const Shader::Info& info,
        RescalingPushConstant& rescaling, const VideoCommon::MT::ResolvedView*& views,
        Sampler* const*& samplers, std::mutex& sampler_create_mutex,
        boost::container::small_vector<u32, 16>& written_view_indices, u32& view_cursor) {
    views += Shader::NumDescriptors(info.texture_buffer_descriptors) +
             Shader::NumDescriptors(info.image_buffer_descriptors);   // ASSERT 0 (forced Serial)
    for (const auto& desc : info.texture_descriptors) {
        bool is_rescaled{};
        for (u32 i = 0; i < desc.count; ++i, ++views, ++samplers, ++view_cursor) {
            const ImageView& view = views->object ? *views->object : texture_cache.NullImageViewObject();
            VkImageView vk_view = view.Handle(desc.type);                         // immutable
            if (vk_view == VK_NULL_HANDLE) vk_view = texture_cache.NullImageViewObject().Handle(desc.type);
            VkSampler vk_sampler = (*samplers)->TryHandleFor(view, desc.is_depth); // Find only
            if (vk_sampler == VK_NULL_HANDLE) {
                std::scoped_lock lk{sampler_create_mutex};
                vk_sampler = (*samplers)->HandleFor(view, desc.is_depth);          // Emplace under mutex
            }
            out.AddSampledImage(vk_view, vk_sampler);
            is_rescaled |= views->object && IsRescaledFlags(*views);              // atomic image flag, see below
        }
        rescaling.PushTexture(is_rescaled);
    }
    for (const auto& desc : info.image_descriptors) {
        bool is_rescaled{};
        for (u32 i = 0; i < desc.count; ++i, ++views, ++view_cursor) {
            const ImageView& view = views->object ? *views->object : texture_cache.NullImageViewObject();
            const VkImageView vk_view = view.TryStorageView(desc.type, desc.format);
            if (vk_view == VK_NULL_HANDLE && view.ImageHandle() != VK_NULL_HANDLE) return false; // ViewLazyHandle
            if (desc.is_written) written_view_indices.push_back(view_cursor);                    // → intent
            out.AddImage(vk_view);
            is_rescaled |= views->object && IsRescaledFlags(*views);
        }
        rescaling.PushImage(is_rescaled);
    }
    return true;
}
```

Policy:

* **Same order and count** as `PushImageDescriptors`, so the payload is byte-identical apart from
  streamed UBO handles (§9.3). `MT_SHADOW` checks it.
* `texture_cache.IsRescaling(image_view)` (texture_cache.h:1740) reads the image's `Rescaled`
  flag through `slot_images`. The resolver reads `flags.Load()` of the image it validated
  (`views->image`). The image version covers `Rescaled` changes (`ScaleUp/Down` bump). Expose
  `TextureCache::IsRescalingResolved(const ResolvedView&)` doing exactly what `IsRescaling(view)`
  does: `IsBuffer` → false, else `True(flags & Rescaled)`.
* NULL views: `NullImageViewObject()` = `slot_image_views.UncheckedAt(NULL_IMAGE_VIEW_ID)`
  (inserted at construction, never erased).
* `MarkModification(image_id)` for written storage images becomes a commit intent
  (`ResolvedDraw.views[i].written = 1`). The committer calls `texture_cache.MarkModification(id)` in
  the same view order. `modification_tick` ordering matters for `SynchronizeAliases`, and clean
  images have no aliases, but keep the order anyway.
* The `Sink` for the resolver is `DescriptorArenaSink` (§9.1). For the shared old code, add a
  `GuestDescriptorQueue` adapter so that one template could serve both later. Phase 5 does not
  change `PushImageDescriptors`.

---

## 9. Descriptor and uniform memory owned by resolvers

### 9.1 `DescriptorArena` (`vk_mt_resolve.h`)

```cpp
class DescriptorArena {
public:
    static constexpr size_t Entries = size_t{1} << 18;   // 262144 × 24 B = 6 MiB per resolver
    /// Resolver. Blocks (stop-aware, guard released) while the worker has not executed enough.
    [[nodiscard]] std::span<DescriptorUpdateEntry> Allocate(u32 count, u64& end_position);
    /// VulkanWorker (inside a recorded command), in recording order.
    void ReleaseFromWorker(u64 end_position) { ring.Release(end_position); executed.store(end_position, std::memory_order_release); }
    [[nodiscard]] bool Starved() const noexcept { return starved.load(std::memory_order_acquire); }
private:
    VideoCommon::MT::RingArena<DescriptorUpdateEntry> ring{Entries};
    std::atomic<u64> executed{0};
    std::atomic<bool> starved{false};
};

struct DescriptorArenaSink {      // mirrors UpdateDescriptorQueue::Add* (vk_update_descriptor.h)
    DescriptorUpdateEntry* cursor; bool use_descriptor_buffer;
    void AddSampledImage(VkImageView v, VkSampler s) { *cursor++ = VkDescriptorImageInfo{s, v, VK_IMAGE_LAYOUT_GENERAL}; }
    void AddImage(VkImageView v) { *cursor++ = VkDescriptorImageInfo{VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL}; }
    void AddBuffer(VkBuffer b, VkDeviceAddress base, VkDeviceSize off, VkDeviceSize size);  // same branch as UpdateDescriptorQueue::AddBuffer(4 args)
};
```

* `use_descriptor_buffer = device_supports && pipeline.uses_descriptor_buffer`, the same as
  `UpdateDescriptorQueue::Acquire`'s `use_descriptor_buffer` (vk_update_descriptor.cpp:41). The
  device flag is read once from `guest_descriptor_queue.SupportsDescriptorBuffer()` (new const
  accessor).
* **Release by the worker**: the payload pointer is consumed by the closure that
  `ConfigureDrawResolved` records (push descriptors copy it, `UpdateDescriptorSet` reads it, and the
  descriptor-buffer path in RDESC-off mode `WriteDescriptorBuffer`s it on the committer). After the
  **last draw of a batch**, the committer records
  `scheduler.Record([arena = &rv.arena, end = batch_desc_end](vk::CommandBuffer) { arena->ReleaseFromWorker(end); });`.
  This is in order with the draw closures, so all earlier payloads have been read. Rejected draws'
  spans (§10.2) are inside the batch range and are freed by the same release.
* `Allocate` full → the resolver sets `starved = true`, **leaves its EpochGuard** (records how
  many draws are done, then re-enters before resuming) and spins/yields with `stop` checks. See
  §9.4 for how the committer unblocks it.

### 9.2 `DescriptorBufferRing::AllocateBlock` (vk_descriptor_buffer.h/.cpp)

Used only when the pipeline `uses_descriptor_buffer` and `EDEN_NX2_MT_RDESC`.

```cpp
struct Block { u32 id; u32 chunk; VkDeviceSize offset; u8* host; VkDeviceSize size; };
static constexpr VkDeviceSize BlockSize = 64 * 1024;
/// Any resolver. A block whose last user's tick is complete, or nullopt (caller: Starved).
[[nodiscard]] std::optional<Block> AllocateBlock(const Scheduler& scheduler);   // IsFree only: atomic read
/// Committer: the block's contents are referenced by work recorded up to `tick`.
void StampBlock(u32 id, u64 tick) noexcept;          // blocks[id].tick.store(tick, release); in_use=false
/// Resolver: the block is full / batch done: hand it to the committer to stamp.
```

* Storage: at construction, **2 extra chunks per resolver** (`MaxResolvers = 8`; allocate
  `chunks_per_frame * FRAMES_IN_FLIGHT + 2*K` chunks, host-visible `MemoryUsage::Upload`, the same
  create path). These are carved into `chunk_capacity / BlockSize` blocks each. The frame ring
  (`Allocate`, `TickFrame`, `generation`) is untouched; the committer still uses it for
  ExclusiveAccess draws.
* Free list under `block_mutex` (std::mutex; one `AllocateBlock` per ~16-64 draws). A block is
  reusable when `stamped && scheduler.IsFree(tick)`. `IsFree` is a master-semaphore atomic read,
  safe from any thread (`Scheduler::IsFree`, vk_scheduler.h:180).
* The resolver keeps a `DescriptorBlockCursor{Block, used}`. Per draw it checks
  `layout.size <= block.size - used`, else gets a new block (the old one goes on
  `pending_stamp[r]`). It writes `WriteDescriptorBuffer(device, layout, entries, block.host + used)`.
  That function reads only the `DescriptorBufferLayout` (immutable, owned by the pipeline;
  ResolveInfo) and calls `vkGetDescriptorEXT` (device-level, thread-safe). It records
  `desc_block/desc_block_offset/desc_chunk` in the slot.
* Committer: in `ConfigureDrawResolved`, `UpdateDescriptorBufferChunk(slot.desc_chunk)` and
  `SetDescriptorBufferOffsetsEXT(offset = block.offset + desc_block_offset)`. The binding info for
  the extra chunks comes from `BindingInfo(chunk)` (chunk index space extended). At each batch
  end, `StampBlock(id, scheduler.CurrentTick())` for every block the batch used, including the
  still-open cursor block; stamping again with a later tick is fine. `CurrentTick` here is the
  command buffer the draws were recorded into. A later `Flush` only increases the tick.
* **Elision** of `ConfigureDraw`'s `reuse_allocation` memcmp (last_descriptor_payload,
  vk_graphics_pipeline.cpp:886-905) does not apply on this path, so each draw writes new bytes.
  That is the price of parallel writes. In RDESC-off mode the committer keeps today's per-pipeline
  reuse check against the arena payload.
* Exhaustion: `AllocateBlock` → nullopt → resolver `Starved` (§9.4). There is never a
  `scheduler.Finish()` from a resolver; it can't touch the scheduler (I2).

### 9.3 `ResolverUniformRing` (streamed small UBOs)

Phase 4 streams staged UBO bytes on the committer: `BindHostGraphicsUniformBuffer` captured
branch → `BindSmallUniformReusing(runtime, CapturedBytes, ...)` or `BindMappedUniformBuffer`. That
puts the 8.5 % memcmp/memcpy on the committer. The stream buffer and `UniformStreamReuse` (a
global static, uniform_stream_reuse.h `UniformStreamReuseState`) are committer-owned.

* Per resolver: one `vk::Buffer` of 16 MiB, `MemoryUsage::Upload` (host memory; VRAM is the
  constraint), usage `UNIFORM_BUFFER | SHADER_DEVICE_ADDRESS`, persistently mapped. Created by the
  committer at `Start` (MemoryAllocator is not thread-safe).
* Allocation: bump within the batch, aligned to `GetUniformBufferAlignment()`. Each batch's region
  is `[begin, end)`. Batch regions are freed in order when `IsFree(stamp[b + ReuseWindow])`,
  `ReuseWindow = 8` batches. The committer writes `stamp[b] = CurrentTick()` at batch end into a
  per-resolver `std::array<std::atomic<u64>, 256>` ring.
* Reuse memo per resolver (an instance of `UniformStreamReuse<Ref>`, **not** the static): the
  same `(stage, slot, size, bytes)` as an allocation made in the last `ReuseWindow` batches →
  reuse the offset. Because release lags `ReuseWindow` batches, a reused region is never freed
  before the command buffer of the reusing draw completes. Mismatch → new allocation + memcpy
  from the packet blob (`UboEntry.offset`). The bank-17 root-table stream (≤256 B from
  `mirror.root_tables.tables[0]` zero-padded to `size`) uses the same ring.
* Descriptor entry: `sink.AddBuffer(ring_buffer, ring_device_address, offset, size)`, which equals
  `RebindStreamUniform`/`BindMappedUniformBuffer`'s `guest_descriptor_queue.AddBuffer(buffer,
  device_address, offset, size)` (vk_buffer_cache.h:202-203).
* Under `MT_SHADOW` the payload compare for streamed UBO entries compares **the bytes** at both
  addresses, not the handles.
* `EDEN_NX2_MT_RUBO=0`: the resolver emits a placeholder entry plus a patch record `{entry_index,
  stage, slot, blob offset, size}`. The committer streams it through the runtime and patches the
  arena entry before `ConfigureDrawResolved`. The arena is not yet released, and the committer
  reading it after the acquire publish is safe.

### 9.4 Starvation and the cycle committer → resolver → worker/GPU

There is a cycle: the committer waits for batch `b` (resolver `r`); resolver `r` waits for arena
or block space; the space is released by worker closures sitting in the committer's
**undispatched** chunk, or by a GPU tick whose command buffer the committer has not submitted.
That deadlocks (R4).

`Nx2DrawPipeline::PressureCheck()` runs inside every committer wait on resolvers (§4.2) and at
every committer idle:

```cpp
for (auto& rv : resolvers) if (rv->arena.Starved() || rv->blocks_starved || rv->ubo_starved) {
    scheduler.DispatchWork();                 // hand the chunk with release closures to the worker
    if (rv->blocks_starved || rv->ubo_starved) scheduler.Flush();   // submit: ticks can complete
    counters.pressure_flushes++;
}
```

The resolver clears its `starved` flag once it allocated. Ring sizes are chosen so this is rare
(arena: 262k entries ≈ 700+ draws of 350 entries; blocks: 2 chunks per resolver).

---

## 10. Commit (committer)

`Nx2DrawPipeline::CommitDraw` (vk_mt_pipeline.h:1359) becomes:

```cpp
void Nx2DrawPipeline::CommitDraw(const u64* entry) {
    DrawHeader header; Spans spans = ParsePacket(entry, header);        // as today (+ v2 arrays)
    WriteDrawState(header, spans);                                       // as today 1376-1397
    if (verify) VerifyDraw(...);                                         // as today
    r.FlushWork();
    ResolvedDraw* res = nullptr;
    if (K > 0) { WaitBatch(header.batch); res = &resolved.Slot(header.seq); ASSERT(res->seq == header.seq); }
    std::scoped_lock lock{r.buffer_cache.mutex, r.texture_cache.mutex};
    r.buffer_cache.MtApplyUploads(spans.uploads, spans.blob);           // BEFORE either path (same side effects)
    const bool use_resolved = res && res->status == ResolveStatus::Ok && !shadow && Validate(*res, header);
    if (res && res->status == ResolveStatus::Skip) { CountSkip(); return; }   // PIPE=resolver async-shader skip
    if (use_resolved) {
        ApplyIntents(*res);                                              // §10.2
        CommitResolved(header, *res);                                    // §10.3
    } else {
        CountFallback(res);                                              // reason: status / validation
        CommitExclusive(header, spans);                                  // phase-4 body 1399-1428 (t_capture + ConfigureDrawPipeline + RecordDraw)
        if (shadow && res && res->status == ResolveStatus::Ok) Shadow(*res, header);   // §11.1
    }
    committed_seq.store(header.seq, std::memory_order_release);
}
```

### 10.1 Validation (I4)

`Validate(res, header)` returns false on the first failure, which is counted by reason:

1. `res.channel_gen == committer channel_gen` (bumped by `BindCurrentChannel`).
2. `res.pipeline == header.pipeline` (PIPE=seq), or `pipeline_epoch` unchanged (PIPE=resolver).
3. Views: for each `ViewRefFull` that is not null, `slot_image_views.IsCurrent(view, view_gen)`,
   `slot_images.IsCurrent(image, image_gen)` and `slot_images[image].version == image_version`.
   The texture mutex is held, so these are plain reads of committer-owned state.
4. Samplers: `slot_samplers.IsCurrent(id, gen)`. They are never erased, so only the debug build
   checks.
5. Buffers: if `buffer_epoch.load() != res.buffer_epoch`, check `slot_buffers.IsCurrent(id, gen)`
   for every `BufferRef` and `VertexBind`.
6. `MT_RCHECK`: `res.desc_count == pipeline.num_descriptor_entries`.

Why this is sufficient: since resolve time, the committer may have run structure changes (draw
commits before this one, Serials) and SEQ/CPU threads may have set image flags. Every change that
would make the old path behave differently is one of the following:
* (a) a view or image erased or re-created → generation;
* (b) image state that `PrepareImage` would act on → version;
* (c) a buffer deleted or joined → generation.

Two changes that do **not** invalidate:
* The committer creating a new buffer over a range the resolver found in bounds. Impossible:
  `FindBuffer` returns the existing one.
* A new image overlapping a resolved image. `JoinImages` deletes the old image (Dead, version)
  or adds an alias (`NoteAliasesChanged` bumps the version).

### 10.2 Intents (committer, both mutexes held, in this order)

1. For `NeedsSyncBufferWrites`: `SynchronizeBufferWrites` for every distinct buffer in `buffers`.
2. Geometry: `ApplyResolvedGeometry(res)` → `channel_state->index_buffer` / `vertex_buffers[i]` /
   `vertex_buffers` enable mask from `VertexBind`s. **Only** where the values changed, mark the
   committer-mirror flags `VertexBuffer0+i` / `IndexBuffer`. The comparison is against the
   channel_state binding, which is exactly what `UpdateVertexBuffer` (buffer_cache.h:1708) does
   when dirty. **R8**: `UpdateIndexBuffer`/`UpdateVertexBuffer` skip work when the flags are
   clean. The committer's flags are exact (phase 4), so if they say clean and the resolver result
   differs from channel_state, that is a bug. `MT_RCHECK` asserts it.
3. Storage bindings → `channel_state->storage_buffers` / `written_storage_buffers` /
   `enabled_storage_buffers` (state for later ExclusiveAccess draws and `MarkWrittenBuffer`).
4. For each `BufferRef`: `TouchBuffer(slot_buffers[id], id)` and `MarkUsage(offset,size)`, deduped
   per draw. For `written` SSBOs: `MarkWrittenBuffer(id, device_addr, size)` in the old order
   (the stage loop order of `BindHostGraphicsStorageBuffers`). This keeps `QueueFenceDownload`,
   `setWriteTick`, `Nx2NoteListWrite` and pending-download subtraction where today's code puts
   them. The tracker half was already done at capture (`MtCaptureWrittenRange`) and is idempotent.
5. Views: `lru_cache.Touch(image.lru_index, frame_tick)` unless `image.last_touch_frame ==
   frame_tick` (then store). Blacklisted views: `image.scale_rating = 0`. Written storage images:
   `MarkModification(image)` in view order.
6. `texture_cache.SynchronizeDescriptors(false)`, `buffer_cache.SetUniformBuffersState(masks,
   sizes)`, and `UnbindGraphicsTextureBuffers(stage)` for enabled stages (parity with
   `ConfigureImpl`).

### 10.3 `CommitResolved` and `GraphicsPipeline::ConfigureDrawResolved`

```cpp
void Nx2DrawPipeline::CommitResolved(const DrawHeader& h, const ResolvedDraw& res) {
    RasterizerVulkan& r = rasterizer;
    GraphicsPipeline* const p = res.pipeline;
    r.mt_commit_pipeline = p; SCOPE_EXIT { r.mt_commit_pipeline = nullptr; };
    r.mesh_draw = p->IsMeshPipeline();
    p->SetEngine(r.maxwell3d.Real(), r.gpu_memory);                 // R1
    r.ApplyVertexRepack(p);                                        // ConfigureDrawPipeline 527-538, factored out
    { MtResolvedCommitScope bypass{r.buffer_cache};                // SynchronizeBuffer → true (uploads applied)
      r.buffer_cache.BindHostGeometryBuffers(h.is_indexed); }      // dirty-gated binds from ApplyResolvedGeometry
    if (r.buffer_cache.any_buffer_uploaded) { r.buffer_cache.runtime.PostCopyBarrier(); r.buffer_cache.any_buffer_uploaded = false; }
    r.texture_cache.UpdateRenderTargets(false);
    r.texture_cache.CheckFeedbackLoop(ViewsFrom(res));
    if (p->IsBuilt() && !p->HasPipeline()) { r.ReportDrawFailure("configure failed"); return; }
    if (!p->ConfigureDrawResolved(DescriptorPointer(res), res)) { r.ReportDrawFailure("configure failed"); return; }
    r.UpdateDynamicStates();
    r.query_cache.NotifySegment(true);
    r.HandleTransformFeedback();
    r.query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, r.maxwell3d->regs.zpass_pixel_count_enable);
    r.RecordDraw(h.is_indexed != 0, h.instance_count);
}
```

* `ConfigureDraw` (vk_graphics_pipeline.cpp:882) is split into
  `ConfigureDrawCommon(const DescriptorUpdateEntry* data, VkDeviceSize db_offset, u32 db_chunk,
  bool db_ready, rescaling, render_area)`. The old `ConfigureDraw` becomes a wrapper:
  `data = guest_descriptor_queue.UpdateData()`, plus the ring/reuse logic. `ConfigureDrawResolved`
  covers both RDESC modes. With RDESC on, `db_offset/db_chunk` come from the block and the
  `last_descriptor_*` memo is untouched. With RDESC off, it runs the same reuse check as today
  against the arena payload. The remainder (`RequestRenderpass`, `is_built` wait, `UpdateRescaling`,
  `UpdateGraphicsPipeline`, `UpdateDescriptorBufferChunk`, the descriptor-set elision against
  `last_descriptor_payload`, the record closure) is shared verbatim.
* The `rescaling` / `render_area` push constants come from `res` (`RescalingPushConstant::Assign`,
  as the memo path does at :846).
* The ordering difference from `ConfigureImpl`: there, `BindHostGeometryBuffers` runs before the
  stage buffers and `UpdateRenderTargets` after the image pushes. Here, geometry binds and
  render-target updates both happen on the committer, in that same relative order. The descriptor
  pushes happened earlier on the resolver and carry no scheduler side effects. Recorded commands
  keep the old order: vertex/index binds, render pass request, pipeline/descriptor closure, dynamic
  states, draw.

---

## 11. EBR integration, the unsafe-state inventory, shadow and stats

### 11.1 EBR

* **Manager**: one `VideoCommon::MT::EpochManager` per rasterizer. It is created before the
  caches attach and destroyed after them; put it first in `RasterizerVulkan`'s member order, or
  in `Nx2DrawPipeline` with detach on `Stop`. When `DeferredFree()` is on, attach it with
  `SetEpochManager` to `slot_images`, `slot_image_views`, `slot_samplers`, `slot_buffers`, every
  channel's `image_views_mt`/`samplers_mt` (in `CreateChannel`), and `graphics_cache_mt`. Detach
  everything in `Stop`, after the resolvers join and a final `Collect()`, so single-threaded use
  frees immediately again.
* **EpochGuard placement**: a resolver enters once per owned batch, before the first resolve, and
  exits after the batch publish. It also exits around every blocking wait (arena, block, UBO
  ring, ChannelBarrier spin) and re-enters after. Rule: a resolver never holds a guard while it
  waits for the committer, otherwise the committer's `Collect` can't advance and memory grows
  (R16). The committer and SEQ do not enter guards: they hold the cache mutexes, and objects are
  only retired by the committer, or by SEQ inside `SeqInnerInvalidation` under the texture mutex,
  which never erases.
* **Retire points** (all committer):
  * `StableSlotVector::erase` with an epoch attached already defers `~T` and index reuse
    (stable_slot_vector.h `erase`).
  * **Moved-out objects (R2)**: `DeleteImage` (texture_cache.h:3584) and `InvalidateScale` (:2403)
    `std::move` images and views into `sentenced_image_view` / `gpu_sentenced_images` /
    `sentenced_images` **before** `erase`. A resolver that passed the generation check may be
    reading handles while the move nulls them. That is a data race; the result is discarded by
    validation, but TSan flags it and the read is UB. Add
    `StableSlotVector::erase_then(id, F&& on_reclaim)`, which bumps the generation now and runs
    `on_reclaim(T&&)` in the EBR callback before `~T`, on the thread that calls `Collect` (the
    committer only, as below). Under MT, `DeleteImage`/`InvalidateScale` and `DeleteBuffer`
    (buffer_cache.h ~2718: `delayed_destruction_ring.Push(std::move(slot_buffers[id]))` then
    `slot_buffers.erase`) use `erase_then([this](T&& obj){ ring.Push(std::move(obj)); })`.
    The rings are committer-only, and `Collect` runs only on the committer, so this is safe.
    `RetainPendingImageDownloads` keeps `slot_images[image_id]` by reference for downloads; it
    still sees the object in place until reclaim. Ordering: Dead + `BumpVersion` move to **before**
    the view erasures in `DeleteImage` (today they come after), so a resolver that loads the
    version after a view erase sees it changed.
  * `SingleWriterMap` rebuild/Clear: already retires the old table through the attached manager.
  * Evicted pipelines (PIPE=resolver): `epochs.Retire` → push to `evicted_pipelines` with tick.
  * `Retire` takes `retire_mutex`, so any thread may call it. **`Collect` only on the committer**:
    once per committed batch (`BatchEnd` case) and at idle. `Retire`'s automatic
    `CollectLocked` at 64 entries would run reclaim callbacks on the retiring thread. Retiring is
    committer-only, so that is fine, but assert `OnCommitter()` in reclaim callbacks that touch
    rings.

### 11.2 State that `ConfigureImpl`/`ConfigureDraw`/`ConfigureDrawPipeline` touch and that is NOT safe off the committer

| State | Where | Resolver handling |
|---|---|---|
| `GraphicsPipeline::transitions`, `transition_keys`, `last_use_frame` | `AddTransition`, `Next`, `MarkUsed` (SEQ) | never read; `last_use_frame` → atomic (R21) |
| `GraphicsPipeline::tex_memo` | ConfigureImpl 405-416, 633-664, 800-846 | not used; resolver L1 instead |
| `GraphicsPipeline::last_descriptor_payload/_buffer_offset/_chunk/_generation` | ConfigureDraw 886-905, 937-946 | committer only (RDESC off) or bypassed (RDESC on) |
| `GraphicsPipeline::maxwell3d`/`gpu_memory` (`SetEngine`) | rasterizer 526 | never read by the resolver; store `Real()` (R1) |
| `GraphicsPipeline::descriptor_allocator`, `build_*`, `is_built` wait, `memo_last_handles` | ConfigureDraw, stats | committer/worker; `is_built` read-only atomic |
| `guest_descriptor_queue` (cursor, frame payload, `Acquire` → `WaitWorker`) | 760, 782-796 | replaced by `DescriptorArena` |
| `descriptor_buffer_ring` cursor/generation/frame_ticks, `Allocate` → `scheduler.Wait/Finish` | ConfigureDraw 892-903 | `AllocateBlock` only (§9.2) |
| Scheduler (`RequestRenderpass`, `UpdateRescaling`, `UpdateGraphicsPipeline`, `UpdateDescriptorBufferChunk`, `Record`, `GraphicsPipelineMissing`) | ConfigureDraw | committer only |
| `StateTracker` flags (`flags` → committer mirror) | UpdateDynamicStates, Touch* | committer only; resolvers never call `UpdateDynamicStates` |
| committer mirror `dirty.flags` (vertex repack, Vertex*/Index flags) | rasterizer 527-538, buffer cache Update*/BindHost* | committer; resolver mirror flags are separate (§6) |
| texture `DescriptorTable`s (`graphics_image_table`, `graphics_sampler_table`: read bits, cached entries, `bound_ids`, `current_limit`) | SynchronizeDescriptors, VisitImageView, GetSamplerId, TryReuse/TryFillClean | not used; the overlay + twin maps instead; the committer still runs `SynchronizeDescriptors` |
| `channel_state->image_views`/`samplers` (`std::unordered_map`), `image_view_descriptors` | VisitImageView, FindSampler, RemoveImageViewReferences | twins only; sampler writers under `sampler_create_mutex` |
| `texture_bindings_serial`, `render_targets`, `rt_active_mask`, feedback-loop cache, framebuffers, `is_rescaling` | UpdateRenderTargets, CheckFeedbackLoop, `IsRescaling()` | committer |
| `has_deleted_images` retry loop, `ScaleDown`/`InvalidateScale` from blacklist | FillImageViews | resolver → Slow (`blacklist && Rescaled`) |
| `PrepareImage`: `RefreshContents` (uploads, guest reads, I5), `SynchronizeAliases`, sparse binding, `MarkModification` (`modification_tick++`), `lru_cache.Touch` | VisitImageView | only clean images resolve; the touch/modification are intents |
| `ImageView::StorageView` lazy creation, `Sampler::HandleFor` variant emplace, `CustomBorderColorBudget` | PushImageDescriptors | `TryStorageView` (slow on null), variant emplace under `sampler_create_mutex` (§7.1.4) |
| buffer `channel_state`: `uniform_buffers[].buffer_id`, `enabled_uniform_buffer_masks`, `uniform_buffer_sizes`, `dirty_uniform_buffers`, `fast_bound_uniform_buffers`, `uniform_buffer_binding_sizes`, `storage_buffers`, `written_storage_buffers`, `enabled_storage_buffers`, `index_buffer`, `vertex_buffers`, `has_deleted_buffers`, `uniform_cache_shots/hits`, `total_graphics_storage_buffers` | SetUniformBuffersState, Bind*/Update* | resolver reads none (uses packet + mirror); the committer writes them in intents (§10.2) |
| `graphics_segments`, `graphics_ssbo_memo`, `StorageBufferMemo`/`IndexBufferMemo` | DoUpdateGraphicsBuffers, MemoStorageBufferBinding | not used (SEQ already resolved the binding) |
| `BufferCache::runtime` stream buffer (`BindMappedUniformBuffer`, `last_stream_uniform`), `UniformStreamReuseState` (global static), vertex repack layout, quad/uint8 index converters | BindHostGraphicsUniformBuffer, BindHostGeometryBuffers | `ResolverUniformRing` + per-resolver reuse; geometry binds on the committer |
| `memory_tracker`, `gpu_modified_ranges`, `uploaded_buffer_ranges`, `any_buffer_uploaded`, `SynchronizeBuffer` | sync paths | SEQ (capture) / committer (apply); never on the resolver |
| `Buffer::MarkUsage`, LRU (`TouchBuffer`), `MarkWrittenBuffer` commit half, `SynchronizeBufferWrites` | Bind*Storage/Uniform, FindBuffer | intents |
| `FindBuffer`/`CreateBuffer`/`JoinOverlap`/`DeleteBuffer`, `page_table` writes | Update* | committer; the resolver uses `TryResolveRange` |
| GMMU page table (`GpuToCpuAddress`, `IsValidEntry`, `IsContinuousRange`, `MaxContinuousRange`, `LayoutGeneration`) | many | SEQ only (packet v2 ships results) — Serial Map/Unmap run concurrently with resolvers |
| query cache, `HandleTransformFeedback`, `mesh_draw`, `mt_commit_pipeline`, `draw_counter`, `FlushWork` | ConfigureDrawPipeline, RecordDraw | committer |
| `Settings::values.*` | rescale, logging | ANY (read-only) |

### 11.3 `EDEN_NX2_MT_SHADOW`

The committer always uses the phase-4 result and compares it against the resolver's `ResolvedDraw`
(when `status == Ok` and validation passed; otherwise it counts `shadow_unvalidated`). The
compare happens inside `CommitExclusive`, after `ConfigureImpl`:

* Hook: `GraphicsPipeline::shadow_sink`, a thread_local pointer set by the committer. When set,
  `ConfigureImpl` copies out `views` (after FillImageViews), `samplers`, the payload span
  `[UpdateData(), Cursor())`, `rescaling.Data()` and `render_area`. Then the committer reads
  `channel_state->vertex_buffers[i]`/`index_buffer` binding ids/offsets after
  `UpdateGraphicsBuffers`.
* Compared fields: view ids (in order), sampler ids, payload entry count, every payload entry
  bytewise. For streamed UBO entries, compare the **bytes** at both buffers (stream buffer
  `mapped_span` vs `ResolverUniformRing` host pointer) and the range. Also rescaling words,
  render_area (uses flag + floats), vertex/index buffer ids + offsets + sizes + strides, written
  image set, and `pipeline`.
* Divergence → `VerifyReport(0x5000 | field, "MT shadow: draw {} {} resolver {} committer {}")`,
  rate-limited as today (vk_mt_pipeline.h `VerifyReport`). `shadow_checks`/`shadow_mismatches`
  counters.
* Shadow keeps the resolvers' full side effects: arena use, block writes and the UBO ring are all
  released normally. It can therefore also catch ring lifetime bugs, with lockstep off.
* `EDEN_NX2_MT_SHADOW=2`: additionally run `Validate()` and log every validation failure with the
  ids involved (finds missing `BumpVersion` sites).

### 11.4 `EDEN_NX2_MT_STATS` additions (`MT::Counters` + `LogStats`)

* Per resolver: draws/s, batches/s, average batch size, busy %, wait-for-data %, starved ms
  (arena / block / ubo), L1 hit % (views, samplers), sampler creations, variant emplaces.
* Slow reasons (`SlowReason` histogram); validation failures by reason (channel, pipeline,
  view_gen, image_gen, image_version, buffer_gen); `resolved_ok %` = committed via the resolver /
  all packets.
* Committer: `commit_wait_ns` (waiting for resolver batches), `pressure_flushes`, fallback
  (ExclusiveAccess) draws/s, `ring_wait` (SEQ back-pressure), `stream_full_ns`, `committed lag`
  (`next_seq - committed_seq` avg/max).
* EBR: `epochs.PendingCount()`, reclaimed/s, map rebuilds/s.
* UBO ring: bytes/s, reuse hit %, region waits.
* `late_pipeline_create`, `pipe_mismatch`, shadow checks/mismatches, `shadow_unvalidated`.

---

## 12. Phase 5b — per-render-pass POD replay closures (`EDEN_NX2_MT_REPLAY=1`)

Problem: each draw records several `Scheduler::Record` closures. These come from
`ConfigureDraw`'s closure, each `Update*` dynamic state that changed, vertex/index binds, and
`RecordDraw`. Each closure is a virtual-dispatch `TypedCommand` placement-new'd into a 32 KiB
`CommandChunk` (vk_scheduler.h:270-313), and the worker runs it through a linked list. The
committer pays the construction cost and the chunk fills up and dispatches often.

### 12.1 Format

A replay buffer per render pass: a `std::vector<u64>`-like arena owned by the committer, released
after worker execution, the same scheme as `DescriptorArena` (`ReplayArena`, 4 MiB ring,
`ReleaseFromWorker`). Ops are POD, 8-byte aligned, `{u16 op, u16 words}` header:

```cpp
enum class ReplayOp : u16 {
    BindPipeline,          // VkPipeline; also SetGraphicsPipelineMissing semantics: pipeline==null → skip until next BindPipeline
    PushConstants,         // VkPipelineLayout, stages, offset, size, bytes[size]
    BindDescriptorBuffers, // chunk → BindingInfo(chunk) resolved at replay (DescriptorBufferRing const)
    SetDescriptorBufferOffsets, // layout, offset
    PushDescriptorTemplate,     // template, layout, const DescriptorUpdateEntry* data (arena-owned)
    UpdateAndBindSet,           // template, layout, DescriptorAllocator*, data → Commit() at replay (worker, as today)
    BindVertexBuffers,          // first, count, VkBuffer[count], offsets[], sizes[], strides[]
    BindIndexBuffer,            // VkBuffer, offset, VkIndexType
    DynamicState,               // sub-op + POD args (viewport[16], scissor[16], depth bias, blend consts,
                                // depth bounds, stencil faces, line width, cull, front face, depth compare,
                                // stencil op, enables, topology, primitive restart, rasterizer discard,
                                // logic op, color write mask/enable/blend eq, alpha-to-coverage/one, vertex input)
    Draw, DrawIndexed, DrawMeshTasks,
    Barrier,               // BarrierFeedbackLoop
    End,
};
```

* `ReplayEncoder` (committer) with `Append<Op>(args...)`, plus the last-state mirror for elision.
* One `scheduler.Record([buf, begin, end, &replayer](vk::CommandBuffer cmdbuf){ replayer.Run(cmdbuf, buf+begin, buf+end); })`
  per **flush point**:
  * a render-pass change: `Scheduler::RequestRenderpass` sees a different framebuffer, or
    `RequestOutsideRenderPassOperationContext` / `EndRenderPass` is reached;
  * any other `scheduler.Record` by non-draw code (clears, copies, queries, barriers, compute): a
    hook in `Scheduler::RecordWithUploadBuffer` → `replay.FlushIfOpen()` **before** recording;
  * 256 draws or 64 KiB of ops;
  * `FlushWork`, `DispatchWork`, `Flush`, `Finish`, the idle hook.
  Order is preserved: the replay closure is recorded before whatever forced the flush.
* `GraphicsPipelineMissing` semantics (`scheduler.SetGraphicsPipelineMissing` inside the
  `ConfigureDraw` closure; draw closures test it) are reproduced in the replayer's local
  `pipeline_missing` flag. The scheduler's flag is set at the end of `Run` to the last value, so
  later non-replay closures see the same state.

### 12.2 Routing

* `ConfigureDrawCommon`'s record closure → `encoder.ConfigureDraw(...)`: emits
  BindDescriptorBuffers/BindPipeline/PushConstants/descriptor ops with the same conditions.
* `RecordDraw` (vk_rasterizer.cpp:556) → `encoder.Draw*`. GPU logging (`gpu_log_vulkan_calls`)
  disables replay.
* `BufferCacheRuntime::BindVertexBuffers/BindVertexBuffer/BindIndexBuffer/BindQuadIndexBuffer` →
  encoder ops when `replay.Active()`. The quad/uint8 conversions record their own copy closures
  first (non-draw Record → flush) and then the bind op.
* `RasterizerVulkan::Update*` dynamic states → `encoder.Dynamic(...)`. Each `Update*` already
  gates on `StateTracker` flags; only the `scheduler.Record` call is swapped. This touches 30+
  sites; do it with a helper `RecordDynamic(F&&)` that either appends a typed POD (when the
  lambda's capture is one of the registered POD structs) or falls back to a real Record (flush
  first). Phase 5b-1 can convert only the hot ones: viewport, scissor, depth bias, blend constants,
  stencil, cull/front face, depth test/write/compare, topology. Everything else falls back.

### 12.3 Committer-side elision (encoder state, reset at each flush and render-pass change)

* `BindPipeline` with the same VkPipeline → drop. `Scheduler::UpdateGraphicsPipeline` already
  elides by `GraphicsPipeline*`; this adds VkPipeline identity across equivalent pipelines.
* `PushConstants` with the same layout+offset+bytes as the last push for that range → drop.
  Rescaling words are mostly identical across draws, as are render area and down factor.
* `SetDescriptorBufferOffsets` with the same layout+offset → drop. With RDESC on, offsets differ
  per draw, so this rarely hits; with RDESC off and the reuse memo, it does.
* `BindVertexBuffers` / `BindIndexBuffer` with identical arrays → drop. The flags-gated binding
  usually prevents these already. Phase 4 sees redundant rebinds after `InvalidateState`.
* `DynamicState` identical to the last value of that sub-op → drop.
* All elisions are invalidated by anything that may change the command-buffer state: a flush
  point, `InvalidateState`, `RequestOutsideRenderPassOperationContext`, or a new command buffer
  (`AllocateNewContext`).
* Stats: ops/s by kind, elided/s by kind, closures/s, draws per closure.

### 12.4 Verification

`EDEN_NX2_MT_REPLAY=2`: the encoder emits the ops **and** the old closures, and the replayer runs
in "check" mode. It compares the op stream against a trace that the old closures write into a
`RecordingCommandBuffer` shim (vk::CommandBuffer has no virtual interface, so the shim is a
separate trace written by each converted closure under a debug macro). The vkCmd sequence must be
identical modulo elided duplicates; mismatches are logged. Also run with the Vulkan validation
layer (`EDEN` debug setting) for one demo run.

---

## 13. Ordered implementation steps (each compiles; behaviour-neutral until its switch)

| Step | Content | Neutral because |
|---|---|---|
| **S0** | Fix R1 (`SetEngine(maxwell3d.Real(), …)` in `ConfigureDrawPipeline` + compute), `last_use_frame` atomic, add switches (§1) | pure fixes |
| **S1** | `EpochManager` instance; `SetEpochManager` on slot vectors/twin maps (and channel twins in `CreateChannel`) when `MT::Active() && DeferredFree()`; `StableSlotVector::erase_then`; `DeleteImage`/`InvalidateScale`/`DeleteBuffer` move-out in reclaim callbacks under MT; Dead+BumpVersion before view erasure; committer `Collect()` per batch end/idle | MT off: no manager attached → immediate destruction as today; phase-4 run must stay identical (VRAM: sentenced objects are delayed by at most one Collect) |
| **S2** | `Sampler` fixed variant array + atomic count + `sampler_create_mutex` in `FindSampler`/`Emplace`; `ImageView::published_storage` + `TryStorageView`; `TextureCache::NullImageViewObject`, `IsRescalingResolved`; `BufferCacheRuntime::MakeBufferDescriptor` shared with `BindBuffer` | same behaviour, uncontended mutex |
| **S3** | `GraphicsPipeline::ResolveInfo` (+ Spec flags from `FindSpec`); `ConfigureDraw` → `ConfigureDrawCommon` split; `ApplyVertexRepack` factored out of `ConfigureDrawPipeline` | refactor only |
| **S4** | Packet v2 (`SsboEntry`, `GeometryEntry`, `tic_valid`, `batch`, `layout_gen`) + `BatchEnd`/`CloseBatch` rules + `committed_seq` + SEQ back-pressure; committer ignores the new arrays and treats `BatchEnd` as a no-op | K=0 path unchanged; VERIFY compares the v2 arrays against the committer's own `FindBuffer`/binding results (cheap new check) |
| **S5** | Texture `TryResolveViewCaptured`, `TryResolveSamplerCaptured`, `CreateSamplerFromResolver`; buffer `TryResolveRange`; `BumpVersion` audit; selftest stress tests T1-T6 (§14.2) | unused by the draw path |
| **S6** | `Resolver` threads: lifecycle, mirrors, delta application (shared `ApplyMirrorEntry`), ChannelSync barrier, per-batch EpochGuard, `ResolvedRing`, `ResultArena`; `ResolveOne` fills ids only (`views`, `samplers`, `buffers`, `vertices`; no descriptors); committer waits for batches and **only compares ids under SHADOW**, always records via phase 4 | committer output unchanged; K>0 adds threads only |
| **S7** | `DescriptorArena` + sink + worker release; `ResolverUniformRing`; `PushImageDescriptorsResolved`; resolver payload + rescaling + render area; SHADOW payload compare | output unchanged |
| **S8** | Commit path: `Validate`, `ApplyIntents`, `CommitResolved`, `ConfigureDrawResolved` (RDESC off: committer writes descriptor-buffer bytes from the arena payload with today's reuse memo); `PressureCheck`; default on with K>0 and SHADOW off | first behaviour change, A/B with K=0 |
| **S9** | `DescriptorBufferRing::AllocateBlock`/`StampBlock`, extra chunks, resolver `WriteDescriptorBuffer` (RDESC on) | switch |
| **S10** | `EDEN_NX2_MT_PIPE=resolver`: `graphics_cache_mt`, `pipeline_epoch`, SEQ eviction move, `known_hashes`/`pure_translation`, capture from per-hash `ShaderInfo`s, resolver `Refresh` + `BuiltPipeline` | switch, default off |
| **S11** | Stats (§11.4) | log only |
| **5b-1** | `ReplayArena`, `ReplayEncoder`, replayer, flush hook in `RecordWithUploadBuffer`; route `ConfigureDrawCommon` + `RecordDraw` | switch |
| **5b-2** | vertex/index bind ops; hot dynamic states | switch |
| **5b-3** | elision (§12.3) + `REPLAY=2` check mode | switch |

Run MKW title 1 (single and split screen) after S1, S4, S6, S7 (SHADOW, zero mismatches),
S8 (A/B fps), S9 and 5b-3.

---

## 14. Risks specific to this code base, and how to verify each

### 14.1 Risk table

| # | Risk | Mitigation | Verification |
|---|---|---|---|
| R1 | `SetEngine(maxwell3d, …)` stores the committer **mirror** pointer in shared `GraphicsPipeline::maxwell3d` (`ThreadEnginePtr` conversion returns `Get()`) | S0: store `Real()`; resolvers never use `pipeline->maxwell3d` | `MT_RCHECK`: assert `pipeline->maxwell3d.Real() == rasterizer.maxwell3d.Real()` at commit |
| R2 | Objects are `std::move`d out of stable slots into sentenced rings before `erase` (DeleteImage, InvalidateScale, DeleteBuffer): concurrent resolver reads of moved-from handles | `erase_then` reclaim callbacks; Dead/version before erasure | T1 under TSan; SHADOW=2 counts `view_gen` failures (expected) and no crashes; ASAN/TSan standalone of T1 |
| R3 | Lazy `ImageView::StorageView` and `Sampler::HandleFor` mutate shared objects | `TryStorageView` (slow on null), fixed variant array + mutex | T6; SHADOW payload equality; count `ViewLazyHandle` (should drop to ~0 after warm-up) |
| R4 | Deadlock committer → resolver (arena/block/UBO full) → worker/GPU → undispatched chunk on the committer | `PressureCheck` in every committer wait (DispatchWork / Flush) | T4 with a fake worker that only runs when "dispatched"; stats `pressure_flushes`; a watchdog that logs if a committer wait exceeds 500 ms with resolver states |
| R5 | Descriptor-buffer blocks reused while the GPU still reads them | stamp per batch end with `CurrentTick()`; reuse only `IsFree(tick)`; stamp again if reused within a batch | Vulkan validation layer + GPU-assisted validation run; `EDEN_NX2_MT_RDESC=0` A/B for visual diffs |
| R6 | UBO reuse across command buffers frees memory still in use | release lag `ReuseWindow` batches ≥ reuse window | SHADOW byte compare; T4 variant for the ring; poison freed regions under `MT_POISON` (0xCD) |
| R7 | GMMU page table read on resolvers while Serial Map/Unmap modify it | packet v2: SEQ translates everything; `tic_valid` from SEQ | `MT_RCHECK`: a thread-local "no translate" guard asserts in `MemoryManager::GpuToCpuAddress`/`IsContinuousRange` on the Resolver role |
| R8 | Dirty-flag divergence: the resolver mirror's flags are consumed out of order (only owned batches); the committer's exact flags gate geometry binds | forced dirty set (§6); resolver never elides; `ApplyResolvedGeometry` asserts consistency | SHADOW vertex/index compare; `MT_RCHECK` assert in §10.2 step 2 |
| R9 | Pipeline creation needs draw-time guest memory; a resolver miss can't create exactly | default PIPE=seq; resolver mode only for `pure_translation` hash tuples, late creates counted and checked under VERIFY | `pipe_mismatch == 0` over a demo before enabling; `late_pipeline_create` stat |
| R10 | Resolved draws do not update the committer's `DescriptorTable`s / `tex_memo` → those draws fall back with misses; `CheckFeedbackLoop` caches by `texture_bindings_serial` | still run `SynchronizeDescriptors` per draw; memo misses are only a cost | fallback-draw ratio in stats; feedback-loop barrier count unchanged vs K=0 (add counter) |
| R11 | Blacklisted storage images on rescaled images (`FillImageViews` → `ScaleDown`) | resolver → Slow | SHADOW; `ViewNotClean` stats |
| R12 | Missing `BumpVersion` at a flag change → a stale resolved view is used (wrong texture, missing upload) | S5 audit; SHADOW=2 | SHADOW view-id mismatches with `validation passed` = this bug class; grep audit list in the commit message |
| R13 | GC deletes buffers every frame → a global `buffer_epoch` alone would re-resolve everything | per-buffer generation is the criterion, the epoch is only a fast path | stats: validation failures by `buffer_gen` should be ≪1 % |
| R14 | LRU touches skipped → GC evicts images or buffers in use | intents touch every resolved image/buffer (dedup per frame) | compare `texture_cache` GC deletions/s, K=0 vs K=2 |
| R15 | Concurrent sampler creation: `slot_samplers`/maps single-writer assumption, `CustomBorderColorBudget` | all writers under `sampler_create_mutex` | T2 (multi-writer-under-mutex + readers) under TSan |
| R16 | EpochGuard held while blocked → `Collect` stalls → retired images (VRAM ~7.3/8 GB at 2x) pile up | release the guard around every wait; guard per batch | stats `epochs.PendingCount()` stays < a few hundred; VRAM in split screen K=0 vs K=2 |
| R17 | Batch left open while SEQ blocks (Serial) or idles → committer waits forever | CloseBatch at every non-draw entry and at SeqMark | lockstep off, `EDEN_NX2_MT_BATCH=64` run; watchdog |
| R18 | SPMC reclaim to the slowest consumer: a slow resolver throttles SEQ | priority High, small per-entry work for non-owned packets (skip by header kind) | `stream_full_ns`, per-resolver busy % |
| R19 | Channel switch while resolvers hold `TextureCacheChannelInfo*` / mirrors | barrier Serial with acks | MKW menus ↔ race transitions; a test title with multiple channels; `channel_gen` validation failures counted |
| R20 | `ResolvedRing` overrun (SEQ > 4096 draws ahead) | back-pressure in `EmitDraw` | `ASSERT(slot.seq == header.seq)` in release builds under `MT_RCHECK` |
| R21 | `last_use_frame` non-atomic SEQ write vs committer `EvictUnusedPipelines` read; eviction mutates `graphics_cache` on the committer while SEQ looks up | atomic; eviction moved to SEQ (S10, or earlier if a non-Switch-2 title is tested) | TSan on a non-Switch-2 title; Switch 2 titles never evict (vk_pipeline_cache.cpp:1235) |
| R22 | `texture_cache.IsRescaling()` (global) vs per-view `IsRescaling(view)` | resolver reads only image flags (validated by version); global on the committer | SHADOW rescaling words; test with resolution scaling 2x |
| R23 | Twin maps out of step with `image_views`/`samplers` (an erase path that misses the twin: `InvalidateScale`/`DeleteImage` go through `RemoveImageViewReferences` → ok; channel erase/reuse; `CreateChannel` storage reuse) | `EraseChannel`/`CreateChannel`: `image_views_mt.Clear()`, `samplers_mt.Clear()` when the deque slot is reused | debug check under `MT_RCHECK` every 600 frames: `image_views.size() == image_views_mt.Size()` and per-key equality |
| R24 | Resolver uses mirror regs that phase 3/4 never verified for fields only the resolver reads (render area `surface_clip`, `rt[0]`, `tex_header.limit`, `sampler_binding`) | same deltas as the committer; resolver mirror = committer mirror | VERIFY: compare resolver-mirror regs with real regs at the barrier Serial and every 1024th draw under lockstep (the resolver copies a CRC of regs into the slot; the committer compares it with `CRC(real.regs)` while SEQ waits) |
| R25 | Uploads applied after the resolver read the buffer: the resolver saw the buffer before `MtApplyUploads` created/joined buffers for this draw's ranges (`FindBuffer` inside `MtApplyUploads`) | validation is after `MtApplyUploads` → joins bump generations → fallback | count `buffer_gen` failures that coincide with uploads |
| R26 | Exceptions on resolver threads (`ASSERT`, `std::bad_alloc`) | `try/catch` around `ResolveOne`: mark `Slow/Other`, log once | fault injection under `MT_RCHECK=2` (randomly fail 1 % of resolves) → output identical (all fallback) |
| R27 | Timer slack / scheduling on 8C/16T: SEQ + committer + front + K resolvers + worker + CPU cores (MKW uses 3 guest cores) | K=2 default; resolvers High (not Critical) | A/B K=1,2,3; `pinning neutral` (plan), re-test |
| R28 | `CaptureView` overlay misses on the resolver differ from the committer's (thread_local `t_capture` reads only the overlay; the committer may fall back to guest memory on a miss) | resolver: overlay miss → Slow; never read guest memory | `OverlayMiss` stat ~0; VERIFY's existing overlay-miss report |

### 14.2 Unit stress tests (add to `mt/selftest.h`, run by `EDEN_NX2_MT_SELFTEST=1`; TSan-able standalone)

All of these use only `mt/*.h` and small fakes. They can be built **outside the CMake tree**
(no CMakeLists edits) into the scratchpad:

```
g++ -std=c++20 -O1 -g -fsanitize=thread -I src -I externals/fmt/include -DMT_SELFTEST_MAIN \
    -x c++ - -o /tmp/mt_selftest <<<'#include "video_core/mt/selftest.h"
int main(){ return VideoCommon::MT::RunAll() ? 0 : 1; }'
```

`common/assert.h` and logging need stubs. Add `mt/selftest_stubs.h`, used only when
`MT_SELFTEST_MAIN` is defined, which maps `ASSERT` to `abort` and `LOG_*` to `fprintf`. Run under
`-fsanitize=thread` and `-fsanitize=address,undefined` separately.

| Test | Shape | Checks |
|---|---|---|
| T1 slot+EBR | 1 writer (insert/erase_then with move-out into a fake sentenced ring), 3 readers (resolve by id with the §7 protocol, read a canary `u64` + a heap pointer field) for 2 s | readers never see a destroyed canary inside a guard; a stale gen is always detected at "commit"; no TSan reports |
| T2 multi-writer map | 2 writers under one mutex (InsertOrAssign/Erase), 4 readers Find; forced rebuilds | value is always a value inserted for the key; tables reclaimed (PendingCount → 0 at end) |
| T3 buffer model | fake `page_table` (atomic_ref stores), `StableSlotVector<FakeBuffer>`; writer: create/join/delete ranges, bump epoch; readers: `TryResolveRange` model, then "validate" | a resolved `{id, gen}` that validates always covers the range; joins always invalidate |
| T4 arena + worker | resolver allocates spans; a committer thread "records" release closures into a fake chunk dispatched only when full or on `PressureCheck`; a fake worker runs them | no deadlock in 10^6 allocations with a tiny capacity; no span overwritten before release (poison check) |
| T5 batch publish | SEQ thread writes packets with seq/batch; K resolver threads fill slots and publish; committer waits and reads in order | every slot read has `seq == expected` and the content hash matches; back-pressure prevents overrun with a 64-slot ring |
| T6 variant array | 4 threads `Find`/`Emplace` (under mutex) on a 32-entry fixed array | no duplicate keys, all handles readable, count monotonic |
| T7 barrier | SEQ emits a barrier Serial every 1000 packets; resolvers copy "real" state; SEQ mutates the real state only outside barriers | resolver mirrors equal the real state at each barrier |

### 14.3 Integration verification order

1. `EDEN_NX2_MT=1 EDEN_NX2_MT_RESOLVERS=2 EDEN_NX2_MT_SHADOW=1 EDEN_NX2_MT_VERIFY=1` on the MKW demo
   (alt demo per `eglantine-mkw-setup`): zero shadow and verify mismatches.
2. The same with VERIFY off (concurrency on): zero shadow mismatches; `shadow_unvalidated` small.
3. `SHADOW=2`: list all validation failures; every reason must be explainable (GC, uploads, CPU
   writes).
4. S8 on: A/B fps vs K=0 and vs MT off (randomized A/B as in previous rounds), single and split
   screen; screenshots compared (Flameshot path from memory notes) at fixed demo frames.
5. Validation-layer run (one demo lap) with RDESC on and with RDESC off.
6. 5b: `REPLAY=2` check mode zero mismatches, then A/B.
7. 30-minute soak in split screen: VRAM stable, `PendingCount` stable, no watchdog logs.

---

## 15. Summary of new/changed symbols (for grep)

* `mt/mt_switches.h`/`role.h`: `Shadow()`, `PipeOnResolver()`, `ResolverDescriptors()`,
  `ResolverUniforms()`, `Replay()`, `ResolverChecks()`.
* `mt/stable_slot_vector.h`: `erase_then`. `mt/selftest.h`: T1-T7; `selftest_stubs.h`.
* `renderer_vulkan/vk_mt_resolve.h` (new): `Resolver`, `ResolverMirrors`, `ResolvedDraw`,
  `ResolvedRing`, `DescriptorArena`, `DescriptorArenaSink`, `ResolverUniformRing`,
  `DescriptorBlockCursor`, `ResolveL1`, `SlowReason`, `ResolveStatus`.
* `vk_mt_pipeline.h`: `CloseBatch`, `SerialBarrier`, `WaitBatch`, `PressureCheck`, `Validate`,
  `ApplyIntents`, `CommitResolved`, `CommitExclusive`, `Shadow`, `ApplyMirrorEntry`,
  `committed_seq`, `epochs`, packet v2.
* `texture_cache`: `TryResolveViewCaptured`, `TryResolveSamplerCaptured`,
  `CreateSamplerFromResolver`, `sampler_create_mutex`, `NullImageViewObject`,
  `IsRescalingResolved`. `vk_texture_cache`: `ImageView::TryStorageView`, `published_storage`,
  `Sampler::TryHandleFor`, fixed `variants`.
* `buffer_cache_mt.h`: `TryResolveRange`, `BufferEpoch`, `ApplyResolvedGeometry`,
  `MtResolvedCommitScope`. `vk_buffer_cache.h`: `MakeBufferDescriptor`.
* `vk_graphics_pipeline`: `ResolveInfo`, `GetResolveInfo`, `ConfigureDrawCommon`,
  `ConfigureDrawResolved`, `shadow_sink`, Spec flags. `pipeline_helper.h`:
  `PushImageDescriptorsResolved`.
* `vk_descriptor_buffer`: `AllocateBlock`, `StampBlock`, extra chunks.
* `vk_pipeline_cache` (S10): `graphics_cache_mt`, `pipeline_epoch`, `known_hashes`, SEQ eviction.
* 5b: `ReplayArena`, `ReplayEncoder`, `ReplayOp`, replayer, `Scheduler` flush hook.
