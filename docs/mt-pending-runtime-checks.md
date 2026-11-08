# Pending runtime checks (run with the PC idle)

From /home/appo/src/eglantine-buildenv/tools; BIN=/home/appo/src/Eglantine-mt/build/pgo/bin/eden-cli.
Results in runs/log-<tag>.txt and runs/cli-<tag>.out.

## Phase 4 (commits 05d55b8..e5ff281)
1. MT unset baseline: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 ./demo.sh $BIN 120 mt-off`
2. VERIFY: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_VERIFY=1 EDEN_NX2_MT_STATS=1 ./demo.sh $BIN 120 mt-verify`; `grep "MT verify\|MT stats" runs/cli-mt-verify.out`
   (the "uncaptured syncs" check was replaced by an exact per-range test; re-verify. "concurrent CPU writes" counted separately)
3. Free-running: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_STATS=1 ./demo.sh $BIN 120 mt-free` + portal screenshot
4. Compute packets: 2 and 3 with `EDEN_NX2_MT_COMPUTE=packet`
5. Role checks: 3 with `EDEN_NX2_MT_CHECK=1`; `grep "MT check"`
6. Debug fallback: `EDEN_NX2_MT_DRAWS=serial`

## Phase 5 (commits 438bee7..HEAD on mkw-mt)
Same BIN/demo.sh. New switches: EDEN_NX2_MT_RESOLVERS=K (default 2; 0 = phase 4),
EDEN_NX2_MT_SHADOW=1|2, EDEN_NX2_MT_RCOMMIT=1 (off by default), EDEN_NX2_MT_RCHECK=1|2,
EDEN_NX2_MT_BATCH=N (16), EDEN_NX2_MT_EBR=0|1 (on with MT). `grep "MT resolvers/s"` gives
per-resolver draws/s, batch size, busy/wait %, sampler creations, ok/committed/slow (by
reason)/missing, validation failures by reason, batches, commit wait ms, ring waits,
barriers, shadow checks/mismatches/unvalidated and EBR pending.
0. Standalone stress tests (no emulator, any time): `docker run --rm --security-opt seccomp=unconfined -u 1000:1000 -v /home/appo/src:/home/appo/src -w /home/appo/src/Eglantine-mt eglantine-build:pgo bash src/video_core/mt/selftest_standalone.sh all` -> RESULT: ALL PASS, sanitizer_reports=0 (tsan and asan).
1. Neutrality of S0-S4 (EBR deferral, packet v2, R1): `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_RESOLVERS=0 EDEN_NX2_MT_STATS=1 ./demo.sh $BIN 120 mt5-k0` vs `mt-free`: same fps, no new errors; packet B avg grows a little (v2 arrays). Also once with EDEN_NX2_MT unset (sampler variant array, ResolveInfo/ConfigureDraw split, erase_then order) vs `mt-off`.
2. In-emulator selftest: `EDEN_NX2_MT=1 EDEN_NX2_MT_SELFTEST=1 ...`; `grep "MT selftest"`: all PASS incl. p5_t1..p5_t7.
3. Shadow, lockstep (exact memory): `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_SHADOW=1 EDEN_NX2_MT_VERIFY=1 EDEN_NX2_MT_STATS=1 ./demo.sh $BIN 120 mt5-shadow-verify`; `grep "MT shadow\|MT verify\|MT resolvers" runs/cli-mt5-shadow-verify.out`. Expect 0 "MT shadow:" lines (fields: 1 view count, 2 view id, 3/4 samplers, 5-8 buffer ids by kind ubo/ssbo/vertex/index, 9 rescaling, 10/11 image descriptor entries), shadow checks > 0, slow mostly view-not-clean/view-map-miss/buffer-miss early then low; resolved ok % = ok/(ok+slow).
4. Shadow free-running: 3 without EDEN_NX2_MT_VERIFY (tag mt5-shadow). Mismatches 0; unvalidated small; "committer waited 2 s" must never appear (resolver/committer deadlock watchdog).
5. `EDEN_NX2_MT_SHADOW=2`: every "failed validation" line by reason (view = image version/generation changed between resolve and commit: GC, uploads, CPU writes; buffer = GC/joins). Buffer failures should be << 1 % of draws (R13).
6. Fault injection: 4 with `EDEN_NX2_MT_RCHECK=2 EDEN_NX2_MT_RCOMMIT=1` (1 % of results marked failed): picture identical; with RCHECK=1 no "resolved slot of draw" lines (R20).
7. Only after 3-5 are clean: `EDEN_NX2_MT=1 EDEN_NX2_MT_RCOMMIT=1 EDEN_NX2_MT_STATS=1` (tag mt5-rcommit), randomized A/B against EDEN_NX2_MT_RESOLVERS=0, single and split screen; portal screenshots compared; "committed" ~= ok rate. Then K=1/2/3 and EDEN_NX2_MT_BATCH=4/16/64 (R17/R18/R27: ring waits, commit wait ms, wait %).
8. Channel switches (R19): menus <-> race, title screen; "barriers" > 0 in stats, no channel validation failures storm, no hang.
9. 30 min split-screen soak with RCOMMIT=1: "EBR pending" stays small (< a few hundred), VRAM stable vs RESOLVERS=0 (R16).
Not implemented yet (no runtime check): resolver UBO ring/buffer half of the commit (RUBO), descriptor-buffer blocks (RDESC/S9), PIPE=resolver (S10), POD replay (5b, EDEN_NX2_MT_REPLAY is read but unused).

## Phases 6 and 7 (branch mkw-mt-seq, commits b2f6ae8..)
BIN=/home/appo/src/Eglantine-mt2/build/pgo/bin/eden-cli. New switches (all inert with EDEN_NX2_MT unset):
EDEN_NX2_MT_TRAPLOCK (on), EDEN_NX2_MT_LOCKSCOPE (on), EDEN_NX2_MT_COND (on), EDEN_NX2_MT_INDIRECT (on),
EDEN_NX2_MT_DRAWTEX (on), EDEN_NX2_MT_DMA=packet (default serial), EDEN_NX2_MT_UBO_GEN=1 (default off),
EDEN_NX2_GPU_TIMESTAMPS=1 (any MT mode).
1. MT unset baseline still identical: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 ./demo.sh $BIN 120 seq-off`; compare fps with phase 4's mt-off.
2. VERIFY with everything on: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_VERIFY=1 EDEN_NX2_MT_STATS=1 EDEN_NX2_MT_DMA=packet EDEN_NX2_MT_UBO_GEN=1 ./demo.sh $BIN 120 seq-verify`;
   `grep "MT verify\|MT stats\|MT stages" runs/cli-seq-verify.out`. Expect no new "uniform buffer ... changed" (a stale UBO_GEN reuse shows up there) and no "upload ... changed".
3. Free-running, phase 6/7 defaults: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_STATS=1 EDEN_NX2_LOCK_STATS=1 ./demo.sh $BIN 120 seq-free` + portal screenshot.
   In "MT stats" the serial list should lose most of cond-render, draw-indirect, draw-texture; in "MT stages" compare
   "sequencer draws" / "wait-committed" / "committer idle" with the phase 4 run. "Lock waits" lines now name the lock
   ([buffer], [texture], [tracker]): GPUBack should no longer wait on GPUCommit [buffer]; game threads (traps) should wait far less.
4. A/B of the lock work: 3 with `EDEN_NX2_MT_TRAPLOCK=0`, and with `EDEN_NX2_MT_LOCKSCOPE=0` (fps and lock waits).
5. A/B per Serial conversion if anything looks wrong: `EDEN_NX2_MT_COND=0`, `EDEN_NX2_MT_INDIRECT=0`, `EDEN_NX2_MT_DRAWTEX=0`.
6. DMA packets: 3 with `EDEN_NX2_MT_DMA=packet`; "dma serial N/s" in "MT stages" counts copies kept Serial (destination GPU-written);
   check the portal/race visuals and that "dma" moves from the serial list to the command list.
7. UBO generations: 3 with `EDEN_NX2_MT_UBO_GEN=1`; "ubo gen: hits/misses/unprotected" in "MT stages". Mostly "unprotected" means
   the uniform pages are not write-protected (CPU-modified in the tracker) and the switch can't help; keep it off then.
8. GPU time: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_GPU_TIMESTAMPS=1 ./demo.sh $BIN 120 gpu-ts` (MT unset) and with `EDEN_NX2_MT=1`;
   `grep "GPU timestamps" runs/cli-gpu-ts.out`: ms GPU per frame vs frame time says whether a run is GPU-bound.
9. Role checks with the new paths: 3 with `EDEN_NX2_MT_CHECK=1 EDEN_NX2_MT_DMA=packet`; `grep "MT check"`.

## Results 2026-11-08 (merged mkw-mt, BIN=build/pgo, demo 120 s, VSYNC 240)
Hangs fixed: SpmcStream::Commit published `published` with a release store before reading
`waiters` (Dekker pair with WaitForData): lost wake-up, committer asleep with entries pending
while the sequencer waited (both observed hangs). Watchdog (MT_STATS / EDEN_NX2_MT_WATCHDOG=1)
reports any MT wait > 2 s and stalled cursors, then kicks waiters: 0 reports in all runs below.
| run | fps (mean of 10 s windows) | notes |
|---|---|---|
| MT unset (m-off) | 47.9 | 20 errors = baseline (unmapped reads) |
| MT=1 K=0 VERIFY | 42.1 | 0 mismatches (1844 concurrent guest writes classified) |
| MT=1 K=2 VERIFY | 34.0 | before the upload read-after-protect fix: 113 (one buffer, explained) |
| MT=1 K=0 free | 43.1 | screenshot correct |
| MT=1 K=2 free | 43.3 | screenshot correct; batches 8-9 draws, commit wait ~400 ms/s |
| SHADOW=1 VERIFY | 31.0 | 0 shadow mismatches, 1 verify (async GPU-side write, 1 byte) |
| SHADOW=1 free / SHADOW=2 | 37.1 / 36.4 | 0 mismatches; validation failures buffer ~1/s, view rare |
| RCHECK=2 SHADOW=1 | 39.9 | 183 injected/s handled |
| RCOMMIT=1 / +RCHECK=2 | 42.8 / 41.2 | committed == ok; screenshots not taken (window covered) |
All runs: 120 s, clean "committer stopped", no exit hang.
