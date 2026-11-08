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
