# Cycle 11 plan (draft): config B in the 1-player race, after cycle 10 A + B

Branch mkw-mt-lite (worktree $HOME/src/Eglantine-mtB, HEAD 9eb3813: cycle 10 A = 27492f5
VRAM guard, B = 9eb3813 front waits). Config B = `EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1
EDEN_NX2_ASTC_BC7=1 EDEN_NX2_ASTC_DISKCACHE=1`. Same method as cycles 6-10: one sub-agent per
workstream, launched once, disjoint files/functions; sub-agents do not build; the coordinator
builds, integrates, commits each piece; ONE final test round. Every piece has its own switch
(Tegra::Nx2EnvMode / Nx2Switch, default on under MT lite when it should help, `=0` = today's
code path, `a` = ALT_RANDOM A/B). No CMakeLists.txt edits, no new .cpp (new headers only),
src/video_core/nx2.h read-only.

## 1. Where the time goes (cycle 10 data + race logs b0/prof1/chk1 in tools/runs)

Fast run (b0/b2/perf1, ~50 fps, GPU 53 %, fence 0):
- Load: ~85k draw packets/s (~1700 draws/frame), 6.3k compute packets/s, packets 1180 B avg,
  1.06-1.11M front macro calls/s, 3.8-3.9M same-value writes/s dropped early by REGDEDUP,
  sequencer serials ~250/s with 44-49 ms/s sequencer wait (channel ~188/s, dispatch ~62/s =
  collision-map program, forced serial "indirect/collision" ~62/s), queries 10.5k/s,
  fences 9.2k/s, barriers 7.7k/s, cond-render 1.9k/s (MT stats/s lines, log-b0.txt).
- Per-thread ceilings (busy share at 50 fps -> fps if that thread alone were the limit):
  front own work ~62 % (38 % in WaitConsumed / sched_yield / TimedWaitSettled) -> ~80 fps;
  committer busy 65-73 % (idle 270-345 ms/s in b0) -> ~70-75 fps; sequencer draws 350-410 ms/s
  + serial waits ~48 ms/s -> ~100 fps; GPU 53 % -> ~94 fps. Nothing is at 100 %: the limit is
  still the front <-> back round trips (front waits), exactly the "hidden waits" class that paid
  in every earlier cycle (per-draw micro-optimisations measured inside the +-5 fps noise).
- Front waits by reason (prof1 race window, slow run, GPU_PROFILE): puller 63-190/s (all
  waited, 27-154 ms/s, ~2.4 ms each), writers(commands) 124-565/s (79-215 waited, 31-162 ms/s,
  committer share 9-50 ms), position 19-34/s (all waited, 38-57 ms/s, ~2 ms each, all
  "payload-seq / sequencer-only", refused overlay "past known words"). Each wait costs about the
  back's backlog (~1-2.5 ms): the front runs ~2 ms ahead, every sync pays it in full.
  Cycle 10 B (ACQ_SEQ, LIST_PRECISE) only removes the committer share of B1/B2's waits; the
  sequencer share (backlog) stays.
- Committer (perf1): Idle 21.5 %, SpinRecursiveMutex 5.3 %, DoUpdateGraphicsBuffers 4.8 %,
  GpuToCpuAddress 4.1 %, PrepareImage 3.1 %, RefreshContents 2.1 %. Sequencer: BackLoop 13.2 %
  (idle spin), CaptureDraw 7.6 %, CallMethod 5.3 %, SpinRecursiveMutex 5.1 %, Execute 3.3 %,
  SeqReadDescriptor 2.5 %, GpuToCpuAddress 2.3 %, WriteBlockCached 2.2 %. prof1 back zones:
  method replay 114-116 ms/s for ~850k methods/s, draw 180-186 ms/s for ~60k draws/s.
- VRAM: usage at the driver budget in every MT run (6.25-6.46 of 6.4-6.5 GiB); texture cache
  5.7-6.2 GiB device memory vs critical 5.0-5.3 GiB in every frame; evictions 210-290/s, ASTC
  re-created 76-88/s, served from the disk cache (130-190 hits/s, hash 30-76 ms/s). That work
  runs on the committer (PrepareImage -> RefreshContents -> TryTranscodeFromDiskCache), the
  thread with the lowest ceiling. VMA slack 450-700 MiB. Cycle 10 A stops the silent host
  placement but not the churn (its emergency collections and relocations can add to it).
- GPU params v2 (chk1): ~5.8k deferred calls/s (7 % of draws), 0 mismatches, but groups are
  ALL size 1: released by 0x5f3 index_buffer.start_addr_low (76 %), 0x702 vertex_streams[0]
  address low (13 %), 0x856 pipelines[5] (fragment program) offset (6 %): every call is a
  different mesh/material. Multi-draw grouping cannot grow without bindless vertex/index
  pulling; max gain ~7 % of draws' fixed cost -> NOT a cycle 11 item.
- GPU side at 2x: 53 % busy in fast runs, BC7 encode only on disk-cache misses (0-2 per 10 s in
  the race). Not a limiter for 1 player once placement is fixed -> no GPU-side item this cycle.
- Host: cpufreq boost = 0 in all runs (Ryzen 7 5800X locked to base clock, ~-15-20 % single
  thread vs boost). Not code; tell the user (his choice) and keep it fixed across A/B arms.

## 2. Ranked workstreams

### #1 W1 "front forwarding": the front stops paying the back's backlog (biggest)
Expected gain: +8-15 % fps in fast mode if the cycle 10 fast-run residue of front waits is
>= 150 ms/s (prof1 slow-mode total ~360 ms/s; perf1 fast-mode front 38 % waiting). Reasoning:
puller + position waits are ~2-2.5 ms each because they wait for the whole sequencer backlog;
removing the wait (not shortening it) returns that time to the front, whose own work is only
~62 % at 50 fps.

F1 acquire forwarding (EDEN_NX2_C11_ACQFWD, on under LITE; EDEN_NX2_C11_ACQFWD_CHECK=1 diag):
- Case: a semaphore GEQ/EQ acquire (Puller::ProcessSemaphoreAcquire / ProcessSemaphoreTrigger,
  site 1 "pending release, value not predicted" = the QMD plain-release words B1 handles;
  ACQUIRE_STATS: met by memory in every observed case) whose satisfying release is already
  QUEUED IN THE SAME RING before it (GpuSplit pending-release table, gpu_split.h
  PredictedRelease / C10 B1 ledger in mt/c10_front.h).
- Change: the front does not wait at all. Everything after the acquire runs on the same Vulkan
  queue after the release's work (in-order sequencer -> committer -> queue; compute -> graphics
  barriers from NoteComputeWrites), so GPU-side ordering holds without the front stalling. The
  front's own later reads keep their own guards (WaitForListWriters / WaitForWriters for lists,
  FrontDmaRead positions, DMA overlay), so nothing the front reads skips a writer wait.
- Verification on the back instead of the front: split.DeferFunction(...) puts a check record
  into the ring at the acquire's position; the sequencer turns it into an ordered committer
  Command (existing Nx2DrawPipeline::Command API, no edit there) that, once the committer passed
  the release, reads the word; if not yet met (QMD release words are written by fence
  callbacks), it registers a fence-callback check. Unmet at fence time = counted + logged
  rate-limited ERROR "C11 acquire forwarded but not met" (CHECK mode: front also waits the old
  way and compares).
- Refused (old/B1 path): page GPU-written by shader work, release value known not to meet,
  acquire on a word another channel releases, CPU-released words (no pending release), any
  syncpoint/CPU-visible puller effect between release and acquire that the front itself would
  perform (must stay ordered; list them in the switch's start line).
F1b position waits (EDEN_NX2_C11_POSFWD): the 19-34/s MME DMA reads that wait for the
sequencer only ("past known words"): log which words/sizes (DMA_READ_WHY + macro index) and,
where the extra words are the 16-byte report's second u64 with a value the front knows
(D2 OVERLAY64 covers words 0-1 only), extend the overlay; else leave as is.
F2 compute write ranges on the front (EDEN_NX2_C11_LISTRANGE, contingent on the list dump):
the sequencer part of the list writer wait (WaitForListWriters waits for every compute writer
SENT because write ranges are only known at capture). The front already mirrors QMDs
(Kepler compute front mirror, a7d35eb) and reads CPU-written constant buffers: compute the SSBO
write ranges of each dispatch on the front (the same cbuf address/size words CaptureCompute
reads; refuse when a cbuf is GPU-written or bindless) and wait only for writers whose range
overlaps the list bytes. EDEN_NX2_C10_LIST_CHECK-style verify switch
(EDEN_NX2_C11_LISTRANGE_CHECK=1).
Files: gpu_split.cpp/.h (front side, ledger, the new deferred check record), puller.cpp/.h,
dma_pusher.cpp/.h, maxwell_3d.cpp FrontDmaRead + FrontNoteWrites, kepler_compute.cpp/.h front
QMD mirror functions only, mt/c10_front.h, mt/lite6_drain_switches.h, mt/lite5_list_switches.h,
new mt/c11_front.h.
Risks: a forwarded acquire that the hardware would have blocked on a LATER release (CPU or other
channel) runs work early -> wrong data; mitigated by the refusal list + back-side check (must be
0 in the test round). Deadlock is impossible by construction (front never waits on it). The
game may change its submission pattern when syncs get cheaper (WRITERS_DEDUP lesson): watch
compute dispatches/draw.

### #2 W2 "no back bubbles": channel binds and collision dispatches stop serialising
Expected gain: +3-6 %. ~250 Serials/s each stop the sequencer until the committer has drained
(44-49 ms/s of sequencer wait) and then leave the committer idle until the sequencer refills
it: a pipeline bubble on the thread with the lowest ceiling, and the front's next drain pays
the refill. RasterizerVulkan::BindChannel also calls state_tracker.InvalidateState() ~188
times/s, so the next draw re-emits all dynamic state.
- S1 same-channel elision (EDEN_NX2_C11_SAMECHAN): BindChannel to the channel already bound on
  both threads is a no-op (count binds by kind first: same / real switch; if MKW mostly
  rebinds the same channel this alone removes most of the 188/s).
- S2 channel bind as an ordered Command (EDEN_NX2_C11_CHANCMD): the sequencer switches its own
  channel context at once (gpu_memory / engine pointers it captures with: engine_ptr.h already
  separates per-thread engine views) and queues texture_cache/buffer_cache/pipeline_cache/
  query_cache BindToChannel + state_tracker.ChangeChannel as a committer Command
  (MT_COMMAND instead of MT_SERIAL in vk_rasterizer.cpp BindChannel). ReleaseChannel stays
  Serial (rare). Risk: any sequencer code reading rasterizer-level channel state that the
  committer owns (R17 in docs/mt-phase4-spec.md) -> audit list in the header.
- S3 collision-map dispatch as a packet (EDEN_NX2_C11_DISPATCH_CMD): SeqDispatch forces
  serial_dispatch for CollisionMapProgram 0x6e0ae200ee9aa220 ("publishes an image into guest
  memory after it runs"): capture it like any compute packet and queue the publish as a
  WritesGuestMemory Command with a settle mark (L9 WMARK) so front reads of that image wait
  for the committer position, not a sequencer stall. 62/s.
Files: vk_rasterizer.cpp BindChannel/ReleaseChannel/BindToChannel, vk_state_tracker.h/.cpp
(ChangeChannel / InvalidateState only), vk_mt_pipeline.h: SerialRaw, Serial, Command,
SeqDispatch, CaptureCompute, CommitCompute, StaysInBatch, WritesGuestMemory, LogStats (serial
kinds only); vk_mt_seq.h; buffer_cache.h / texture_cache.h: BindToChannel functions only;
new mt/c11_serial.h.
Risks: channel state races (sequencer translating with the new channel while the committer
still records with the old one: that is the intended ordering, but anything shared must be
split); stats must show serial waits ~48 -> <10 ms/s.

### #3 W3 "VRAM diet": stop the evict / re-create loop instead of guarding it
Expected gain: +3-6 % fps in fast mode (removes ~30-76 ms/s of hashing plus file reads,
image creation and uploads from the committer), stalls > 100 ms 3-11 per window -> ~0-2, and it
removes the root of the slow mode A only guards. Target: texture cache device memory under
critical (5.0-5.3 GiB) with ~300 MiB margin, i.e. ~0.8-1.2 GiB less.
- V1 drop the 1x originals of rescaled images (EDEN_NX2_C11_DROP1X): Image::ScaleUp keeps the
  1x image (+25 % of every rescaled image at 2x, U4 note in docs/batch8-plan.md). Free its
  memory after scale-up for render targets that are GPU-only (no CPU read / download in the
  last N frames, not aliased by a non-rescaled view); re-create it on demand in ScaleDown /
  download / alias copy (blit scaled -> new 1x). Count MiB freed and re-creations.
- V2 decode class trim on by default under DISKCACHE (EDEN_NX2_C11_DECODE_TRIM): the persistent
  ASTC decode classes (~350 MiB, all classes 64..4096) are only needed on disk-cache misses
  (0-2 per 10 s in the race); free classes unused for 600 frames (VRAM_DECODE_TRIM exists, off:
  make it the default under DISKCACHE with a shorter idle time) and allocate the miss path's
  class lazily.
- V3 per-category accounting first (part of EDEN_NX2_VRAM_STATS): render targets (1x / 2x),
  rescale originals, sampled images (ASTC BC7/BC1 / other), decode classes, staging,
  buffers, VMA slack. Without it V1/V2 are guesses; it is cheap and goes in regardless.
- V4 coordinate A: relocation of host-resident images and emergency collections only when
  usage < critical - margin (no relocation that would evict a frame-hot image), counted.
- Optional V5 (EDEN_NX2_C11_RAMTIER, off by default): keep the BC bytes of evicted ASTC images
  in a bounded system-RAM LRU (key = disk cache key) so a re-creation skips the file read
  (the hash stays: guest pages of evicted images are no longer write-tracked). Only if V1+V2
  do not get under critical. User RAM is often full (memory note): cap 256-512 MiB.
Files: texture_cache/texture_cache.h + texture_cache_base.h (GC, budget, rescale, churn and
TickFrame functions; NOT BindToChannel, NOT FillGraphicsImageViews/TryFillImageViewsClean/
TryReuseGraphicsBindings), vk_texture_cache.cpp/.h (Image ScaleUp/ScaleDown/original image,
persistent decode classes, Batch 7 T2 disk cache block), texture_cache/astc_disk_cache.h,
vulkan_common/vulkan_memory_allocator.cpp/.h, mt/c10_vram.h, mt/lite8_vram.h, mt/lite9_vram.h,
new mt/c11_vram.h.
Risks: a dropped 1x original needed by a path not covered (CPU read of a render target, copy
into a non-rescaled image) -> black/stale textures; the check is screenshots + a counter of
on-demand re-creations; rescale code is subtle (blacklisting, ScaleDown on rescale-off).

### #4 W4a "capture repeat": the committer reuses the previous draw's texture half
Expected gain: +3-8 % only once the committer is the ceiling (after #1/#2 it should be: 65-73 %
busy at 50 fps -> ~85-95 % at 60-65 fps); ~0 before. Earlier TEX_MEMO (5ecec58, off) was
measured in the single-threaded back, where the TIC re-read was on the critical thread; here
the sequencer already reads every TIC/TSC entry, so the committer can trust it.
- Sequencer (CaptureDraw / FinishOverlayCompact): keep, per pipeline, the previous capture's
  descriptor read bytes (raw_reads with descriptor=true + the texture handle words); if this
  draw's are byte-identical, set a packet flag `tex_repeat` (and skip emitting those overlay
  entries). Counter first: repeat rate per 10 s (EDEN_NX2_C11_REPEAT_STATS) - the go/no-go.
- Committer (CommitDraw -> GraphicsPipeline::ConfigureImpl): with `tex_repeat`, the channel
  unchanged and the texture cache view epoch / bindings serial unchanged (TextureMemo's
  existing validity fields), reuse tex_memo's view/sampler ids, descriptor entries and
  rescaling words without read_handle / FillImageViews / PrepareImage for sampled images;
  written images (storage, render targets) still go through PrepareImage / MarkModification.
  RefreshContents for sampled images is skipped only if the texture cache's CPU-modified
  epoch (new counter bumped by WriteMemory/InvalidateRegion marking any image) is unchanged.
Switch: EDEN_NX2_C11_REPEAT (on under LITE), EDEN_NX2_C11_REPEAT_CHECK=1 (do the full path and
compare view ids / descriptor entries; mismatches must be 0).

### #5 W4b "one lock per run of packets": sequencer/committer lock hand-offs
Expected gain: +2-4 % (SpinRecursiveMutex 5.1 % sequencer + 5.3 % committer self time; earlier
"cache locks once per draw / per QMD launch" commits gained +2.4 fps each). Contingent on
EDEN_NX2_LOCK_STATS (which lock, which waiter/owner):
- If buffer/texture mutex vs guest traps / fence thread: the committer takes both cache locks
  once per run of consecutive draw packets (up to 16, or until a contender is spinning: add a
  `contended` flag set in SpinRecursiveMutex::LockContended and checked between packets)
  instead of twice per draw (CommitDraw: uploads under buffer lock, then ConfigureDrawPipeline
  takes both).
- If tracker_mutex sequencer vs committer: committer-side tracker writes per draw
  (MarkWrittenBuffer, IsRegionGpuModified, MtFlushWrittenBatch) batched into one TrackerScope
  per run; SynchronizeBuffer already skips the tracker under t_capture.
- If it is uncontended cost (cache-line ping-pong): same batching helps; also replace
  std::this_thread::get_id() with a thread_local id in SpinRecursiveMutex::lock.
Switch: EDEN_NX2_C11_LOCKRUN (on under LITE).
W4 files (a+b): vk_mt_pipeline.h: CaptureDraw, SeqReadDescriptor, SeqTakeUploads,
FinishOverlayCompact*, CommitDraw, CommitterLoop, ParsePacket; mt/draw_packet.h,
packet_compact.h, capture.h, capture_plan.h, tracker_lock.h; vk_graphics_pipeline.cpp/.h
(ConfigureImpl, TextureMemo); texture_cache.h: FillGraphicsImageViews,
TryFillImageViewsClean, TryReuseGraphicsBindings + a new epoch getter (one line agreed with
W3); buffer_cache.h + buffer_cache_base.h (except BindToChannel); common/spin_recursive_mutex.h
and spin_recursive_mutex_stats.h; vk_rasterizer.cpp ConfigureDrawPipeline only; new
mt/c11_commit.h.
Risks: memo reuse after an image changed without the epoch moving (alias sync, rescale,
GPU-modified sampled image needing a barrier) -> wrong texture; REPEAT_CHECK must be 0.
Longer lock holds can starve guest CPU write traps -> the `contended` break.

Not chosen (with reason): multi-draw grouping (groups size 1 by construction, 7 % of draws);
sequencer register-delta blocks (back method replay ~115 ms/s, but SPLIT_STATE-style blocks
measured 0 before and the sequencer is not a ceiling); thread pinning (measured +0.1 +- 5);
FRONT2 parse thread (round 6 census: barriers too dense); GPU-side encoder/render pass work
(GPU 53 % in fast mode); resolvers (latency, mt-round3).

## 3. File ownership (disjoint; shared files split by function, small local Edit calls)

| agent | owns |
|---|---|
| W1 front forwarding | gpu_split.cpp/.h (all except BackLoop's serial/command plumbing W2 touches: none expected), engines/puller.cpp/.h, dma_pusher.cpp/.h, maxwell_3d.cpp FrontDmaRead + FrontNoteWrites, kepler_compute.cpp/.h front QMD mirror functions, mt/c10_front.h, mt/lite5_list_switches.h, mt/lite6_drain_switches.h, new mt/c11_front.h |
| W2 serial bubbles | vk_rasterizer.cpp BindChannel / ReleaseChannel / BindToChannel / the MT_SERIAL(Channel) lines, vk_state_tracker.*, vk_mt_pipeline.h SerialRaw / Serial / Command / SeqDispatch / CaptureCompute / CommitCompute / StaysInBatch / WritesGuestMemory / serial counters in LogStats, vk_mt_seq.h, BindToChannel in buffer_cache.h and texture_cache.h, mt/engine_ptr.h, new mt/c11_serial.h |
| W3 VRAM diet | texture_cache.h + texture_cache_base.h GC/budget/rescale/churn/TickFrame functions, vk_texture_cache.cpp/.h (Image, scaling, decode classes, T2 block), astc_disk_cache.h, vulkan_memory_allocator.*, mt/c10_vram.h, lite8_vram.h, lite9_vram.h, new mt/c11_vram.h |
| W4 committer (a REPEAT + b LOCKRUN) | vk_mt_pipeline.h CaptureDraw / SeqReadDescriptor / SeqTakeUploads / FinishOverlayCompact* / CommitDraw / CommitterLoop / ParsePacket, draw_packet.h, packet_compact.h, capture.h, capture_plan.h, tracker_lock.h, vk_graphics_pipeline.*, texture_cache.h FillGraphicsImageViews / TryFillImageViewsClean / TryReuseGraphicsBindings, buffer_cache.h + buffer_cache_base.h (except BindToChannel), common/spin_recursive_mutex*.h, vk_rasterizer.cpp ConfigureDrawPipeline, new mt/c11_commit.h |
| coordinator | this doc (docs/cycle11-plan.md), build, integration, commits, launcher, docs/cycle11-results.md, the test round |

Hooks needed in a file another agent owns are reported as exact lines, not edited (e.g. W4's
texture cache CPU-modified epoch bump inside W3's WriteMemory/InvalidateRegion functions).

## 4. Final test round (cycle 11, once)
Race (tools/race), 2x, VSYNC_HZ 240, speed limit off, idle PC, boost state recorded:
stable vs B+c10 vs B+c10+c11 (3 each, alternating) with nvidia-smi; ALT_RANDOM runs for the
switches that are cheap to alternate (C11_ACQFWD=a, C11_REPEAT=a, C11_LOCKRUN=a; SAMECHAN /
CHANCMD / DROP1X are not alternated: structural state, compare whole runs); checks:
C11_ACQFWD_CHECK (0 unmet), C11_LISTRANGE_CHECK (0 stale), C11_REPEAT_CHECK (0 mismatches),
chk1/chk2 suite (GPUPARAM_CHECK, REGDEDUP_CHECK, DMA_READ_CHECK, MT_WATCHDOG, MT_VERIFY,
LITE7_CAPTURE_CHECK, C10_LIST_CHECK), VRAM_STATS + TC_CHURN (evictions/s, host-resident 0,
per-category MiB), screenshots in the race window (textures after DROP1X, HUD, water). Watch
compute dispatches/draw (the game may change its work when syncs get cheaper).

## 5. Data the cycle 10 final round should capture (missing today)
1. A fast-run GPU_PROFILE (separate from the fps runs; it costs 25-35 %): "GPU split drains
   per second" with the new C10 counters (acq_tries/nowait/short/full/unmet/refused,
   list_calls/nowait/short/kept) and the front/back per-second zones. This decides W1's size:
   residue ms/s per reason (puller, writers(commands), position) after B.
2. EDEN_NX2_PULLER_STATS + EDEN_NX2_ACQUIRE_STATS in the race: remaining acquire sites/ops/
   addresses after B1 (W1-F1 refusal list).
3. EDEN_NX2_LOCK_STATS=1 (one run): waits by lock name (buffer / texture / tracker) and by
   waiter/owner thread (GPUBack, GPUCommit, guest cores, VulkanWorker, PresentDefer) - W4b's
   design choice.
4. EDEN_NX2_LIST_DUMP (short window inside the race) + DMA_READ_WHY: what the ~200
   waited/s command lists and the ~26/s "past known words" DMA reads are (W1-F2 / F1b go/no-go).
5. A count of BindChannel calls per 10 s split into same-channel rebinds vs real switches
   (one LOG line in RasterizerVulkan::BindChannel if no counter exists; else the MT stats
   "serial channel" number plus channel ids) - W2-S1 vs S2.
6. VRAM_STATS + TC_CHURN with A on: host-resident images/buffers (must be 0), emergency
   collections and relocations per 10 s, evictions/s and ASTC re-created/s compared with
   before A (did A raise the churn?), disk cache hits and hash ms per 10 s.
7. perf per thread (GPU, GPUBack, GPUCommit) saved as TEXT (perf report --no-children top 40
   and --children top 40 with call graph) so SpinRecursiveMutex splits into contended spin vs
   uncontended cost and DoUpdateGraphicsBuffers/PrepareImage callers are known.
8. top -H per thread (front, GPUBack, GPUCommit, VulkanWorker, PresentDefer, 6 guest cores)
   and EDEN_NX2_WAIT_STATS (guest waits on GPU syncpoints per frame) + "last 600 presents"
   fence line: is the game ever waiting on GPU completion in the fast mode?
9. Bimodality after A: >= 3 B runs; any run < 40 fps must show host-resident > 0 or not.
10. Boost state (/sys/devices/system/cpu/cpufreq/boost) noted in every run header.
