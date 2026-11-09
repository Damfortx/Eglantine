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

## Steps 3-6 (commits a71c376..a37157d on mkw-mt): one test round
Commits: a71c376 RBUF (+ epoch guard for the trap-path slot reads), 0a7f905 COMPACT, d9cec9a
FLUSHCMD, 10a4d2d REPLAY (+ t9/t10 selftests), a37157d RREFRESH. Nothing below was run in the
emulator yet (compile + standalone TSan/ASan only).
BIN=/home/appo/src/Eglantine-mt/build/pgo/bin/eden-cli, from /home/appo/src/eglantine-buildenv/tools
(`./demo.sh $BIN 120 <tag>`, results in runs/cli-<tag>.out and runs/log-<tag>.txt). Every switch
below is inert with EDEN_NX2_MT unset and ON by default under EDEN_NX2_MT=1:
| switch | step | what =0 restores |
|---|---|---|
| EDEN_NX2_MT_RBUF | 3 | the committer runs ConfigureImpl's buffer half (texture half still resolved) |
| EDEN_NX2_MT_COMPACT | 4 | the v2 packet layout (full TIC/TSC entries and uniform bytes, fixed header) |
| EDEN_NX2_MT_FLUSHCMD | 5 | every FlushRegion/InvalidateRegion on the sequencer is a Serial |
| EDEN_NX2_MT_REPLAY | 6 | one scheduler closure per record (=2: replay plus the worker-side segment order check) |
| EDEN_NX2_MT_RREFRESH | extra | CPU-modified images make the resolve slow (view-not-clean) |
Older ones still worth flipping when bisecting: EDEN_NX2_MT_RCOMMIT, EDEN_NX2_MT_COMPUTE=serial,
EDEN_NX2_MT_ADAPTIVE, EDEN_NX2_MT_RESOLVERS=0.

0. Standalone (any time, already PASS at commit time): `docker run --rm --security-opt seccomp=unconfined -u 1000:1000 -v /home/appo/src:/home/appo/src -w /home/appo/src/Eglantine-mt eglantine-build:pgo bash src/video_core/mt/selftest_standalone.sh all`
   -> RESULT: ALL PASS, sanitizer_reports=0 for tsan and asan; new: t8_uniform_ring (resolver ring
   lifetime vs a simulated GPU), t9_packet_replica (compact TIC/TSC + uniform replicas, skipping
   reader), t10_replay_arena (replay blocks, elision, worker order).
1. Baseline, MT unset: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 ./demo.sh $BIN 120 s6-off` (expect ~47.9 fps; nothing new runs).
2. VERIFY, everything on: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_VERIFY=1 EDEN_NX2_MT_STATS=1 ./demo.sh $BIN 120 s6-verify`;
   `grep "MT verify\|MT stats\|MT draw packet\|MT resolved buffers\|MT replay\|MT stages" runs/cli-s6-verify.out`.
   Expect 0 mismatches (compact packets: the decoded overlay and the kept uniform bytes are what
   VERIFY compares with guest memory), "MT draw packet B" total ~1 KB (header/overlay/ubo bytes
   split), "order errors 0" is only checked with REPLAY=2 (item 6).
3. Shadow (resolver payload incl. the buffer half vs the old path): `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_SHADOW=1 EDEN_NX2_MT_STATS=1 ./demo.sh $BIN 120 s6-shadow`;
   `grep "MT shadow\|MT resolvers\|MT resolved buffers" runs/cli-s6-shadow.out`: 0 "MT shadow:" lines
   (new fields: 12 payload entry/count, 13 streamed uniform size).
4. Free run, defaults, with stats: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_STATS=1 ./demo.sh $BIN 120 s6-free` + portal screenshot.
   Look at: "MT resolved buffers/s" (committed ~= resolver ok; reasons left to the committer;
   ring MB/s and reuse), "MT resolvers/s" (view-not-clean should drop with RREFRESH; commit wait
   ms), "MT stages ... flush: skipped/commands/serial" and the serial list in "MT stats" (flush and
   invalidate Serials should mostly go), "MT replay /s" (draws/segment, elided records),
   "MT draw packet B" and "B avg".
5. The fps number: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 ./demo.sh $BIN 120 s6-fps` vs step 1 and the earlier 42.8 (RESOLVERS=2 + RCOMMIT).
6. Replay order check: 4 with `EDEN_NX2_MT_REPLAY=2` (tag s6-replay2): "order errors 0" in "MT replay /s".
7. Bisect (only if 2-6 show a problem or a loss): 4 with one switch at 0 per run, tags
   s6-rbuf0 (EDEN_NX2_MT_RBUF=0), s6-compact0, s6-flushcmd0, s6-replay0, s6-rrefresh0; picture
   problems first with REPLAY=0 and RBUF=0 (they change what is recorded), hangs with FLUSHCMD=0
   (it changes what the sequencer waits for).
8. Fault injection, everything on: 4 with `EDEN_NX2_MT_RCHECK=2` (tag s6-rcheck2): picture identical,
   no "resolved slot" lines; RCHECK=1 also reports "uniform binding(s) held another buffer" (should be 0).
Known risks to watch in this round: RBUF replaces ConfigureImpl's buffer half on the fast path
(a missed side effect shows as wrong geometry/uniforms: compare with s6-rbuf0); COMPACT relies on
every reader applying every draw packet's inserts/uniform patches in stream order (a divergence
shows as VERIFY "overlay misses"/"uniform buffer changed" or SHADOW mismatches: compare with
s6-compact0); FLUSHCMD's texture skip relies on texture page bits plus "no draw/compute in flight"
(stale CPU reads of render targets would show as wrong pictures in readbacks: s6-flushcmd0);
REPLAY elides repeated state inside a segment (state bugs: s6-replay0); RREFRESH uploads CPU-
modified images at commit (stale textures: s6-rrefresh0).

## Throughput pieces (branch mkw-mt-perf, docs/mt-perf-plan.md): one test round
Switches (src/video_core/mt/perf_switches.h), all inert with EDEN_NX2_MT unset and ON by default
under EDEN_NX2_MT=1; `=0` restores the previous behaviour ("alt" = also accepts `a` for the
EDEN_NX2_ALT_SECONDS A/B windows):
| switch | piece | =0 restores |
|---|---|---|
| EDEN_NX2_MT_LAZY_WAKE | P1 | every SpmcStream::Commit checks for sleeping consumers and futex-wakes them |
| EDEN_NX2_MT_UBIND_FOLD | P2 | one "uniform" Command per constant buffer bind |
| EDEN_NX2_MT_CMD_IN_BATCH (alt) | P3 | query/fence/barrier/clear/cond-render Commands end the open batch |
| EDEN_NX2_MT_BATCH_MIN=N (default 4) | P4 | =1: the adaptive close fires after any draw |
| EDEN_NX2_MT_CONT_MEMO (alt) | P5 | IsContinuousRange per storage buffer per draw |
| EDEN_NX2_MT_INVAL_TEXFILTER (alt) | P6 | texture mutex for every sequencer FlushCaching with ranges |
| EDEN_NX2_MT_INVAL_ONELOCK (alt) | P7 | tracker lock twice per invalidated range |
| EDEN_NX2_MT_EBR_LAZY (alt) | P8 | epoch collection takes both cache mutexes every batch / 16 draws |
| EDEN_NX2_MT_RALIAS (alt) | P9 | views of aliased images are view-not-clean (committer slow path) |
| EDEN_NX2_MT_DMA_CONSUMED (alt) | P10 | MME DMA reads (WaitForWriters, the non-GPU-written Drain) wait for `settled` of everything sent |
BIN=<mkw-mt build>/build/pgo/bin/eden-cli, from /home/appo/src/eglantine-buildenv/tools.
0. Standalone (no emulator): `docker run --rm --security-opt seccomp=unconfined -u 1000:1000 -v /home/appo/src:/home/appo/src -w <tree> eglantine-build:pgo bash src/video_core/mt/selftest_standalone.sh all` -> ALL PASS, sanitizer_reports=0 (new t11_lazy_wake).
1. fps, defaults: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_STATS=1 ./demo.sh $BIN 120 perf-free` + portal screenshot. Compare with the 35 fps baseline and MT unset (47.9-51.1).
   With EDEN_NX2_GPU_PROFILE=1 once (costs fps): "GPU split drains per second" dma read / writers (dma read) wait ms should drop sharply (P10).
   In the stats: "MT stats/s ... commands: uniform" ~0 (P2); "MT resolvers/s ... N per batch" >= 4 and "batches" well under draws/2 (P3/P4); "MT stages ms/s: sequencer draws" per draw packet (target <= ~5 us); "view-not-clean N (aliases A, buffer B, cpu C, sparse D, dead E, rescaled F, cpu+alias G)" tells what still takes the committer's slow path; committer idle.
2. Correctness, all on: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_VERIFY=1 EDEN_NX2_MT_STATS=1 ./demo.sh $BIN 120 perf-verify` (0 new mismatches) and `EDEN_NX2_MT_SHADOW=1` (tag perf-shadow; 0 "MT shadow:" lines; with RALIAS the alias views are resolved, so shadow compares them too).
3. Hangs: 1 with `EDEN_NX2_MT_WATCHDOG=1` must log no "MT wait > 2 s"; if anything hangs, first `EDEN_NX2_MT_LAZY_WAKE=0` (a missed flush point leaves a consumer asleep with entries pending; the 32 KiB auto-flush bounds it, so a hang means a wait without Flush).
4. Bisect only if 1-3 show a loss or a problem, one switch at 0 per run (tags perf-lazy0, perf-fold0, perf-cmdb0, perf-bmin1 (EDEN_NX2_MT_BATCH_MIN=1), perf-cont0, perf-texf0, perf-onelock0, perf-ebr0, perf-ralias0, perf-dmac0). Wrong uniforms/geometry: UBIND_FOLD first; stale or flickering textures on aliased render targets: RALIAS; missed texture updates after CPU writes: INVAL_TEXFILTER; wrong indirect draw counts / culling after MME DMA reads: DMA_CONSUMED (also run 1 once with EDEN_NX2_DMA_READ_CHECK=1: "0 stale" expected).
5. A/B of the "alt" ones in one run: e.g. `EDEN_NX2_ALT_RANDOM=1 EDEN_NX2_ALT_SECONDS=2 EDEN_NX2_MT_RALIAS=a` (3 runs, tools/alt.py).

## Round 6 (branch mkw-mt-lite, commits 6171e38..5e5a574, docs/mt-lite-plan6.md): one test round
Compiled only (build/pgo rc=0); nothing below has run. BIN=/home/appo/src/Eglantine-mtB/build/pgo/bin/eden-cli,
from /home/appo/src/eglantine-buildenv/tools, 2x attract demo (`./demo.sh $BIN 120 <tag>`; results in
runs/cli-<tag>.out, runs/log-<tag>.txt). L = `EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1` (now includes
GPUPARAM2, REGDEDUP and the W3 switches REPORTSIZE/OVERLAY64/SEQPOS/I2MPOS/PENDACQ/PENDPOLL by default).
Note: REGDEDUP's commit also fixes a real bug (FrontRunMacro skipped repeated MME DMA reads 0x157/0x158
under MT lite since iteration 3, unconditionally fixed): MT lite numbers may shift because of it.
1. Startup lines: `grep "MT lite 6:\|Register dedup: .* latch-only\|GPU params v2: pipeline" runs/log-r6-stats.txt`
   from run 3: regdedup on; reportsize..pendpoll true, pendcheck false; INFO "built with draw parameter
   built-ins (cb0)". Any WARNING "reads the draw parameter words from memory" = those draws see 0
   (wrong geometry): note hash/stage.
2. fps pairs vs MT unset, alternating, >= 3 each:
   `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 ./demo.sh $BIN 120 r6-off<i>` and
   `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 ./demo.sh $BIN 120 r6-lite<i>`
   + one portal screenshot of an r6-lite run (missing/exploded track or kart geometry = wrong
   base vertex/instance from GPUPARAM2: rerun with EDEN_NX2_MT_LITE_GPUPARAM2=0).
3. Stats run: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 L EDEN_NX2_GPU_PROFILE=1 EDEN_NX2_PULLER_STATS=1 EDEN_NX2_ACQUIRE_STATS=1 EDEN_NX2_GPUPARAM_STATS=1 ./demo.sh $BIN 120 r6-stats`:
   - `grep "GPU params in 10 s"`: "v2: N calls" ~10k/s*10, copy-free ~all, "built-in draws" > 0, fallbacks ~0;
     group size histogram (expected mostly 4), "released by"/"methods" = what sits between calls.
   - "GPU split drains per second": writers (commands) ~108 -> ~0-10 ms/s; position (was ~109) down;
     puller down. `grep "MT lite 6 position waits"`: sequencer-only > 0, less ms/wait than settle.
     PULLER_STATS: "pendacq nodrain", "pendpoll lazy" > 0; ACQUIRE_STATS site 4 met ~0.
   - `grep "Register dedup:"`: dma+macro dropped/s in the millions; front zones "front method"/"macro"/"push" down.
     "writers (dma read)" may rise (0x157 fix).
4. Randomized in-run A/Bs (3 runs each, `./alt.py runs/log-<tag>.txt`):
   `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 L EDEN_NX2_ALT_RANDOM=1 EDEN_NX2_ALT_SECONDS=2 EDEN_NX2_MT_LITE_GPUPARAM2=a ./demo.sh $BIN 120 r6-ab-gp2-<i>`
   `... EDEN_NX2_MT_LITE_REGDEDUP=a ... r6-ab-dedup-<i>`
   Grouping on/off (no 'a'): separate runs with `EDEN_NX2_GPUPARAM2_GROUP=1` vs default (r6-grp1-<i>).
   W3 switches have no 'a' mode: if 2/3 show a loss, one run each with EDEN_NX2_MT_LITE_<X>=0 for
   SEQPOS, OVERLAY64, PENDACQ (tags r6-<x>0).
5. GPUPARAM_CHECK: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 L EDEN_NX2_GPUPARAM_CHECK=1 ./demo.sh $BIN 120 r6-gpcheck`:
   "check ... mismatched 0", "v2 check N compared, 0 mismatched", no "GPU params check" warnings
   (groups are size 1 in this mode). Also once `EDEN_NX2_SPLIT_VERIFY=1` (r6-splitverify): no new register differences.
6. REGDEDUP CHECK: `... L EDEN_NX2_MT_LITE_REGDEDUP_CHECK=1 EDEN_NX2_GPU_PROFILE=1 ... r6-dedupcheck`:
   "would have emitted (total)" 0. Without MT: `EDEN_NX2_MT_LITE_REGDEDUP=1 EDEN_NX2_MT_LITE_REGDEDUP_CHECK=1 EDEN_NX2_GPU_PROFILE=1` (r6-dedup-nomt): "(sink)" > 0 in a normal =1 run, 0 violations in CHECK.
7. VERIFY: `... L EDEN_NX2_MT_VERIFY=1 EDEN_NX2_MT_STATS=1 ... r6-verify`: 0 mismatches (besides classified concurrent guest writes).
8. DMA_READ_CHECK: `... L EDEN_NX2_DMA_READ_CHECK=1 ... r6-dmacheck`: "without a drain (0 stale)". If stale: bisect
   I2MPOS=0, SEQPOS=0, OVERLAY64=0, REPORTSIZE=0. PENDCHECK: `... L EDEN_NX2_MT_LITE_PENDCHECK=1 EDEN_NX2_PULLER_STATS=1 ... r6-pendcheck`: "pendacq would meet" ~0.
9. Watchdog: `... L EDEN_NX2_MT_WATCHDOG=1 ... r6-watchdog`: no "MT wait > 2 s"; no new "Channel X waits on
   semaphore" lines (else PENDACQ=0, then PENDPOLL=0). No device loss in any run.
10. Without MT: `EDEN_NX2_GPUPARAM2=1` (MT unset, r6-gp2-nomt): copy-free inline, picture correct; MT unset with
    nothing set must match r6-off (no "GPU params v2" lines, disk pipeline cache loads unchanged).
11. FRONT2 census (go/no-go for a two-stage front): `... L EDEN_NX2_FRONT2_STATS=1 EDEN_NX2_GPU_PROFILE=1 ... r6-front2`
    (not an fps run): `grep "FRONT2 census"`: close FRONT2 if dry decode < ~40 ms/s or barriers > ~5k/s.
12. Split screen (needs ONE manual calibration first, user at the PC; tools/splitscreen/README.md):
    `INPUT=record ./splitscreen/splitscreen.sh $BIN 600 cal` (drive menus: P1 a/f+h/arrows, P2 o/e+r/F5-F8, F12 at "GO"),
    `./splitscreen/rec2timeline.py splitscreen/rec/keys-rec-cal.txt 100 > splitscreen/timeline.txt`, then
    `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 ./splitscreen/splitscreen.sh $BIN 240 ss-off` and
    `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 ./splitscreen/splitscreen.sh $BIN 240 ss-lite`
    (alternate, >= 2 each): `cat runs/fps-<tag>.txt`; marker `grep "Key script: press key 69" runs/log-<tag>.txt`;
    screenshot during the race to confirm split screen.

## Batch 7 (branch mkw-mt-lite, commits 4d3eb39..a04d1ed, docs/batch7-plan.md): one test round
Compiled only (build/pgo rc=0); nothing below has run. BIN, demo.sh, L as in Round 6.
1. BC7 picture: `env NOPROF=1 EDEN_NX2_ASTC_BC7=1 ./demo.sh $BIN 120 b7-bc7` (MT unset) + portal
   screenshot vs a default run (b7-bc3): log "ASTC recompression: BC7 (GPU encoder), opaque BC1 off";
   ground no longer darkened, no block garbage / wrong alpha (foliage, kart decals). Corrupt textures
   => EDEN_NX2_ASTC_BC7=0 confirms; then try EDEN_NX2_ASTC_BC7_OPAQUE_BC1=1. Device loss / validation
   errors on BC7 copies => note the texture (BC7<->BC3 copy risk). fps vs b7-bc3 (encoder ~3x ALU):
   2 runs each, NOPROF=1 VSYNC 240. VRAM: same as BC3 except opaque textures (BC1 -> BC7 doubles them).
2. Disk cache: `env EDEN_NX2_ASTC_DISKCACHE=1 ./demo.sh $BIN 120 b7-dc1` twice (b7-dc1, b7-dc2):
   start line (dir under the shader cache /<title>/astc_bc, cap); run 1 "misses"/"written" > 0,
   run 2 "hits" >> misses, read ms small, no corrupt/write errors; picture identical to b7-bc3
   (screenshot). Also once with BC7 on (key includes the mode: the BC3 entries must not be hit).
   Check hitches: "hash X ms" per 10 s on misses. `du -sh` the dir <= cap.
3. GPUPARAM2 R1 (stats run, L + EDEN_NX2_GPU_PROFILE=1 EDEN_NX2_GPUPARAM_STATS=1, b7-gp-stats):
   `grep "GPU params in 10 s"` -> "R1: memory-word fallbacks", "LDC.64 replaced", "count draws",
   "per-draw multi waiting"; `grep "kept waiting for CPU parameters\|per-draw plan (R1)"` = macro
   44's shape (copy into the next plan). No WARNING "reads the draw parameter words" without a
   fallback count. Picture: portal screenshot; geometry wrong => EDEN_NX2_GPUPARAM2_MULTI=0, then
   _LDC64=0, then _FALLBACK=0. CHECK: `L EDEN_NX2_GPUPARAM_CHECK=1` (b7-gpcheck): "v2 check N
   compared, 0 mismatched" incl. multi calls.
4. R2 capture: CHECK run `L EDEN_NX2_MT_LITE7_CAPTURE_CHECK=1 EDEN_NX2_MT_STATS=1` (b7-capcheck):
   "MT lite 7 capture/s" check counts: desc mismatches 0, lockskip-not-held 0, overlay mismatches 0
   (ubo bytes changed after unlock may be > 0, same race as SEQLOCK). In-run A/B 3 runs:
   `L EDEN_NX2_ALT_RANDOM=1 EDEN_NX2_ALT_SECONDS=2 EDEN_NX2_MT_LITE7_LOCKSKIP=a` (b7-ab-lock-<i>),
   same for DESCDEDUP. MT_STATS "sequencer draws ms/s" vs Round 6 (432 ms/s); VERIFY run
   (`L EDEN_NX2_MT_VERIFY=1`, b7-verify): 0 mismatches. Bisect on picture problems: UBOLOCK,
   DESCDEDUP, OVERLAY, LOCKSKIP (=0 each).
5. fps pairs vs MT unset (>= 3 each, alternating) as Round 6 step 2 with tags b7-off<i> / b7-lite<i>.
