# MT lite (approach B, branch mkw-mt-lite from mkw-mt 76df913)

Goal: beat MT unset (61-66 fps, attract demo 2x, VSYNC 240) with the lightest 2-thread back:
sequencer (engines + capture) and committer (everything else), no resolvers, minimum coordination.

## Reading of the data (docs/mt-round3-findings.md, mt-perf-*.md, code)
- K=0 already removes batches, BatchEnd, resolver wake-ups and WaitResolved (`Batching()` is
  `resolver_count != 0`, `WaitResolved` returns null). The 31 % committer wait and the batch
  machinery are a K>0 cost: the lite default is K=0.
- tracker_mutex on the committer (TrackerScope ctor 11.8 %) is mostly
  `BindHost{Graphics,Compute}UniformBuffer`: every bound uniform buffer of every draw asks
  `MtIsRegionGpuModified` (tracker lock) and then throws the answer away, because the packet's
  UboEntry (`staged`, decided by the sequencer at capture) overrides it. ~4.6 locks/draw against
  the sequencer's pass-2 hold (uploads memcpy inside) and the CPU traps: that is the contention
  both threads show (sequencer SpinRecursiveMutex::lock 10.9 %).
- Front 67 % waiting: under EDEN_NX2_MT a Drain also waits for `BackQuiet()`. `RequestIdleHook`
  only reaches the committer (`mt_back->RequestHook()`), so the sequencer finishes its whole idle
  spin (8192 pauses, ~60-120 us on Zen 3) before storing back_quiet; the committer's Idle spin
  (4096 pauses) ignores `hook_requested` too. Without MT the request ends the back's spin at once.
  So nearly every Drain under MT pays ~100+ us that MT unset does not (puller ~850 drains/s,
  ~400 of them waiting): a likely large part of the front's sched_yield 32.7 % (the BackQuiet loop
  yields after 16384 pauses) and of the ping-pong.
- Puller/sync drains wait for `settled` of everything sent. What the front reads after them
  (semaphore words, syncpoints, report words) is written by the sequencer or by committer
  Commands, which already note `committer_write_position` (P10): the same reasoning as
  EDEN_NX2_MT_DMA_CONSUMED applies, and the quiet wait is not needed under MT (the front never
  records; both threads submit from their own idle paths; MustFlushRegion is lock-safe).
- Committer per entry: a seq_cst `committer_quiet` store (xchg) and a seq_cst fence, only for a
  WaitCommitted Dekker pair that fires ~0 times a second.
- TIC validity bits (SeqTicValid memo per TIC) are read only by resolvers.
- Rejected: lazy TIC/TSC/upload reads on the committer. The sequencer writes semaphore payloads
  the guest CPU observes before the committer records earlier draws; the CPU may then rewrite
  descriptors/vertex data, so the capture of guest bytes on the sequencer stays (it is what
  makes MT correct, VERIFY 0 mismatches). Channel binds as Commands (Serial today, ~150/s, 40-75
  ms/s of sequencer wait) need per-thread channel state in the caches: out of scope.

## Pieces (all header/inline, inert with EDEN_NX2_MT unset; read once, no 'a')
EDEN_NX2_MT_LITE=1 (with EDEN_NX2_MT=1): EDEN_NX2_MT_RESOLVERS defaults to 0 and every piece
below defaults on; `EDEN_NX2_MT_LITE_<X>=0` turns one off, `=1` turns it on without LITE.
| id | switch | change | files |
|---|---|---|---|
| L0 | EDEN_NX2_MT_LITE | umbrella; resolvers default 0 | mt/mt_switches.h, mt/lite_switches.h |
| L1 | EDEN_NX2_MT_LITE_UBO | committer: uniform buffer streamed/cached decision from the packet's UboEntry, no `MtIsRegionGpuModified` (tracker lock) when the capture has the entry; VERIFY keeps the check (it compares) | buffer_cache/buffer_cache.h |
| L2 | EDEN_NX2_MT_LITE_LOOP | committer loop: `committer_quiet` stored only on change, no per-entry fence; Idle does the fence + commit_waiting wake; WaitCommitted polls (yield, then 20 us sleeps) instead of a futex only the committer could end | renderer_vulkan/vk_mt_pipeline.h |
| L3 | EDEN_NX2_MT_LITE_TICVALID | no tic_valid bits/memo in draw packets while resolver_count == 0 | vk_mt_pipeline.h (CaptureDraw) |
| L4 | EDEN_NX2_MT_LITE_DRAIN | Drain(Puller/Sync) and WaitForPosition: `consumed` past everything sent + `settled` past min(target, committer_write_position); no BackQuiet wait | gpu_split.h/.cpp |
| L5 | EDEN_NX2_MT_LITE_QUIET | RequestIdleHook under MT also sets the sequencer's hook_requested (its idle spin ends, back_quiet at once); committer Idle spin breaks on hook_requested (also helps Scheduler::WaitSubmitted callers) | gpu_split.h, vk_mt_pipeline.h |
Kept as they are (already the light shape with K=0): lazy wake (P1), UBIND_FOLD (P2), CONT_MEMO,
INVAL_TEXFILTER/ONELOCK, DMA_CONSUMED, COMPACT, REPLAY, FLUSHCMD. EBR stays on (CPU trap path
reads cache slots under an epoch guard).

## What to measure (BIN=$HOME/src/Eglantine-mtB/build/pgo/bin/eden-cli, from tools/)
Start line to check: `MT lite: true (ubo true, loop true, ticvalid true, drain true, quiet true; resolvers 0)`.
1. fps: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 ./demo.sh $BIN 120 lite-fps`
   vs MT unset (same tree, `lite-off`) and full MT (EDEN_NX2_MT=1, `lite-full`).
   Expected: above full MT (38-47) and K=0; target > MT unset. No "MT resolvers" stats lines.
2. Stats/profile: same + `EDEN_NX2_MT_STATS=1` (tag lite-stats): "committer idle" up, sequencer
   draws ms/s per packet down (less tracker spinning); once with `EDEN_NX2_GPU_PROFILE=1`
   (tag lite-prof): "GPU split drains per second" puller/sync waited ms far lower. perf of
   GPUCommit: TrackerScope ctor ~gone; GPU (front): sched_yield/Drain shares down.
3. Correctness: `EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 EDEN_NX2_MT_VERIFY=1 EDEN_NX2_MT_STATS=1`
   (tag lite-verify): 0 mismatches (L1 is off under VERIFY by design; L2-L5 are active).
   Picture: portal screenshot of lite-fps.
4. Hangs: 1 with `EDEN_NX2_MT_WATCHDOG=1`: no "MT wait > 2 s" (L2 changes WaitCommitted, L4 what
   the front waits for).
5. Bisect only on a loss/problem, one at 0 per run: EDEN_NX2_MT_LITE_UBO=0 (wrong uniforms),
   _LOOP=0 (hang/stall in WaitCommitted), _TICVALID=0, _DRAIN=0 (semaphore/acquire or fence
   problems: hangs or flicker), _QUIET=0; EDEN_NX2_MT_RESOLVERS=2 with LITE (lite fixes on the
   full design). Optional A/B with LITE: EDEN_NX2_MT_COMPACT=0, EDEN_NX2_MT_REPLAY=0.
6. For approach A's design: `EDEN_NX2_MT=1 EDEN_NX2_MT_LITE_QUIET=1 EDEN_NX2_MT_LITE_DRAIN=1
   EDEN_NX2_MT_LITE_UBO=1` (resolvers still 2) shows whether the drain/quiet/UBO fixes carry over.

## Risks
- L4: a front read after a puller drain that depends on committer work other than Commands
  (draw/compute packets writing guest memory synchronously on the committer) would be early. None
  known: GPU writes go through the buffer cache (GPU-modified at capture -> MustFlushRegion ->
  RunFlush -> FlushRegion Command, noted). Command-list writes (WritersCommands) are untouched.
- L4/L5: not waiting for quiet means submits come from the committer's own Idle (~4096 pauses
  later) instead of before the drain returns; a CPU-side fence wait would see them a little later.
- L2: a WaitCommitted (I5 texture write barriers, back-pressure) now polls; ~0/s in MKW, but a
  title with many I5 barriers would pay up to 20 us per wait.
- L5 makes both threads skip the rest of their spin after a request, so the next work after it
  may need a futex wake.
- Gains are unmeasured: the sequencer still costs about what the old back did per draw; L1/L4/L5
  attack waits and contention, not capture work.
