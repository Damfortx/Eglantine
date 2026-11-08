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
