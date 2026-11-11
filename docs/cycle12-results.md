# Cycle 12 results (2026-11-10)

Builds (pgo, eden-cli): **REF** = mkw-sync (cycle 11 + upstream nx2 merge, Eglantine-sync),
**C12** = mkw-c12s at f9c05ce951 (mkw-sync + cycle 12 W1-W3, Eglantine-c12s). Old C11 (pre-sync)
was not run (see "Not done").

Setup: `tools/race` (assists ON, window 5400..7200 = 1800 presents, run stopped 12 s after the
2nd marker) and `tools/splitscreen` (2P), `NOPROF=1 EDEN_NX2_VSYNC_HZ=240`, config B
(`EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 EDEN_NX2_ASTC_BC7=1 EDEN_NX2_ASTC_DISKCACHE=1`) + `MT_STATS
VRAM_STATS TC_CHURN C11_STATS C12_STATS` on every run. One emulator at a time, round lock, no
builds. CPU boost **0 (off)** in all 10 runs. GPU util/W/VRAM = mean of `nvidia-smi -l 1` inside
the window; counters = means of the 10 s lines inside the window; thread % = `top -H` samples
inside the window. Logs: `tools/runs/*-{c12-*,ref-*,ss-*}`.

**Session caveats (absolute fps are lower than cycle 11b's 48-50):** first runs after the data dir
migration (shader cache / ASTC disk cache rebuilt in run 1); the desktop held ~1245 MiB of VRAM
(two Claude desktop GPU processes, sunshine, cosmic apps) vs ~740 MiB in cycle 11b, and two java
processes used ~35 % of a core in the background. Consequence: the out-of-memory ratchet fired in
**every** run (REF up to 512 MiB, C12 1024 MiB, c12-3 2816 MiB), so all runs are over critical on
~50 % of frames with 100-190 evictions/s. Compare C12 vs REF within this round only.

## Runs (order)

| # | tag | build / switches | fps | low | stalls | worst ms | GPU util / W / VRAM MiB | evict/s | ASTC re-created/s | OOM ratchet max |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | c12-chk | C12 + all CHECKs + MT_VERIFY + WATCHDOG, shots 10 s (warm-up) | 24.1 | 23.0 | 7 | 259 | 35 / 64 / 7191 | 122 | 43 | - |
| 2 | c12-chk2 | C12 + all CHECKs + WATCHDOG (no VERIFY), shots 10 s | 34.5 | 33.6 | 5 | 316 | 48 / 133 / 6929 | 248 | 121 | 1408 |
| 3 | ref-verify | REF + MT_VERIFY + WATCHDOG (REF warm-up) | 23.5 | 22.9 | 5 | 247 | 34 / 109 / 7413 | 81 | 19 | - |
| 4 | c12-1 | C12 default | **42.8** | 40.4 | 2 | 154 | 52 / 143 / 7057 | 110 | 37 | 1024 |
| 5 | ref-1 | REF default | **39.3** | 35.8 | 3 | 222 | 51 / 139 / 7422 | 125 | 34 | 512 |
| 6 | c12-2 | C12 default | **45.5** | 43.8 | 3 | 175 | 52 / 147 / 7053 | 100 | 43 | 1024 |
| 7 | ref-2 | REF default | **39.1** | 36.8 | 1 | 166 | 50 / 140 / 7380 | 118 | 36 | 512 |
| 8 | ss-c12-1 | C12 default, 2P (calibration + counters, shots 5 s) | **29.7** | - | 0 | - | 49 / 125 / 6162 | 42 | 15 | 2048 |
| 9 | ss-ref-1 | REF default, 2P | **31.1** | - | 0 | - | 53 / 129 / 6595 | 35 | 6 | 1408 |
| 10 | c12-3 | C12 default | **37.5** | 36.4 | 4 | 226 | 48 / 137 / 7189 | 189 | 78 | **2816** |

No run aborted (none under 10 fps). Host-resident textures: 0 MiB in every 1P window; **2P: 360 MiB
(C12) / 180 MiB (REF)** at peak (VRAM pressure, both builds).

2P fps = presents 6000 -> 10200 / wall time from the `present #N` lines (same present range for both;
fps.py's marker-to-end window covers different ranges because REF got further: C12 30.4 over
f5400-12000, REF 33.4 over f5400-13200 incl. a 64 fps load window at its start).

## Means and ratios

| | C12 | REF | C12/REF |
|---|---|---|---|
| 1P (runs 4,6,10 vs 5,7) | **41.9** (37.5-45.5) | **39.2** (39.1-39.3) | **1.07** (1.13 without c12-3) |
| 1P low (approx 1 %) | 40.2 | 36.3 | 1.11 |
| 2P (run 8 vs 9, one each) | **29.7** | **31.1** | **0.96** |

1P: C12 won both interleaved pairs (+9 %, +16 %); c12-3 (last run) is the outlier, explained by the
VRAM ratchet climbing to 2816 MiB (critical 3225 MiB vs ~5000, 189 evictions/s, 78 re-creations/s).
REF was very stable (39.1/39.3). 2P is n=1 per arm, within noise of a -5..+5 % band; C12's 2P run
ratcheted further (2048 vs 1408 MiB, over critical 18 % vs 2.5 % of frames).

## Per-thread / stage (1P race window means; 2P in the last column pair)

| | c12-1 | c12-2 | c12-3 | ref-1 | ref-2 | ss-c12 | ss-ref |
|---|---|---|---|---|---|---|---|
| draw packets/s | 74.6 k | 74.1 k | 78.1 k | 77.5 k | 81.9 k | 96.9 k | 97.3 k |
| top: front (GPU) % | 61 | 64 | 61 | 74 | 72 | 74 | 73 |
| top: GPUBack % | 71 | 74 | 70 | 69 | 69 | 80 | 75 |
| top: GPUCommit % | 76 | 74 | 76 | 73 | 74 | 80 | 76 |
| guest cores % (mean) | 60 | 63 | 60 | 59 | 61 | 52 | 54 |
| sequencer draws ms/s | 420 | 434 | 434 | 412 | 420 | 495 | 455 |
| sequencer wait-committed ms/s | 26 | 29 | 24 | 25 | 25 | 33 | 37 |
| committer idle ms/s | 247 | 262 | 220 | 223 | 224 | 228 | 247 |
| serial wait ms/s | 7.7 | 6.4 | 7.3 | 8.3 | 8.4 | 7.5 | 6.5 |
| front list-writer waits (compute) /s, ms/s | 461, 173 | 504, 206 | 406, 184 | n/a | n/a | 646, 287 | n/a |
| posadj reads / served per s | 173 / 92 | 224 / 122 | 186 / 90 | - | - | 0 / 0 | - |

Front busy (top) falls from ~73 % (REF) to ~62 % (C12) in 1P: fewer/shorter front waits. The front's
largest remaining wait is the **list writer waits on compute (~170-210 ms/s 1P, ~290 ms/s 2P)**,
mostly program 0x0 and two compute programs (0x114cc6d700, 0x1157eb0e00). Posadj never fires in 2P
(0 reads).

## Repeat hit rates (C12, race window)

| | c12-1 | c12-2 | c12-3 | ss-c12 |
|---|---|---|---|---|
| W3 capture repeat hits | 58.6 % | 55.5 % | 59.8 % | 52.9 % |
| W2 buffer repeat taken | 15.9 % | 16.9 % | 17.0 % | 7.2 % |
| texture repeat (draws given it) | 36.8 % | 34.3 % | 37.1 % | 39.3 % |
| C11 identical captures | 50.2 % | 49.5 % | 49.8 % | 40.6 % |

Capture memo `gen-skipped` pages are 0 in every run (GENS is on but never skips: all pages are
compared or unprotected), so its gen path is untested by data, not exercised.

## CHECK verdict

- **c12-chk2 (CHECK, capture repeat active):** posadj payload mismatch **0** (18 lines), C12 commit
  repeat check mismatches **0**, C11 repeat check mismatches **0**, C12 capture check
  (614 k / 10 s checks) mismatches **0**, **gen stale 0**; no "C12 ... check" ERROR; watchdog
  silent (only its exit line). Timestamp mismatches are the expected words 2-3 class.
- **c12-chk (with MT_VERIFY):** same zeros, but VERIFY makes the capture repeat inactive
  ("inactive: verify/lockstep/resolvers"), which is why run 2 was added. VERIFY reported
  **85 277 mismatches / 129 k checks**, all of the "uncaptured syncs 1 (first 0x6f6362b00 / 0x159f70400
  +65536)" kind (no mirror-register mismatch). **REF shows the same (89 545 / 132 k, same addresses)**
  (run 3), so it comes from the sync base (upstream merge / its storage-buffer sizing), not from cycle
  12. Older VERIFY runs (c10/c11 era) had 0-225. To investigate on the sync base.
- **Screenshots** (`tools/runs/calib-c12-chk2/` t0120-t0190, `calib-ss-c12-1/` t0090-t0270): HUD
  (coins, lap, place, items), minimap, track, karts, water/sky correct in 1P; 2P both views correct
  (character select, course intro, start grid, lap 1/3 and 2/3 with both HUDs and the shared
  minimap). In run 1 the eden-cli window was behind other windows (shots show the desktop).
- 2P was not run with CHECKs (budget).

**Verdict: C12 correct** (all C12/C11 checks 0, screenshots correct); MT_VERIFY "uncaptured
syncs" flood is pre-existing in REF.

## 2P calibration

The uncalibrated `splitscreen/timeline.txt` works as is: shots show character select ~f2840,
course intro ~f3430, start grid ~f4790, racing at f5790 (lap 1/3) and lap 2/3 at f9940. Mark 5400
falls in the start scramble; the fair window is f6000-10200 (inside lap 1-2). `timeline.txt` was
not changed.

## Install

C12 correct and >= REF in 1P (41.9 vs 39.2) -> **installed**:
`build/pgo/bin/eglantine` copied to `~/Applications/eglantine-local/mt/eglantine` (backup
`eglantine.bak-c11-2026-11-10`) and `~/Applications/eglantine-local/sync/eglantine` (backup
`eglantine.bak-sync`).

## Not done

- Old C11 1P: skipped. Its pipeline cache version differs (pre-31) and it would read the migrated
  `~/.local/share/eden -> eglantine` dir: the run would be shader-translation heavy (no valid speed
  number in one run) and could rewrite the new cache. Run 10 went to a 3rd C12 1P instead.
- 3rd REF 1P, 2nd 2P pair, 2P CHECK run (budget 10 runs).

## Top remaining limits

- **1P:** VRAM pressure first (this session: OOM ratchet in every run, over critical ~50 % of
  frames, the c12-3 drop to 37.5 fps follows the ratchet to 2816 MiB; C12 ratchets further than REF,
  1024 vs 512 MiB, worth checking whether the memos add device allocations). On the CPU side the
  front's list-writer waits on compute (~190 ms/s) are now its largest wait; committer ~75 % busy
  with ~250 ms/s idle.
- **2P:** back end: GPUBack and GPUCommit both ~80 % (committer idle 228 ms/s), sequencer draws
  495 ms/s at 97 k draws/s, plus front list-writer waits 287 ms/s; host-resident textures appear
  (180-360 MiB). Posadj does nothing in 2P, buffer repeat only 7 %.
