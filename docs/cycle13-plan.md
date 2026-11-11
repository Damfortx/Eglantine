# Cycle 13 plan: VRAM first, then the front's list waits and the 2P back end (mkw-c12s)

Goal (user): 2-player split screen at a locked 60 fps, 2x resolution, RTX 3070 Ti 8 GB, Ryzen 7
5800X, CPU boost off. Base: mkw-c12s (C12 = f9c05ce951 + 2e6cc3a780 results), config B
(`EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 EDEN_NX2_ASTC_BC7=1 EDEN_NX2_ASTC_DISKCACHE=1`). Method as
cycles 6-12: one agent per workstream, disjoint files/functions, new code header-only under
`src/video_core/mt/` (c13_*.h), no CMakeLists.txt, `src/video_core/nx2.h` read-only, the coordinator
builds, integrates and commits, ONE final test round.

**This plan uses existing data only** (no emulator run, no build): docs/cycle12-results.md and the
cycle 12 logs `tools/runs/*-{c12-*,ref-*,ss-*}` plus `calib-c12-*`, `calib-ss-c12-1`, the c11b logs
for the no-ratchet baseline, and a code reading of the merged base. Numbers marked "est." are
projections, not measurements.

## 1. What the cycle 12 data says

### 1.1 1P (C12 41.9 fps, REF 39.2; c11b 48.6 in a session with ~500 MiB less desktop VRAM)

- **The out-of-memory ratchet is a bug, not real memory pressure.** In the race window the heap is
  at 5.67-5.82 GiB of a 6.45 GiB budget (C12) and 6.0 GiB (REF), and "device full" is 0 in every
  race window. All "Out of device memory: keeping N MiB more free" steps happen during the loads
  (40-55 s, c12-3 also at 88 s), when vkAllocateMemory runs ~4 GiB / 10 s (3 GiB dedicated) and a
  few small images fail. The code (texture_cache.h TickFrame) adds 512 MiB **per frame** in which
  `MemoryPressureEvents()` changed, so one burst of failures spread over 3 frames is +1536 MiB:
  c12-3 at 88.303 / 88.337 / 88.371 s went 1280 -> 1792 -> 2304 -> 2816 MiB after 8 host
  placements totalling ~7 MiB. Decay is 128 MiB per 600 quiet frames: 2816 MiB needs ~13 000
  frames (~5 min), i.e. the whole race.
- With 2.3-2.8 GiB of margin, critical = 3225 MiB while the non-evictable part alone (2x render
  targets 1818 MiB + RT 1x ~100 MiB + buffers/staging/other 1.6-2.0 GiB) is ~3.6-3.9 GiB: the
  collector chases a target it can never reach. Result: over critical on every frame, emergency
  pass every frame (350-413 "keeping hot images" / 10 s), 137-243 evictions/s, ASTC re-created
  58-100/s (each a disk-cache read or a decode on the committer).
- c12-1/2 (ratchet to 1024 MiB, decayed by ~150 s) ran 42.8/45.5; c12-3 (2816) 37.5. REF ratcheted
  only to 512 MiB. **C12 adds no device allocation** (diff 54ce6fbeaa..f9c05ce951: no
  vkAllocate/CreateBuffer/CreateImage/staging; the memos live in host RAM). C12 ratchets further
  because the bump counts frames: a faster build sees more frames inside one failure burst.
- VRAM composition (C11 VRAM categories, race): 2x RTs 1818 MiB in 32 images (constant), ASTC
  BC7 ~0.5-0.65 GiB + BC1-3 ~0.35-0.47 GiB, other sampled ~0.6 GiB, RT 1x ~0.1 GiB, rescale
  originals 13 MiB, buffers/staging/other 1.6-2.0 GiB of which the buffer cache is ~0.48-0.5 GiB
  (so ~1.1-1.5 GiB is staging/stream/other and is not broken down anywhere).
- CPU (top, 1P): front 61-64 % (REF 72-74 %), GPUBack 70-74 %, GPUCommit 74-76 %, committer idle
  220-262 ms/s. The front's largest wait: **list writer waits on compute 406-504 /s, 173-206
  ms/s**. Per program (C12 front census, c12-2): program 0x0 ~84 % of the waits but ~0.1 ms each
  (~20 % of the time); program 0x114cc6d700 ~9 % of the waits at **~1.9 ms each (~44 % of the
  time)**; 0x1157e5xxxx/0x1157e8f400 ~1.9-2.5 ms each. The addresses move between runs
  (0x114c86d700, 0x114d06d700, 0x114ce6d700): they are heap addresses, the key must be the shader
  hash. LISTWAIT_CHECK ("same/changed") and the LISTRANGE gate were **never run** (0 in every line).

### 1.2 2P (C12 29.7, REF 31.1, n=1 each)

- 97 k draws/s = ~3260 draws/frame (1.83x 1P). Sequencer draws 495 ms/s = 5.1 us/draw; committer
  busy ~772 ms/s = ~8.0 us/draw; GPUBack 80 %, GPUCommit 80 %, front 74 % with list writer waits
  646 /s, 287 ms/s. GPU util 49-53 %: not the GPU.
- **Render targets stuck in host memory for the whole race**: C12 4 images / 360 MiB, REF 2 / 180
  MiB, placed at the load (43-52 s) and never moved back ("relocated 0" in every line) although the
  heap sits at 4.5-4.8 GiB of 6.45 GiB (1.6 GiB free). The sizes are exact: 147456 KiB = 6144^2 x 4
  B and 36864 KiB = 3072^2 x 4 B, i.e. a 3072^2 32-bit target (shadow-map shaped) at 2x plus its
  1x original; C12 has two of those pairs, REF one. Why no relocation: RelocateHostImages only
  runs when `total_used_memory < critical` (never true: ratchet 1024-2048 MiB in 2P) and only
  picks images not used in the last 2 frames (`ForEachItemBelow(frame_tick - 2)`), so a target
  used every frame is never picked by design. The C12 vs REF 2P gap (-4.5 %) lines up with 2x the
  host-resident bytes (n=1, weak).
- Memos lose their hit rate in 2P (both views alternate through the same pipelines):
  W3 capture hits 44.1 % (1P 53.6 %), with "throttled" 35 % of captures (1P 24 %); W2 buffer memo
  taken 3.6 % (1P 15.7 %), refused "binding" 83 % of tries (1P 79 %); ubind translations reused
  8.0 % (1P 10.7 %). Each memo is one entry per pipeline slot with a miss-streak throttle
  (vk_mt_seq.h C12MissStreak/C12RecordEvery): A/B view alternation is the worst case for it.
- Posadj never fires in 2P (0 reads). Query commands 17-27 k/s (1P ~10 k/s).
- What 2P at 60 needs: ~196 k draws/s -> sequencer <= ~4.2 us/draw and committer <= ~4.3 us/draw
  at ~85 % busy (today 5.1 and 8.0), front list waits under ~100 ms/s. **No single cycle reaches
  that with boost off**; cycle 13 targets the memory pathology (which costs both modes now) and
  the measurable per-draw waste, and gathers the 2P per-function profile that decides a second
  committer (§2 W4c).

### 1.3 The MT_VERIFY regression (C12 and REF, not cycle 12)

VERIFY reports ~51-60 k mismatches/s of ~105-116 k checks/s, every one "uncaptured syncs 1 (first
0x6f6362b00 / 0x159f70400 +65536)"; c10/c11 had 0-225 per run. Code reading points at upstream's
native uniform bank re-upload, merged in sync-nx2 (buffer_cache.h BindHostGraphicsUniformBuffer,
`EDEN_NX2_BANK_REUPLOAD`, default '1'):

- It runs on the **committer** (under MT the bind runs while recording a packet) and calls
  `memory_tracker.MarkRegionAsCpuModified(device_addr, 64 KiB)` once per frame per bank.
- Under MT the committer's `SynchronizeBuffer` returns at once (`MT::t_capture`: the sequencer
  captured the bytes), so the committer never uploads that mark, and the sequencer only captures
  uploads for guest writes it was told about (MtApplyUploads / NoteGuestWrite; this mark is not
  one, and it bumps no write generation). The two banks stay CPU-modified, and VERIFY's
  `IsRegionCpuModified` check in SynchronizeBuffer counts an uncaptured sync on every later draw
  that syncs them. The size is exactly the 64 KiB bank and there are exactly two addresses.
- So under MT the feature does nothing useful for MKW (the re-upload it wants never happens), and
  it costs: every native UBO bind takes `MT::TrackerScope` and walks `IsRegionGpuModified` over
  the bound range **before** the cheap per-frame set check. 330-370 k binds/s in 1P and ~480-620
  k/s in 2P (C12 "ubind" counters) pay that on the committer (est. 20-60 ms/s, 3-8 % of its busy
  time), and the tracker lock is the one the guest cores take on writes.
- Secondary suspect, if the fix below does not bring VERIFY back to 0-225: upstream fe2cd8be2b's
  `ClaimCpuWrites`, which runs first in RasterizerVulkan::OnCacheInvalidations but not in the MT
  sequencer path SeqCacheInvalidations (sync-nx2-notes.md).

VERIFY is the alarm that guards every MT memo; with 50 % of draws flagged it guards nothing. This
fix comes before any new memo goes default-on.

## 2. Ranked workstreams

Switch rules (as cycle 12): `EDEN_NX2_C13_<X>`; default on under MT lite when expected to help;
`=0` = the cycle 12 path; `=a` = ALT_RANDOM per frame/draw where the state allows it;
`EDEN_NX2_C13_<X>_CHECK=1` = also run the old path / a re-read and compare (counter +
rate-limited ERROR "C13 <piece> check"); `EDEN_NX2_C13_STATS=1` = one "C13 ... (10 s)" line per
workstream (also on with any C13 CHECK).

### W1 VRAM: real pressure signal, hot relocation, render-target diet (1P and 2P; rank 1)

Expected: 1P back to the c11b no-ratchet regime (evictions < 60/s, ASTC re-created < 10/s, over
critical < 30 % of frames): **+8-15 % 1P** (41.9 -> ~46-48 in this session's conditions; c12-2 at
1024 MiB did 45.5, c12-3 at 2816 did 37.5). 2P: host-resident 0 -> **+3-8 % 2P** (est.), more with
the RT diet.

- **V1 pressure margin from the measured deficit (EDEN_NX2_C13_OOM, CHECK).** Replace the
  per-frame +512 MiB step with one adjustment per failure *episode*: the allocator records, at each
  device-full failure, the heap usage and budget (vmaGetHeapBudgets) and the failed size; the
  episode closes after 60 frames without a failure; margin = max(0, usage_at_failure + failed
  bytes - budget + 256 MiB), max over the episode, capped by C11 PressureCap. Episodes during a
  load (allocation rate > 1 GiB/s or frame time > 100 ms in the episode) only hold the margin
  until 300 frames after the load, then drop it to 0 in one step. Decay otherwise 256 MiB per 120
  frames while heap usage + margin < budget. Floor: critical never below the measured
  non-evictable set (2x RTs + RT 1x + non-texture VMA bytes) + 512 MiB, so the collector never
  chases an impossible target. CHECK: log both the old and the new margin per episode ("C13 oom
  episode: failures N, bytes, usage/budget at failure, old margin -> new margin"), and count
  frames where the old rule would have been over critical but the new is not, together with any
  device-full in the 300 frames after (must be 0 in the race).
- **V2 hot relocation by real room (EDEN_NX2_C13_RELOC, CHECK).** When heap usage + image bytes
  + 512 MiB < budget (not the texture cache's critical), relocate host-resident images including
  frame-hot ones, largest first, one per frame. Render targets (GpuModified, not downloadable to
  guest) move by a GPU copy into a new device-local image (same ImageId, swap the backing:
  vk_texture_cache.cpp Image::C13Rebind), not by delete + re-upload; everything else keeps the
  cycle 10 delete path. CHECK: after a rebind, copy back and compare the first frame's contents
  (sampled 64x64 tiles), count mismatches. Expect: "host memory now 0 images" within ~1 s of the
  race start in 2P.
- **V3 render-target diet at 2x (EDEN_NX2_C13_RT_DIET, default OFF: changes image quality;
  census on with C13_STATS).** Census line: the 32 2x RTs by extent, format, bytes, uses/frame and
  whether they are ever presented/blitted to the swapchain. Diet rule: square, non-screen-sized
  (extent not a multiple of the guest's render size), depth or single-channel 32-bit targets that
  are only sampled (shadow-map shape: the 3072^2 x 4 B images seen in 2P) stay 1x (ImageCanRescale
  refuses them; their framebuffer has no other attachment). Saves 108 MiB per such map (6144^2 ->
  3072^2): est. 0.1-0.2 GiB in 1P, >= 0.2 GiB in 2P (two maps, the ones that went to host memory).
  Screenshots decide (shadow edges at 1x = native look). 
- **V4 staging/other census + trim (EDEN_NX2_C13_STAGING_TRIM, CHECK = census only).** Break the
  1.1-1.5 GiB of non-buffer-cache, non-image VMA bytes down (staging pool by usage/age, stream
  buffer, upload rings, ASTC decode scratch, query/descriptor pools). Trim: staging buffers unused
  for 300 frames are freed, and the pool is capped at 256 MiB device-local outside loads. Est.
  0.2-0.5 GiB; only the census is certain.

### W2 Bank re-upload under MT / VERIFY fix (correctness tool + committer cost; rank 2)

Expected: VERIFY back to 0-225 per run; committer -3-8 % busy (est.; 1P +1-3 %, 2P +2-5 % since
the committer is ~80 % there).

- **B1 (EDEN_NX2_C13_BANK, default '1' = new, '0' = the merged path, CHECK).** Reorder: per-frame
  set test and 64 KiB size test first, TrackerScope + IsRegionGpuModified only for a bank not yet
  seen this frame. Under MT the committer never marks: the sequencer, in its uniform capture
  (where it already reads the bank's bytes for MT_LITE_UBO), takes the bank once per frame as a
  captured upload (the same path as a guest write: NoteGuestWrite + BumpWriteGeneration, so the W3
  capture memo sees it), and the committer applies it like any captured upload. Mode `s`: skip the
  re-upload entirely under MT (the c11 behaviour MKW ran with until the merge; Ocarina's white
  flashes are what the feature was for). CHECK: VERIFY's uncaptured syncs by address (top 4) in
  the C13 line, plus the per-frame count of banks re-uploaded and bytes.
- If B1 leaves VERIFY high, B2: the MT sequencer's SeqCacheInvalidations claims CPU writes first
  (`ClaimCpuWrites`), as the single-thread OnCacheInvalidations does since the merge (switch
  EDEN_NX2_C13_CLAIM, CHECK = count of ranges claimed). Only coded if the B1 census shows
  uncaptured syncs left at other addresses/sizes.

### W3 Front list writer waits (1P front and 2P front; rank 3)

Expected: front wait 173-287 ms/s -> est. -30-50 %: 1P +3-6 % where the front is the critical
path, 2P +3-5 %. The LISTWAIT/LISTRANGE pieces need data first (both never ran).

- **L0 data, no code:** the CHECK runs carry `EDEN_NX2_C12_LISTWAIT_CHECK=1` (does the list content
  change under the wait? "same" > 90 % opens LISTWAIT, already built) and `EDEN_NX2_C12_LISTRANGE=1`
  (the gate decides in-run; "shorter" > 30 % keeps it).
- **L1 writer census by shader hash (EDEN_NX2_C13_STATS):** key the per-program census by the
  compute shader hash (stable across runs) and add per program: methods and microseconds between
  the writer's send and the list fetch that waits on it, the back's backlog (ring entries) at the
  wait, and how much of the wait was the sequencer (WaitConsumed) vs the committer
  (TimedWaitSettled). Tells whether the 1.9 ms waits are backlog (L2 helps) or GPU work.
- **L2 writer early kick (EDEN_NX2_C13_KICK, `a` per wait, CHECK).** Programs learned as list
  writers (hash seen in >= 16 waits) get a kick when the front sends their dispatch: the front
  publishes at once (no batching), sets `kick_target` = the dispatch's ring position, and the
  sequencer and the committer treat it like a wait target already pending (the committer ends its
  lock run at the target, flushes; the sequencer skips C12 record work for draws before it). By the
  time the front reaches the list fetch the back has settled, so the wait overlaps the methods in
  between instead of following them. CHECK: count waits that still found the back behind the
  kicked target and the ms; no content risk (the wait itself is unchanged).
- **L3 LISTWAIT on (existing code) only if L0 shows 0 "would differ" over the whole race**; this
  default flip is the results commit's decision, not a code change.

### W4 2P back end: memos that survive view alternation, and the second-committer gate (rank 4)

Expected: capture hits 44 -> ~55 %, buffer memo taken 3.6 -> ~12-15 % in 2P: sequencer and
committer -5-10 % per draw in 2P (est.), +4-8 % 2P fps; ~0 in 1P.

- **M1 two-way capture memo (EDEN_NX2_C13_MEMO_WAYS, default 2, `1` = cycle 12, `4` allowed,
  CHECK = the existing C12_CAPTURE_REPEAT_CHECK).** Each pipeline slot keeps 2 (or 4) entries
  keyed by the register-side key + the first UBO binding address (the per-view camera buffer);
  LRU replacement; the miss-streak throttle counts per way, not per slot. Same validation (pages,
  bytes), same packet flag.
- **M2 two-way W2 buffer memo (same switch, CHECK = C12_COMMIT_REPEAT_CHECK)** for the
  C12UpdateGraphicsBuffers memo: the "binding" refusals (83 % in 2P) are the alternation case.
- **M3 ubind translation cache 2-way** (same switch).
- **W4c second committer: data gate only this cycle.** The data does not support coding it yet:
  the committer still idles 228 ms/s in 2P, its lock runs end by "other entry" 231 k / 10 s and
  "idle" 140 k / 10 s (it is coupled to the sequencer and the ring, not saturated), and there is no
  per-function committer profile in 2P (cycle 10's perf was 1P and diffuse: SpinRecursiveMutex
  5.3 %, DoUpdateGraphicsBuffers 4.8 %, GpuToCpuAddress 4.1 %, PrepareImage 3.1 %). The round's
  2P counters run takes perfthreads on GPUCommit and GPUBack after the window. Gate for cycle 14:
  if > 40 % of committer self time is in thread-safe-able encoding (descriptor build, push
  constants, replay records) rather than cache resolution under the buffer/texture locks, split
  the committer into resolve (serial) + encode (parallel per render pass); otherwise split the
  sequencer's capture per view instead.

### Not chosen (reason)

PRESSURE_CAP default 1024 (a cap on a broken signal; V1 replaces it), POSADJ work in 2P (0 reads),
FRONT2 parse thread (barrier density, round 6), thread pinning, GPU-side work (GPU 49-53 % busy),
boost (the user's choice; recorded per run).

## 3. File ownership (disjoint; shared files split by function)

| agent | owns | new header |
|---|---|---|
| A: W1 VRAM | texture_cache.h TickFrame / RelocateHostImages / C11VramTick / ImageCanRescale / ScaleUp, texture_cache_base.h (budget/reloc members), renderer_vulkan/vk_texture_cache.cpp/.h, vulkan_common/vulkan_memory_allocator.cpp/.h, renderer_vulkan/vk_staging_buffer_pool.cpp/.h, mt/c10_vram.h, c11_vram.h, lite8_vram.h, lite9_vram.h | mt/c13_vram.h |
| B: W2 bank/VERIFY | buffer_cache.h BindHostGraphicsUniformBuffer (bank block), TickFrame, SynchronizeBuffer (VERIFY census), buffer_cache_base.h (bank members), buffer_cache_mt.h, buffer_cache/memory_tracker*.h; the sequencer's uniform capture hook arrives as exact lines in docs/cycle13-hooks-B.md (applied by the coordinator in D's file) | mt/c13_bank.h |
| C: W3 front | gpu_split.h/.cpp, dma_pusher.cpp/.h, engines/kepler_compute.cpp/.h (front QMD mirror, census), engines/maxwell_3d.cpp front functions, engines/puller.cpp/.h, mt/c10_front.h, c11_front.h, c12_front.h; the sequencer/committer kick response as hook lines in docs/cycle13-hooks-C.md | mt/c13_front.h |
| D: W4 memos | renderer_vulkan/vk_mt_seq.h, vk_mt_pipeline.h (capture and commit functions; applies B's and C's hooks there as the coordinator directs), mt/c12_capture.h, c12_commit.h, c11_commit.h, draw_packet.h, packet_compact.h, vk_graphics_pipeline.cpp/.h, buffer_cache.h **C12UpdateGraphicsBuffers only**, texture_cache.h TryRepeatGraphicsBindings / TryReuseGraphicsBindings only | mt/c13_memo.h |
| coordinator | this doc, hooks application, build, integration, commits, docs/cycle13-results.md, the test round | - |

buffer_cache.h is split by function (B: bank, TickFrame, SynchronizeBuffer; D:
C12UpdateGraphicsBuffers). texture_cache.h likewise (A: budget/GC/reloc/rescale; D: the repeat
functions). No agent edits another's function; cross-file needs go to the hooks docs.

## 4. Risks

- V1: a margin that is too small lets the driver page textures (the cycle 10 9 fps mode). The
  floor rule and the episode max keep at least the measured deficit; CHECK logs every episode and
  any device-full after it; host-resident must stay 0 in 1P.
- V2: rebinding an image's backing behind live views/descriptors. The rebind must go through the
  same path as a rescale (views re-created, descriptor epochs bumped) and only between frames;
  CHECK compares contents. Fallback `=0`.
- V3: visible shadow change; off by default, screenshots only. A wrong match (a screen-sized square
  target, e.g. a 2160^2 view in some mode) would lower that target's resolution: the census line
  lists every refused image so a wrong one is visible.
- B1: moving the bank re-upload to the sequencer changes when the bank's host copy refreshes (one
  frame later at most, same as mode '1' single-thread). Mode `s` drops it under MT: fine for MKW,
  maybe not for Ocarina (white flashes): keep `1` the default.
- L2: kicks break the committer's lock runs more often (more lock pairs); `a` A/B and LOCK stats
  show it. No correctness risk (waits unchanged).
- M1/M2: more memo state per slot = more memory traffic on the sequencer; a wrong way choice is a
  miss, not a wrong draw (validation unchanged). CHECK 0 required, VERIFY must be clean first (B1).
- Session noise: the desktop held 1245 MiB of VRAM in cycle 12 vs ~740 in c11b. Record
  `nvidia-smi --query-compute-apps` and the desktop total at each run start; compare arms only
  within the round.

## 5. Final test round (once, after integration; <= 10 runs)

Common: `tools/race` (1P, window 5400..7200) and `tools/splitscreen` (2P, presents 6000 -> 10200),
`NOPROF=1 EDEN_NX2_VSYNC_HZ=240`, config B + `EDEN_NX2_MT_STATS=1 EDEN_NX2_VRAM_STATS=1
EDEN_NX2_TC_CHURN=1 EDEN_NX2_C11_STATS=1 EDEN_NX2_C12_STATS=1 EDEN_NX2_C13_STATS=1`, boost and
desktop VRAM recorded, one emulator at a time, round lock, no builds, nvidia-smi and top -H in the
window. C12 = the installed mt binary (= Eglantine-c12s pgo); C13 = the new pgo build.

| # | tag | what | decides |
|---|---|---|---|
| 1 | c13-chk | C13 1P, all C13 CHECKs + C12 CHECKs + MT_VERIFY + WATCHDOG + C12_LISTWAIT_CHECK + C12_LISTRANGE=1, shots every 10 s (also the warm-up) | VERIFY <= 225 (B1), every check 0, LISTWAIT "would differ", LISTRANGE gate |
| 2 | c13-ss-chk | C13 2P, all CHECKs except VERIFY, shots every 5 s | relocation (host 0), M1/M2 checks 0 in 2P, both views correct |
| 3 | c13-1 | C13 1P default | speed |
| 4 | c12-1 | C12 1P default | speed |
| 5 | c13-2 | C13 1P default | speed |
| 6 | c12-2 | C12 1P default | speed |
| 7 | c13-ss-1 | C13 2P default + perfthreads GPUCommit / GPUBack (10 s each) after the window | 2P speed, W4c gate |
| 8 | c12-ss-1 | C12 2P default | 2P speed |
| 9 | c13-ss-diet | C13 2P + C13_RT_DIET=1, shots every 5 s | VRAM saved, shadow quality, 2P speed vs run 7 |
| 10 | c13-3 | C13 1P + C13_KICK=a + C13_MEMO_WAYS=a (ALT_RANDOM halves) | L2 and M1 per-frame A/B |

Abort any run under 10 fps for > 60 s ("aborted <10 fps"). Pass criteria: all checks 0; VERIFY
back to the pre-merge range; 1P "frames over critical" < 30 % and no OOM step outside the loads;
2P host-resident 0 MiB in the window; screenshots (HUD, minimap, water, both 2P views, shadows
in run 9). Report per thread (front list waits ms/s, sequencer draws ms/s, committer idle, memo
hit rates, evictions/s, heap usage vs budget) next to §1 so the moves are visible, not only fps.
Install C13 only if correct and >= C12 in both modes.
