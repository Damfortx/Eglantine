# Cycle 12 plan: from 1P ~49 fps toward split screen at a locked 60 (config B, mkw-c11)

Goal (user): 2-player split screen at a locked 60 fps, 2x resolution, RTX 3070 Ti 8 GB, Ryzen 7
5800X, **CPU boost off** (`/sys/devices/system/cpu/cpufreq/boost` = 0 on 2026-11-10, in every c11b
run and at the start of this cycle). Base: mkw-c11 9b56f022f4 + 97424c1c1c docs, binary
`build/pgo/bin/eden-cli`, config B = `EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 EDEN_NX2_ASTC_BC7=1
EDEN_NX2_ASTC_DISKCACHE=1`. 1-player race mean 48.6 fps (c11b def-1/2/3: 47.1 / 49.2 / 49.5).
Method as cycles 6-11: one agent per workstream, disjoint files/functions, header-only new code
under `src/video_core/mt/`, no CMakeLists.txt, `src/video_core/nx2.h` read-only, the coordinator
builds/integrates/commits, ONE final test round.

## 0. Data status (read this first)

The data round for this plan was **paused before the first run** (coordinator PAUSE, the user was
using the PC: the installed `eglantine` GUI was running from 17:29 and the user then played another
game). **0 of the 5 allowed emulator runs were made**; no lock file was created. Everything in §1
comes from existing logs: the cycle 11b round (`tools/runs/*-c11b-*`, 2026-11-10, counters
MT_STATS / VRAM_STATS / TC_CHURN / C11_STATS, top -H, nvidia-smi), the c11-chk run, and cycle 10's
fast-run perf (`perf1`, symbols in docs/cycle10-data.md; the .data files no longer resolve against
the current binary). **There is no 2-player measurement at all** (the `*-split*` / `ssboa` logs in
tools/runs are GpuSplit demo runs, not split screen) and the split-screen timeline
(`tools/splitscreen/timeline.txt`) is still an uncalibrated guess. §1.2 is therefore a projection,
and §5 lists the 5 runs to make on RESUME; their numbers decide the gates in §2.

Prepared for the calibration: a session-scratch `shots.sh <dir> <every_s> <delay_s>` (portal
screenshots while eden-cli runs = race.sh's `shot()` loop; splitscreen.sh has no CALIBRATE option;
not committed, recreate it from race.sh if the scratch is gone) and
`race/calibmap.py` maps the shots to present numbers from the 2P log as for the 1P race.

## 1. Where the time goes

### 1.1 One player (c11b def-2, def-3, cap-2 race windows, 49-54 fps; per-s means of the 10 s lines)

| item | value | per frame (at ~50 fps) |
|---|---|---|
| draw packets | 92-118 k/s | ~2100-2300 draws |
| compute packets | 5.2-6.2 k/s | ~115 |
| fences / queries / barriers / cond-render / clears | 7.2-8.4 k / 8.6-12.2 k / 5.5-6.6 k / 1.3-1.7 k / 1.6-1.9 k per s | 155 / 190 / 120 / 30 / 35 |
| delta words (sequencer -> committer mirror) | 1.9-2.3 M/s | ~19 per draw |
| serials | 20-56/s, 1.7-10.3 ms/s wait (external, flush, blit) | ~0.15 ms |
| sequencer "draws" zone | 428-480 ms/s | 8.9-9.7 ms (4.2 us/draw) |
| sequencer wait-committed | 25-30 ms/s = the S3 collision publish guard, 44-53 waits/s at ~0.55 ms | 0.55 ms |
| committer idle | 286-318 ms/s -> busy 680-715 ms/s | ~14 ms (6.3 us/draw) |
| committer lock runs (LOCKRUN) | 1.9-2.1 draws/run; runs ended by **idle 70-75 %** (34-43 k/s) | committer catches up with the ring every ~2 draws |
| top -H (window) | front 77-83 %, GPUBack 76-84 %, GPUCommit 78-80 %, 6 guest cores 61-66 % each, VulkanWorker 27 % | (top counts spin waits as busy) |
| GPU (nvidia-smi) | 40-49 % util, 139-155 W, 7.0 GB | ~9.5 ms if util ~ busy (GPU_TIMESTAMPS not yet run) |
| VRAM / churn | TC device memory 5.7-6.2 GiB vs critical 5.7-6.2 GiB (over critical on 2-85 % of frames); evictions 32-260/s; ASTC re-created 7-23/s; disk cache ~60 hits/s, hash 3-61 ms/s (committer) | |
| front position waits | `posfwd past-known` 59-96/s, always `[macro 0x0 +0 24B of 16B direct]` | 1.2-1.9 waits |
| present | "blocked on the GPU fence" 0 us, "600 ran a frame ahead" | GPU never the limit in fast mode |

Front breakdown (no c11 GPU_PROFILE run exists; cycle 10 fast-run perf1 at 51.7 fps): own work
~62 % (FrontRunMacro 13.8 %, ProcessCommands 7.1 %, FrontWrite 5.5 %, MacroWrite 4.5 %, DmaPusher
CallMethod 4.1 %, rest diffuse) and ~38 % waiting on the back (WaitConsumed 18 %, sched_yield
15.2 %, TimedWaitSettled 5.1 %). Drain reasons (cycle 10 prof1, slow mode, per s): writers
(commands) 235-270 waited (92-103 ms), puller 89-95 waited (36-58 ms), position 29-34 waited
(18-37 ms). Cycle 11 then showed: no acquire left to forward (ACQFWD 0-7 tries / 10 s), channel
binds are Commands, serial waits fell to 2-10 ms/s, but the position waits are 59-96/s (more than
the 29-34/s of cycle 10: assists ON, more draws).

**Per-thread ceilings, 1P** (fps if only that thread limited, own work at 100 % of a core):

| thread / resource | own work per frame | ceiling | distance to 60 |
|---|---|---|---|
| front (GPU thread) own work | ~12 ms (62 % at 51.7 fps, c10) | ~83 fps | ok |
| front own + drains (critical path) | ~12 + ~7.5 ms = ~19.5 ms | **~50 fps = what we measure** | **-17 %** |
| committer | ~14 ms | ~70 fps (~63 at a realistic 90 % for a pipelined stage) | ok, thin |
| sequencer | ~9.5 ms draws + ~3-4 ms replay/compute/other (c10 prof1: method replay ~115 ms/s) | ~75 fps | ok |
| GPU | ~9.5 ms (util) | ~100 fps | ok |
| guest CPU cores | 61-66 % each | not limiting (they wait on the GPU side) | - |

**The 1P ceiling is the front's critical path, not a saturated thread:** nothing is at 100 %; the
committer idles 300 ms/s and ends 70 % of its lock runs because the ring is empty, i.e. the back
waits for the front, and the front waits for the back on each drain (each costs about the back's
backlog, 1-2.5 ms). Same shape as cycles 6-11. To lock 60 in 1P the front's frame (own + drains)
must fall from ~19.5 to < 16 ms: remove ~3.5-4 ms of drains per frame (half of them) or cut own
work by ~30 %.

### 1.2 Two players (projection, NOT measured: gate data in §5)

Split screen at the same output size draws both views: geometry, shadows, culling compute and
the per-draw CPU work roughly double (assume 1.8-2.0x draws: ~4000-4500 per frame), pixel work
stays about the same (same total resolution) except per-view passes (shadows, reflections,
post-processing at half size).

| thread / resource | 2P per frame (x1.8-2.0) | ceiling | distance to 60 |
|---|---|---|---|
| front own work | 21.5-24 ms | 42-46 fps | -30 % |
| front own + drains | 26-33 ms (drains do not double if they are per frame/per view sync) | **30-38 fps** | -40-50 % |
| committer | 25-28 ms | **36-40 fps** | -35-40 % |
| sequencer | 22-26 ms | 38-45 fps | -30-35 % |
| GPU | 12-16 ms (geometry x2, pixels ~x1.1) | 62-80 fps | ~ok, check |
| VRAM | per-view RTs + shadow maps: +0.2-0.6 GiB over a budget that already sits at critical | churn risk | check |

So in 2P **all three CPU threads are above 16.7 ms per frame**: the committer and the front are
the projected ceilings at ~35-40 fps, the sequencer right behind. A locked 60 fps 2P with boost
off needs every per-draw CPU path ~2x cheaper (front 5.5 -> ~3 us/draw, sequencer ~5.5 -> ~3,
committer 6.3 -> ~3); that is more than one cycle. With boost on (5800X 3.8 -> up to 4.7 GHz,
an estimated +15-20 % single thread, not measured here) the same code gains ~+15 % in every
thread for free; boost is the user's choice and stays off in all A/B comparisons (recorded per
run). Realistic cycle 12 target: **1P locked 60 (+23 %) and 2P ~40-45 fps**, with the 2P data
deciding the order of W2/W3.

## 2. Ranked workstreams (each a structural change, expected >= +10 % on its ceiling)

Order: W1 is the 1P ceiling now; W2/W3 are the 2P ceilings (and the 1P ones right after W1); W4
is gated on the 2P GPU/VRAM numbers. All switches `EDEN_NX2_C12_*`: default on under MT lite when
they should help, `=0` = the cycle 11 path, `=a` = ALT_RANDOM A/B where the state allows it,
`_CHECK=1` = do the old path too and compare (counter + rate-limited ERROR "C12 <piece> check"),
`EDEN_NX2_C12_STATS=1` = one 10 s line per workstream.

### W1 "front drains, second pass" (1P ceiling; expected +12-20 % 1P, +8-12 % 2P)
Evidence: front ~38 % waiting at ~50 fps (c10 perf1), position waits 59-96/s with ONE shape, the
writers(commands) list waits ~250/s, LISTRANGE implemented in c11 with both hooks in place but
never run.
- **P1 adjacent-release overlay (EDEN_NX2_C12_POSADJ, CHECK).** Every "past known words" read is
  `macro 0x0 +0, 24 bytes over a 16-byte direct release`: the MME reads the 16-byte report AND the
  next 8 bytes. New data since cycle 11 rejected POSFWD (which only supplied words 2-3): find the
  writer of bytes 16-23 (in the C12 stats line: the noted write that covers them, if any, its
  kind and channel; else "unnoted"). If it is the neighbouring release/report of the same
  sequence (the common GPU layout of consecutive 16-byte reports or a 64-bit sequence word), lay
  BOTH noted payloads into the overlay (extend `KnownPendingWords` to merge adjacent pending
  writes when every byte of the read is covered by noted, front-known values) instead of a
  position wait. If the bytes are unnoted guest memory that the front can read directly (no
  pending writer overlaps them: the read is "past known" only because the range check is per
  write, not per byte), read them from memory. Gain: 59-96 waits/s x ~1-2 ms = 60-190 ms/s of
  front time = 1.2-3.8 ms/frame, the largest single block left. CHECK: also wait and compare all
  24 bytes (mismatches must be 0).
- **P2 LISTRANGE on (EDEN_NX2_C11_LISTRANGE, existing, + EDEN_NX2_C11_LISTRANGE_CHECK=1 in the
  data round).** Default flips to on under LITE only if the data run shows "shorter" > 30 % of list
  calls and 0 stale. No new code unless the stats show a refusal class worth fixing (stale cbuf,
  unknown program): those fixes belong to W1.
- **P3 writer census -> one fix (EDEN_NX2_C12_LISTWAIT).** With GPU_PROFILE in the data round,
  split the remaining writers(commands) waits by last writer kind (compute culling program id /
  copy / I2M / 2D) and by whether the list bytes were in fact changed by that writer (CHECK
  re-read: if the list content is identical before and after the wait in > 90 % of cases, the
  writer is a "same content" rewrite: let the front parse the current bytes speculatively and
  verify at the committer position with the existing list CHECK machinery; on a mismatch the
  front is rewound to the list start, which is only legal before any method of the list was
  sent to the ring, so the speculation stops at the first method that is not buffered in the
  front's gather (FlushMacroList); counted). Implement only the census unless the data shows
  >= 100 waits/s of one kind.
Not repeated: ACQFWD (no cases), FRONT2 parse thread (barriers too dense; re-check only with the
2P census, W5), GPUPARAM v1 (slower), resolvers.

### W2 "committer whole-draw repeat" (committer ceiling; expected committer -25-35 % per draw on
repeats -> +15-25 % on the 2P committer ceiling, +5-8 % 1P after W1)
Evidence: c11 REPEAT_CHECK: 42-59 % of textured captures have byte-identical texture state, 0
mismatches in ~1.4 M checks; the committer's self time is spread over DoUpdateGraphicsBuffers
(4.8 %), GpuToCpuAddress (4.1 %), PrepareImage (3.1 %), RefreshContents (2.1 %), SpinRecursiveMutex
(5.3 %), push descriptors. REPEAT (textures only) was correctly left off in 1P because the
committer idles 300 ms/s; in 2P it is the projected ceiling, which was the plan's own condition.
- Extend the repeat key from textures to the whole binding state: uniform/storage buffer
  bindings (GPU address, size, buffer id, the buffer cache's write epoch), vertex/index bindings
  (address + size; offsets may differ: they go into the draw record), render targets (framebuffer
  id, unchanged since the previous draw of the same pipeline), push constants, rescaling words.
  On a full repeat the committer skips ConfigureImpl except the uploads that changed (inline
  uniforms) and the draw record; the replay arena already elides same-payload state records.
- Validity = the same epochs REPEAT uses (packet_seq chain, view epoch, bindings serial, CPU
  modified epoch, page-table layout gen) plus a new buffer cache epoch bumped by every buffer
  creation/deletion/CPU write/MarkWrittenBuffer of a bound range.
- Turn on the existing REPEAT as the texture half (C11_REPEAT=1 inside C12_COMMIT_REPEAT).
- Switch EDEN_NX2_C12_COMMIT_REPEAT (on under LITE, `a` per draw), CHECK compares descriptor
  entries, buffer ids/offsets/sizes, framebuffer id, push words: 0 mismatches.

### W3 "sequencer capture repeat with write generations" (sequencer ceiling; expected sequencer
draws zone -25-35 % -> +15-20 % on the 2P sequencer ceiling)
Evidence: sequencer draws 428-480 ms/s = 4.2 us/draw; CaptureDraw 7.6 %, SeqReadDescriptor 2.5 %,
WriteBlockCached 2.2 %, GpuToCpuAddress 2.3 %; DESCDEDUP finds 1/3 of descriptor reads duplicated
inside a draw. Batch 7 rejected a whole-capture memo because "guest memory has no exact unchanged
signal". That signal now exists: the MT_UBO_GEN 4 KiB page write generations (seq_diet.h, bumped
by every guest write the GPU side can see, used for uniform staging reuse; off and unmeasured).
- Per pipeline slot (the REPEAT table W4a built): keep the previous capture's raw reads (TIC/TSC
  bytes, uniform bytes, the upload list) with the write generations of the pages they came from.
  If every page generation and the register-side key (bound cbufs, handles, root tables) are
  unchanged, emit the packet by reference ("same as packet N" + the changed vertex/index words):
  no re-read, no re-hash, smaller packets (1.4 KB avg today) -> also less committer parse.
- Correctness: generations miss writes that bypass the tracker (none known; MT_VERIFY's
  "concurrent CPU writes" counter is the alarm); CHECK re-reads and compares every byte.
- Switch EDEN_NX2_C12_CAPTURE_REPEAT (on under LITE after CHECK = 0; turns MT_UBO_GEN on), CHECK.
- W2 and W3 share the packet flag: W3 owns draw_packet.h/packet_compact.h and defines
  `c12_same_as` (u32, in existing padding); W2 only reads it.

### W4 "split-screen GPU / VRAM" (gated on 2P data; expected +10 % only if 2P is GPU- or
VRAM-bound)
- Gate A (GPU): GPU_TIMESTAMPS in 2P > 13 ms/frame or "blocked on the GPU fence" > 1 ms/frame ->
  look at the per-pass cost (per-view shadow/post passes at 2x: rescale only the views' colour
  targets, keep shadow maps 1x when the game sizes them per view: a rescale-rule switch
  EDEN_NX2_C12_SS_RESCALE).
- Gate B (VRAM): 2P TC device memory over critical on > 50 % of frames or evictions > 150/s ->
  DROP1X AGE 300 -> 120 frames for 2P, cap 1024 (EDEN_NX2_C11_PRESSURE_CAP_MB, safe in c11b) and a
  2P-aware critical (budget minus the measured non-texture use, not a fixed fraction:
  EDEN_NX2_C12_VRAM_DYNCRIT).
- If neither gate fires, W4 is dropped and its agent takes W5's census.

### W5 (contingent) front parse/execute split for 2P
Only if the 2P census (`mt/front2.h` line, already in the binary) shows barrier density per
1000 methods at least 2x lower than the round 6 demo census AND the front is the 2P ceiling after
W1. Otherwise not this cycle (rejected in round 6 on density; needs that new data).

### Not chosen (reason)
Thread pinning (+0.1 +- 5), multi-draw grouping (groups size 1), GPU-side encoder work (GPU 40-49
% in 1P), lock micro-work (LOCKRUN already halves lock pairs, runs end on idle), POSFWD timestamp
words (wrong words), pressure cap default (inconclusive), RAMTIER (RAM often full).

## 3. File ownership (disjoint; shared files split by function)

| agent | owns | new header |
|---|---|---|
| W1 front drains | gpu_split.cpp/.h, dma_pusher.cpp/.h, engines/puller.cpp/.h, maxwell_3d.cpp FrontDmaRead / FrontNoteWrites / FrontCallMultiMethod, kepler_compute.cpp/.h front QMD mirror, mt/c10_front.h, mt/c11_front.h, mt/lite5_list_switches.h, mt/lite6_drain_switches.h, mt/front2.h (W5 census reading only) | mt/c12_front.h |
| W2 committer repeat | vk_mt_pipeline.h CommitDraw / CommitterLoop / BeginRun / EndRun / ParsePacket (reading the W3 flag), vk_graphics_pipeline.cpp/.h (ConfigureImpl, TextureMemo), vk_rasterizer.cpp ConfigureDrawPipeline, buffer_cache.h + buffer_cache_base.h (new epoch, bind paths; not BindToChannel), texture_cache.h TryRepeatGraphicsBindings / TryReuseGraphicsBindings / FillGraphicsImageViews, mt/c11_commit.h, common/spin_recursive_mutex*.h | mt/c12_commit.h |
| W3 capture repeat | vk_mt_pipeline.h CaptureDraw / SeqReadDescriptor / SeqTakeUploads / FinishOverlayCompact* / EmitDraw, vk_mt_seq.h, mt/draw_packet.h, mt/packet_compact.h, mt/capture.h, mt/capture_plan.h, mt/seq_diet.h, mt/tracker_lock.h, mt/lite7_capture_switches.h | mt/c12_capture.h |
| W4 2P GPU/VRAM | texture_cache.h + texture_cache_base.h GC / budget / rescale / TickFrame (not W2's functions), vk_texture_cache.cpp/.h, vulkan_common/vulkan_memory_allocator.cpp/.h, mt/c10_vram.h, c11_vram.h, lite8_vram.h, lite9_vram.h, renderer_vulkan/vk_gpu_timestamps.h | mt/c12_vram.h |
| coordinator | this doc, the 2P calibration (tools/splitscreen/timeline.txt, outside src), build, integration, commits, docs/cycle12-results.md, the test round | - |

vk_mt_pipeline.h is split by function between W2 (commit side) and W3 (capture side); the packet
layout (draw_packet.h, packet_compact.h) is W3's only. Hooks needed in another agent's file go
into docs/cycle12-hooks-W<n>.md as exact lines (e.g. W2's buffer epoch bump in a W4 texture
cache function, W3's generation read in a W1 front function), the coordinator applies them.

## 4. Risks

- W1 P1: an adjacent payload whose value the front does not really know (a release overtaken by a
  GPU write of a shader) -> wrong MME parameter -> wrong draw. CHECK must be 0 and the merge
  refuses GPU-written pages. P3 speculation is the riskiest piece: census first, code only with
  data.
- W2: stale binding reuse after a buffer re-creation or a GPU write without a barrier; the epoch
  must be bumped on every buffer cache mutation path (audit list in c12_commit.h). Screenshots +
  CHECK 0.
- W3: a guest write that bypasses the tracker (the reason batch 7 rejected the memo); CHECK must
  be 0 over a whole race and MT_VERIFY must stay 0. The game may change its submission pattern
  once captures are cheaper (WRITERS_DEDUP lesson): watch draws/frame.
- 2P route: uncalibrated timeline; 2 calibration runs may not be enough (menus unknown). If the 2P
  race is not reached in 2 runs, the round falls back to 1P only and 2P stays a projection.
- Boost off is a fixed ~15-20 % handicap that no cycle removes; record it in every run header.
- VRAM: 2P may push the 8 GB card back into the cycle 10 slow mode (host placement); host
  resident must stay 0.

## 5. Data round still to do (on RESUME; max 5 runs; one emulator at a time, round lock)

Common: `NOPROF=1 EDEN_NX2_VSYNC_HZ=240`, config B, `EDEN_NX2_MT_STATS=1 EDEN_NX2_VRAM_STATS=1
EDEN_NX2_TC_CHURN=1 EDEN_NX2_C11_STATS=1 EDEN_NX2_GPU_TIMESTAMPS=1`, boost recorded, nvidia-smi.
1. **1P counters + perf**: + `EDEN_NX2_LOCK_STATS=1 EDEN_NX2_WAIT_STATS=1
   EDEN_NX2_C11_LISTRANGE=1 EDEN_NX2_C11_LISTRANGE_CHECK=1`; perfthreads.sh (THREADS GPU, GPUBack,
   GPUCommit one at a time, 10 s each, text reports) after markend. Gives per-thread self time,
   GPU ms/frame, guest waits, lock waits, LISTRANGE shorter/stale.
2. **1P GPU_PROFILE** (no NOPROF): "GPU split drains per second" by reason (ms/s, committer
   share), front/back zones, front2 census. Decides W1 P1/P3 sizes.
3. **2P calibration 1**: splitscreen.sh with the counters of run 1 + the shots loop every
   2 s; fix timeline.txt from the shots (calibmap.py frames). If it reaches the race, it is the 2P
   counters run.
4. **2P calibration 2 / counters run** (+ perf after the window if 3 already reached the race).
5. **2P GPU_PROFILE** (drains by reason, front2 census for W5).
Abort any run under 10 fps for > 60 s in game ("aborted <10 fps").
Then fill §1 with the measured numbers and re-rank: the workstream whose thread is the measured
2P ceiling goes first; gates W4/W5 are decided by runs 3-5.

## 6. Final test round (cycle 12, once, after integration)

Race 1P and split screen 2P, 2x, VSYNC 240, speed limit off, idle PC, no builds, boost recorded.
- 1P: c11 default vs c12 default, 3 runs each alternating; 2P: same, 2 runs each (if calibrated).
- ALT_RANDOM runs: C12_COMMIT_REPEAT=a, C12_POSADJ=a (cheap to alternate); CAPTURE_REPEAT and
  the VRAM pieces compared as whole runs.
- Checks (one run each mode): C12_POSADJ_CHECK, C12_COMMIT_REPEAT_CHECK, C12_CAPTURE_REPEAT_CHECK,
  C11_LISTRANGE_CHECK all 0; MT_VERIFY, MT_WATCHDOG, chk1 suite; screenshots (HUD, minimap,
  water, both 2P views); host-resident 0.
- Report per thread (front own / drains, sequencer draws, committer idle, GPU ms/frame) next to
  §1 so the ceiling move is visible, not only the fps.
