# Cycle 13 results (2026-11-10)

Builds (pgo, eden-cli): **REF** = mkw-c12s (Eglantine-c12s, = installed build), **C13** = mkw-c13 at
23799a000f (integration of W1-W4, hooks W2 H1/H2, W3 H1-H3, W4 M3 H1-H3; one compile fix:
c13_vram.h no longer includes surface.h, whose gpu.h -> nvdata.h enum names `Success`/`BadValue`
collide with X11 macros in the Vulkan TUs). Defaults commit d17be613de: `C13_OOM` and
`C13_RELOC` on by default under MT lite (`=0` restores cycle 12).

Setup: `tools/race` (window 5400..7200, stopped 12 s after the 2nd marker) and
`tools/splitscreen` (fps over presents 6000 -> 10200), `NOPROF=1 EDEN_NX2_VSYNC_HZ=240`, config B +
`MT_STATS VRAM_STATS TC_CHURN C11_STATS C12_STATS C13_STATS`. C13 arms set `EDEN_NX2_C13_OOM=1
EDEN_NX2_C13_RELOC=1` explicitly (before the default flip). One emulator at a time, round lock.
Logs: `eglantine-buildenv/tools/runs/*-{c13-chk,c13-ss-chk,c13r-*}` (+ `pre-`, `smi-`, `wrap-`).

**Session conditions (worse than cycle 12):** CPU boost **0** in every run; desktop VRAM at run
start **1570-1740 MiB** (cycle 12: ~1245; Vivaldi GPU process 232 MiB with a YouTube video
playing, two Claude desktop GPU processes, sunshine, cosmic apps); driver budget ~6.08-6.14 GiB
(cycle 12: 6.45). Absolute fps are not comparable with cycle 12's table.

## Runs (order)

| # | tag | build / switches | result | GPU util / W / VRAM MiB |
|---|---|---|---|---|
| 1 | c13-chk | C13 1P, W1 + all C13/C12 CHECKs + MT_VERIFY + WATCHDOG + LISTWAIT_CHECK + LISTRANGE, shots 10 s | 26.2 fps (c12-chk 24.1) | 38 / 111 / 7507 |
| 2 | (c13-ss-nomt-hang) | C13 2P, **wrapper bug: env not passed, ran without MT** | stalled at 56 s, aborted (log kept as log-c13-ss-nomt-hang.txt) | - |
| 3 | c13-ss-chk | C13 2P, W1 + all CHECKs (no VERIFY), shots 5 s | 27.9 fps | 49 / 124 / 7052 |
| 4 | c13r-c13-1 | C13 1P, W1 | **36.2** (low 35.9) | 45 / 130 / 7603 |
| 5 | c13r-ref-1 | REF 1P | **aborted <10 fps** (3 fps from ~60 s, 2 images / 145 MiB host-resident) | - |
| 6 | c13r-c13-2 | C13 1P, W1 | **41.2** (low 40.3) | 49 / 136 / 7597 |
| 7 | c13r-ref-2 | REF 1P | **aborted <10 fps** (3 fps from ~60 s, 7 images / 318 MiB host-resident) | - |
| 8 | c13r-ss-1 | C13 2P, W1, commit profile, perfthreads GPUCommit/GPUBack 10 s | **27.9** | 46 / 122 / 7101 |
| 9 | c13r-ref-ss-1 | REF 2P | **aborted <10 fps** (~3 fps, 8 images / 671 MiB host-resident) | - |
| 10 | c13r-c13-now1 | C13 1P, W1 **off** (what the default would have been) | **40.0** (low 39.0) | 44 / 132 / 7516 |

The RT diet run and the `KICK=a / MEMO_WAYS=a` A/B run did not fit (run 2 wasted, runs 5/7/9
aborted; the third C13 1P went to the W1-off control instead).

## Means and ratios

| | C13 | REF | C13/REF |
|---|---|---|---|
| 1P (W1 on: runs 4, 6) | **38.7** (36.2-41.2) | did not reach the race (2/2 aborted) | n/a (REF unusable this session) |
| 1P (W1 off: run 10) | 40.0 | | |
| 2P | **27.9** (runs 3 and 8, both 27.9) | did not reach the race (1/1 aborted) | n/a |

REF fell into the paging collapse (render targets / large images placed in host memory at the
loads and never moved back, ~30 frames per 10 s) in all three REF runs. C13 with RELOC moved them
back (2-9 rebinds per run, host-resident 0 MiB in every C13 window). The W1-off C13 run did not
collapse (only 5 MiB of images ended in host memory: luck of the load), so the collapse is
placement-dependent; RELOC is what recovers from it.

## Per thread / stage (race window means)

| | c13r-c13-1 | c13r-c13-2 | c13r-c13-now1 (W1 off) | c13-ss-chk | c13r-ss-1 | (c12 cycle 12: c12-2 / ss-c12-1) |
|---|---|---|---|---|---|---|
| draw packets/s | 76.3 k | 71.4 k | 83.6 k | 84.8 k | 92.5 k | 74.1 k / 96.9 k |
| sequencer draws ms/s | 453 | 433 | 467 | 478 | 498 | 434 / 495 |
| committer idle ms/s | 272 | 286 | 277 | 279 | 287 | 262 / 228 |
| front list writer waits (compute) ms/s | 200 | 203 | 189 | 265 | 292 | 206 / 287 |
| commit profile us/draw (sampled) | 6.6 | 6.7 | 6.0 | 7.0 | 6.0 | - |
| W3 capture repeat hits | 58.5 % | 60.5 % | 61.0 % | 49.6 % | 49.7 % | 55.5 % / 52.9 % |
| W2 buffer repeat taken | 24.6 % | 24.2 % | 26.7 % | 11.2 % | 8.6 % | 16.9 % / 7.2 % |
| evictions/s | 178 | 160 | 165 | 50 | 57 | 100 / 42 |
| ASTC re-created/s | 65 | 55 | 69 | 12 | 17 | 43 / 15 |
| frames over critical | 100 % | 100 % | 100 % | 24 % | 63 % | 100 % / 39 % |
| heap usage / budget MiB | 5880 / 6112 | 5825 / 6134 | 5760 / 6141 | 5260 / 6084 | 5362 / 6137 | 5677 / 6457 |
| host-resident MiB (max in window) | 0 | 0 | 40 | 0 | 0 | 0 / 360 |

## VRAM (W1)

- **V1 OOM:** the cycle 12 rule would have ratcheted to 2432-2944 MiB in 1P (shadow "old rule"
  column); the new rule held the margin at **0 MiB** in every race window (episodes only at the
  loads: e.g. 2P "failures 7, deficit -224 MiB, old margin 512 -> 1024, new 0 -> 31"). Refails: 0
  in every race window (non-zero only during loads). No "Out of device memory" ratchet steps in
  the windows.
- But 1P is still over critical on 100 % of frames with 160-180 evictions/s: the floor rule lifts
  critical to ~4.8-4.9 GiB while the texture cache counts ~5.4-5.5 GiB used against a 6.1 GiB
  budget; the non-evictable set is ~3.1 GiB (RTs 1933 MiB). With this session's 1.6-1.7 GiB desktop
  the working set simply does not fit; evictions did not drop vs cycle 12 (whose budget was 340
  MiB larger).
- **V2 RELOC:** 2P host-resident **0 MiB** in the window (cycle 12: 360 MiB C12 / 180 REF);
  rebinds 2-9 per run, `no room` 0; RELOC_CHECK tiles 6 (1P) + 8 (2P), **0 mismatches**.
- **Census:** 32 2x RTs, 1829 MiB in 1P; largest 6144^2 f1 576 MiB, 3072^2x3 f106 216 MiB,
  3072^2 f1 144 MiB (diet candidates). VRAM census: VMA device-local ~5.2-5.3 GiB, texture images
  ~3.9-4.0 GiB, buffer pool 265 MiB, stream 128 MiB, staging upload ~100 MiB idle, **unattributed
  ~880 MiB** (driver usage outside VMA blocks). STAGING_TRIM not decided (census only; staging
  is small).
- V3 RT diet: not run (budget).

## CHECK verdict

- **MT_VERIFY (run 1): 0 mismatches** over 24 lines (~105-112 k checks / 10 s each; cycle 12 and
  REF had ~51-60 k mismatches/s, all "uncaptured syncs" on the two 64 KiB banks). W2 B1 fixes it:
  C13 bank line "uncaptured syncs 0, on a bank 0", sequencer banks ~19-21/frame in 1P (~140-160
  MiB / 10 s), ~28/frame in 2P, committer banks 0. B2 (CLAIM) not needed. Note "dirty at commit"
  44-66 k / 10 s (bank still CPU-modified at the committer's bind: the sequencer runs a frame
  ahead and marks the next frame's bank; VERIFY shows no uncaptured sync from it).
- C11 repeat / C12 commit repeat / C12 capture / memo-way checks: **0 mismatches** (1P ~20 M
  checks with VERIFY, 2P ~40 M checks with memo ways 2 active), posadj payload mismatch 0, gen
  stale 0, no "C1x ... check" ERROR, watchdog silent. Only errors: the 16 pre-existing "Unmapped
  Device ReadBlock" (same count in every cycle 12 log).
- Screenshots: 1P (`calib-c13-chk`, t0200/t0230) HUD, coins, lap, place, items, minimap, track
  correct; 2P (`calib-c13-ss-chk`, t0066-t0287) both views, both HUDs, shared minimap, water
  correct. Verdict: **C13 correct**.
- L0: LISTWAIT_CHECK same 99.1-99.5 %, but **would differ 2 (1P) / 3 (2P)** -> LISTWAIT stays
  off (L3 not flipped). LISTRANGE gate decided **off** (shorter 605 of 20000).

## W3 kick / W4 memos / W4c gate

- Kick (on by default): 1P ~660-710 learned waits per 10 s all kicked, 2.1-2.4 ms each; "check
  behind kick" 0 in 1P perf runs but **100 %** in both CHECK runs (531/531, 1153/1153) and front
  list waits did not drop (189-203 ms/s 1P, 265-292 2P vs 206/287 in cycle 12). No measurable
  benefit; no A/B run this round. 2P: kick marks 1343, seq memo skips 76 k / 10 s.
- Memo ways (2): 2P capture hits 49.6 % (cycle 12 44.1 % by the plan's count, 52.9 % by the
  results table's), "other way" hits only 2.2 % of lookups; buffer memo taken 8.6-11.2 % in 2P (7.2
  % in cycle 12), 24-27 % in 1P (16-17 %); ubind other-way translations ~150 k / 10 s.
- **W4c gate (commit profile, 2P): resolve 61 %, encode 16 %, stage bind 16 %, other 7 %** ->
  "encode <= 40 %": no second committer; cycle 14 should split the sequencer's capture per view.
  perf GPUCommit (2P): Idle 21.5 %, SpinRecursiveMutex::LockContended 4.8 %,
  DoUpdateGraphicsBuffers 4.2 %, GpuToCpuAddress 3.7 %, BindHostGraphicsUniformBuffer 2.7 %,
  PrepareImage 2.5 %. GPUBack: BackLoop 13.4 %, CaptureDraw 7.1 %, Maxwell3D::CallMethod 4.9 %,
  LockContended 4.5 %.

## Install

C13 correct; REF could not run either mode in this session's VRAM conditions (3/3 aborted at ~3
fps) while C13 completed every MT run -> **installed** (C13 with OOM/RELOC default-on, d17be613de):
`build/pgo/bin/eglantine` copied to `~/Applications/eglantine-local/mt/eglantine` and
`~/Applications/eglantine-local/sync/eglantine` (backups `eglantine.bak-c12-2026-11-10`).

## Open items

- Run 2 (no MT, wrapper bug) stalled at 56 s of the 2P load with all threads idle after the
  pre-existing `tile_width_spacing` assert; untested whether REF does the same without MT. The
  "otros" launcher runs the sync binary without MT (other games), worth one check.
- No same-session REF number exists; C13 vs C12 speed is undetermined (cross-session: C13 38.7 1P
  / 27.9 2P vs cycle 12's 41.9 / 29.7 with ~500 MiB less desktop VRAM and no video playing).

## Top remaining limits

- **VRAM fit (1P and 2P):** with a 6.1 GiB budget the 1P working set is over critical every frame
  (160-180 evictions/s, 55-70 ASTC re-creations/s). Next: RT diet (576 + 216 + 144 MiB of 2x
  square targets are candidates), and attribute the ~880 MiB "unattributed" driver usage.
- **Front list writer waits** 190-290 ms/s, unchanged by the kick (the waits are GPU/committer
  work, not backlog).
- **2P back end:** sequencer ~500 ms/s at 92 k draws/s, committer resolve-dominated (61 %):
  per-view capture split, not a second committer.
