# Cycle 11b results (2026-11-10)

Build: mkw-c11 at 9b56f022f4 (analysis fixes + portability fixes) compiled with no errors and no
source changes. Race benchmark `tools/race` (assists ON, window 5400..7200 = 1800 presents),
`NOPROF=1 EDEN_NX2_VSYNC_HZ=240`, config B (`EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 EDEN_NX2_ASTC_BC7=1
EDEN_NX2_ASTC_DISKCACHE=1`) plus `EDEN_NX2_MT_STATS=1 EDEN_NX2_VRAM_STATS=1 EDEN_NX2_TC_CHURN=1
EDEN_NX2_C11_STATS=1` on every run. Each run was stopped 12 s after the 2nd marker. One emulator at a
time and no builds during the runs. CPU boost was **0 (off)** in all 9 runs. C10 = Eglantine-mtB
pgo, C11 = Eglantine-c11 pgo. GPU util/power/VRAM = mean of `nvidia-smi -l 1` between the markers.
Counters are the means of the 10 s lines inside the window. Logs: `tools/runs/*-c11b-*`.

## fps table (run order)

| # | tag | build / switches | fps | low | stalls | GPU util / W / VRAM | evict/s | ASTC re-created/s | TC used / critical MiB | frames over critical | OOM margin events |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | c11b-drop | C11 + DROP1X_CHECK + shots every 10 s | 51.0 | 48.6 | 2 | 48 % / 150 W / 7009 | 55 | 9.4 | 6007 / 6032 | 160 / 2518 | 0 |
| 2 | c11b-c10-1 | C10 | 42.7 | 45.8 | 0 | 41 % / 140 W / 7463 | 151 | 25.2 | 6420 / 5927 | all | 0 |
| 3 | c11b-def-1 | C11 default | 47.1 | 44.8 | 5 | 43 % / 146 W / 6503 | 259 | 101.7 | 5417 / 4735 | all | 4 (to 2048 MiB, at 42-46 s, loading) |
| 4 | c11b-cap-1 | C11 PRESSURE_CAP_MB=1024 | 41.3 | 43.1 | 5 | 41 % / 139 W / 7182 | 122 | 13.9 | 5818 / 5818 | 2081 / 2591 | 0 |
| 5 | c11b-c10-2 | C10 | 45.5 | 45.5 | 5 | 40 % / 144 W / 7517 | 147 | 27.0 | 6247 / 5831 | all | 0 |
| 6 | c11b-cap-2 | C11 PRESSURE_CAP_MB=1024 | 53.8 | 52.2 | 3 | 49 % / 155 W / 7027 | 92 | 11.6 | 5768 / 5775 | 355 / 2129 | 0 |
| 7 | c11b-def-2 | C11 default | 49.2 | 45.9 | 4 | 46 % / 148 W / 7060 | 110 | 20.0 | 5846 / 5850 | 697 / 2476 | 0 |
| 8 | c11b-c10-3 | C10 | 41.6 | 42.1 | 4 | 44 % / 139 W / 7551 | 166 | 40.0 | 6104 / 5602 | all | 0 |
| 9 | c11b-def-3 | C11 default | 49.5 | 47.8 | 2 | 48 % / 151 W / 7030 | 53 | 8.5 | 5936 / 5946 | 532 / 2448 | 0 |

No run was aborted (none fell under 10 fps). Host-resident texture memory was 0 MiB in every window.
The error lines (13 "Uncached storage pointer", "Unmapped Device ReadBlock", ...) are the same 31
in C10 and C11. No "C11 ... ERROR" and no watchdog report.

## Means and ratio

| arm | runs | mean fps | range |
|---|---|---|---|
| C10 B | 2, 5, 8 | **43.3** | 41.6-45.5 |
| C11 B default | 3, 7, 9 | **48.6** | 47.1-49.5 |
| C11 B default incl. the DROP1X check run | 1, 3, 7, 9 | 49.2 | 47.1-51.0 |
| C11 B cap 1024 | 4, 6 | 47.6 | 41.3-53.8 |

**C11/C10 = 1.12** (48.6 / 43.3; 1.14 with run 1). The ranges do not overlap: the slowest C11
default run is faster than the fastest C10 run. The gain matches the churn counters: C10 is over
critical on every frame (critical ~5.6-5.9 GiB, used 6.1-6.4 GiB), with 147-166 evictions/s and
25-40 ASTC re-creations/s. C11 is over critical on 20-30 % of frames, with 53-110 evictions/s and
8.5-20 re-creations/s. The exception is def-1, where the ratchet fired. GPU busy rises from 40-44 %
to 46-48 %. Compared with the cycle 10 era baseline (as-b1/b2 46.9 / 51.0), today's C10 runs are
slower, so part of the ratio may be session-specific. The ordering held in every interleaved pair.

## DROP1X (run 1, CHECK)

- It now drops: 29 images / 448 MiB in the first sweep after boot, then 40 / 575 MiB in total over
  the run. Re-created: 10 / 111 MiB, all "scale-down" and almost all during the loads. In the race
  window it re-created ~0 (1 image of 64 KiB). Rescale originals fell from 461 MiB (c11-chk) to
  13 MiB (30 dropped). Refusals are now 24-57 per 10 s, all "flags" (0 storage views, 0 blit helper).
- Speed runs: 41-61 dropped (523-707 MiB) and 11-29 re-created (60-240 MiB) per run, mostly in loads.
- That freed memory is why the out-of-memory ratchet stopped. It fired in 1 of 5 default-cap C11
  runs this round (def-1, max 2048 MiB), against all 3 runs in c11-s1/c11-chk/c10-s1 last round.
- **Screenshots** (`runs/calib-c11b-drop/`, t0090-t0130 in the race): HUD (coins, lap, place,
  item box), minimap, track, water, karts and items render correctly. No black or stale
  textures. **Verdict: correct. DROP1X stays on (default).**

## Pressure cap (EDEN_NX2_C11_PRESSURE_CAP_MB=1024 vs 3072)

The main hypothesis is mostly moot now. With DROP1X working, the margin ratchet did not fire in
either cap run or in 2 of 3 default runs, so the cap had nothing to change in those runs. The cap
runs gave 41.3 and 53.8 fps (the 41.3 run had 0 pressure events, so that spread is noise). The one
default run where the ratchet fired (def-1: critical 4735, 259 evictions/s, 102 re-creations/s)
still ran at 47.1. **Verdict: inconclusive / no measurable gain. The default stays at 3072. Cap
1024 is safe (no host placement, no bad frames) and is the knob to use if the ratchet comes back.**

## Not measured

Priority d (REPEAT=a, LOCKRUN=0) was not run. The last 2 of the 9 runs went to a 3rd C10/C11 pair
to firm up the speed ratio for the install decision.

## Install

The best C11 config is the default config B (no extra env), it is correct, and it is faster than
C10. The C11 `build/pgo/bin/eglantine` was installed to `~/Applications/eglantine-local/mt/eglantine`.
The previous binary was kept as `eglantine.bak-c10-2026-11-10`.
