# Cycle 10 data (2026-11-09, PC idle, branch mkw-mt-lite 2ad8d79)

Race benchmark: tools/race (eglantine-buildenv), Grand Prix race 1 (Mario Bros. Circuit), 2x,
VSYNC_HZ 240, speed limit off, NOPROF unless noted. Window = presents 5400..7200 (1800 presents,
inside lap 1; the kart is driven by an A duty cycle, no steering: 24th, partly off-road).
GPU columns from nvidia-smi 1 Hz over the window. Stable = Eglantine-mkw (mkw-perf2 77e46bc).
B = `EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 EDEN_NX2_ASTC_BC7=1 EDEN_NX2_ASTC_DISKCACHE=1`.

## Calibration
- cal1 (uncalibrated timeline): reached the race, but A was only tapped every 90 frames until
  4800 while GO came at ~f3900-4200. Fix: `2490 A 82 x80@90` (A held 82 of every 90 frames:
  each re-press confirms a menu, ~91 % accelerate in the race), `mark 5400`, `markend 7200`,
  `end 7260`. cal2 (stable, screenshots every 10 s): window is fully in the race, 2 markers.
- Boot: MT-lite binary sometimes needs 65 s to the first present (pipeline cache load 5 s -> 53 s,
  stage cache translates ~7.4k stages) vs 42 s for stable; other runs boot normally.

## Race fps (window)
| run | config | fps | low | stalls>100ms | GPU util / power | notes |
|---|---|---|---|---|---|---|
| s1 | stable | 22.6 | 22.0 | 4 | 90 % / 122 W | fence wait 0 (CPU/back bound) |
| cal2 | stable (+shots) | 22.0 | 21.3 | 7 | 90 % / 122 W | |
| s3 | stable | 21.7 | 21.5 | 2 | 90 % / 121 W | |
| s2 | stable | 17.7* | 8.3 | 156 | 96 % / 96 W | *never reached the race: VRAM collapse |
| b0 | B + churn/vram/mt stats | 50.2 | 48.4 | 5 | 53 % / 149 W | fence 0 |
| b1 | B | 31.7 | 30.0 | 11 | 77 % / 130 W | GPU-bound: fence 10.7-12.8 ms/frame |
| b2 | B | 49.1 | 46.2 | 4 | 53 % / 148 W | fence 0 |
| perf1 | B (+perf, shots) | 51.7 | 51.0 | 3 | 54 % / 152 W | |
| chk1 | B + checks + GPU_PROFILE | 29.9 | | 8 | 71 % / 126 W | |
| chk2 | B + VERIFY | 24.5 | | 10 | 69 % / 120 W | |
| prof1 | B + GPU_PROFILE + stats | 26.4 | | 10 | 79 % / 124 W | GPU 28.6-29.8 ms/frame (timestamps) |
| nodc1 | MT lite BC3, no disk cache | 21.7 | 20.2 | 17 | 81 % / 120 W | churn loop, fence 6-23 ms |
| dc1 | MT lite BC3 + disk cache | 6.0* | 2.9 | 793 | 95 % / 95 W | *collapse, race never reached |
| old1 | MT lite BC3, TC_BUDGET_AUTO=0 | 7.6* | 2.9 | 611 | 95 % / 95 W | *collapse |
B is bimodal: fast runs 49-52 fps (GPU 53 %, ~150 W, no fence wait) and slow runs 26-32 fps
(GPU-bound, 120-130 W, GPU 28.6 ms per frame vs < 20 ms). Low power with high utilisation is the
signature of the GPU waiting on memory over PCIe. Stable vs B (fast runs): 22.1 vs 50.3 fps.

## VRAM (EDEN_NX2_VRAM_STATS / TC_CHURN)
- Budget 6.4-6.5 GiB of 8 (desktop ~1.3 GiB). Process usage 6.25-6.46 GiB = at the budget in
  every MT run; VMA slack (free space inside blocks) 450-700 MiB.
- Texture cache critical 5.0-5.3 GiB (budget auto) but device memory 5.7-6.2 GiB: over critical
  in every frame; B: evictions 210-290/s, ASTC re-created 76-88/s, served by the disk cache
  (1300-1900 hits / 10 s, hash 300-760 ms / 10 s). BC3 without disk cache: same churn, decoded
  on the GPU (fence wait 6-23 ms).
- dc1 collapse: `vkAllocateMemory ... failed 472 / 305` per 10 s while "pressure events 0" and no
  "host memory" warnings. MemoryAllocator::CreateImage uses AUTO_PREFER_DEVICE with only a
  *preferred* DEVICE_LOCAL flag, so VMA silently falls back to a host memory type when the device
  heap is full; buffers likewise. Afterwards fps stays at 3 even with churn ~0: resources stay in
  system memory. Same signature (95 W at 95 % util) in s2 (stable) and old1.

## Correctness (config B, race)
- chk1 (GPUPARAM_CHECK, REGDEDUP_CHECK, DMA_READ_CHECK, MT_WATCHDOG, STAGE_CACHE=verify,
  GPU_PROFILE): GPU params check ~18k compared / 10 s, 0 mismatched (v2 too); regdedup 0 would
  have emitted; DMA reads 0 stale; stage cache 0 verify mismatches; watchdog 0 "MT wait > 2 s".
- chk2 (MT_VERIFY + LITE7_CAPTURE_CHECK): 4 "upload changed" lines (4-10 bytes of floats, the
  known concurrent guest write class), capture check desc/ubo/lock/slot all 0.
- perf1 screenshots in the race window: picture correct (BC7/BC1 textures, karts, HUD, water).

## Profile (config B)
- top (b0, fast): GPU front 76 %, GPUBack 75 %, GPUCommit 71 %, 6 guest cores ~73 %: nothing at
  100 %, a front/back ping-pong.
- perf1 (fast, 51.7 fps), one thread at a time, 14 s each:
  GPU front: WaitConsumed 18.0 %, sched_yield 15.2 %, TimedWaitSettled 5.1 % (= ~38 % waiting on
  the back), FrontRunMacro 7.1 + 6.7 %, ProcessCommands 7.1 %, FrontWrite 5.5 %, MacroWrite
  4.5 %, DmaPusher CallMethod 4.1 %.
  GPUBack: BackLoop 13.2 %, CaptureDraw 7.6 %, CallMethod 5.3 %, SpinRecursiveMutex 5.1 %,
  Execute 3.3 %, SeqReadDescriptor 2.5 %, GpuToCpuAddress 2.3 %, WriteBlockCached 2.2 %.
  GPUCommit: Idle 21.5 %, SpinRecursiveMutex 5.3 %, DoUpdateGraphicsBuffers 4.8 %,
  GpuToCpuAddress 4.1 %, PrepareImage 3.1 %, RefreshContents 2.1 %.
- prof1 (GPU_PROFILE, slow run): front push 654-678 ms/s, macro 93-96 ms/s (909k calls/s),
  split drain 163-185 ms/s, 5.7-5.9M 3D register writes/s (3.2M same value). Drains/s: writers
  (commands) 800-1180 (235-270 waited, 92-103 ms), puller 89-95 (all waited, 36-58 ms), position
  29-34 (all waited, 18-37 ms). Acquire drains: "site 1 op 41 pending met" ~63/s (two addresses
  0x21f6697040/0x...140): acquires a queued release already satisfies still drain.
  Back: split wait 610-690 ms/s (GPU-bound run). MT stages: sequencer draws 100-160 ms/s,
  committer idle 650-710 ms/s (fast run b0: 350-410 / 270-345).
- Host CPU: Ryzen 7 5800X with cpufreq boost = 0 (disabled) during all runs.

## Logs
eglantine-buildenv/tools/runs/{log,cli,racefps,gpu,threads}-<tag>.*; perf data runs/perf/perf1-*.data.
