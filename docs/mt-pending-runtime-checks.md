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
