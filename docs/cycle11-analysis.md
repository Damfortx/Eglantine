# Cycle 11 analysis (interrupted test round, 2026-11-09/10)

Branch mkw-c11 (base accc7c424e) vs cycle 10 mkw-mt-lite (9eb3813ca8). Logs in
tools/runs: `*-c11-chk*` (all CHECK switches + MT_STATS + VRAM_STATS + TC_CHURN),
`*-c11-s1*`, `*-c11-s2*`, `*-c10-s1*` (config B, no stats), `calib-c11-chk/` (screenshots).
`c11-x1` was contaminated (a clang++ build ran at the same time, killed at ~f4800) and
`c10-s2` never reached the race markers: both are left out. Boost 0 in every run.

## Results

| run | fps (race window) | low | stalls | GPU busy |
|---|---|---|---|---|
| c11 B s1 | 45.4 (win 45.9) | 43.4 | 4 | 45 % |
| c11 B s2 | 46.5 (win 47.5) | 44.4 | 5 | 43 % |
| c10 B s1 | 48.1 (win 49.0) | 48.4 | 4 | 47 % |
| c10 B earlier | 46.9 / 51.0 | | | |
| c11 all CHECK | 37.7 | 36.8 | 5 | 58 % |

The c11 mean is 46.0 and the c10 mean is 48.7 (3 runs). That is -4 to -5 %, inside the +-5 fps
spread cycle 10 itself showed, so it may be noise. It is not the +15-30 % the plan expected.
Per-thread CPU in the race window (top -H, % of a core): front 76 / 76 vs c10 81, committer 77 / 79
vs 82, sequencer 74 / 75 vs 77, guest cores 383 / 391 vs 398. Every thread does about the same
work per frame as in c10, so no workstream moved a ceiling.

Only the CHECK run has the C11 / MT counters (the B runs had no STATS switches). Its numbers are
per frame at 37.7 fps. The CHECK work (ACQFWD_CHECK drains and REPEAT_CHECK full paths) makes it
slower.

## Findings per workstream

| piece | what it actually did (c11-chk, race window, per s unless noted) | verdict |
|---|---|---|
| W1 F1 ACQFWD | tries 0-7 **per 10 s**, forwarded 0, refused "other channel" (11) / "max short" (1). VERIFY checked 0, CHECK agree 0 / unmet 0 | **No case to forward.** The earlier paths (MetInMemory, PredictedRelease, ledger, C10 B1) already take every acquire with a pending release. **Default now OFF** |
| W1 F1 VERIFY | never fired (0 forwards) | cost 0. Stays "on with ACQFWD" |
| W1 F1b POSFWD (off) | past-known 22-73/s, always the same shape: `[macro 0x0 +0 24B of 16B direct]` | A 24-byte read over a 16-byte direct release: bytes 16-23 lie past the release, so the timestamp words (8-15) POSFWD would supply do not help. **Stays off** |
| W1 F2 LISTRANGE (off) | not measured (off, no list counters) | stays off, still unmeasured |
| W2 S1 SAMECHAN | same 0, same-back 0 | GPU::BindChannel already skips these. Costs nothing: **keep** |
| W2 S2 CHANCMD | channel commands 103-117/s, refused 0, rebind register diffs 0, serials left 3-47/s (external/flush/blit) at 1.3-7 ms/s | **Works.** Channel binds are no longer Serials (c10: ~188/s at 50 fps). **Keep** |
| W2 S3 DISPATCH_CMD | collision packets 34-39/s, 0 serial. Publish guard: checks = waits = every packet, 27-32 ms/s, about 0.8 ms each. That is the whole sequencer wait-committed (27-32 ms/s) | **Keep**, but it gains little: the guard pays the same committer backlog the Serial paid. Total sequencer waits ~30-38 ms/s vs c10's 44-49 ms/s (about 0.9 vs 0.95 ms/frame). NOGUARD is the only further gain and needs a correctness run |
| W3 V1 DROP1X | **0 dropped in every sweep**, refused 146-416 per 10 s (= all ~32 rescaled images per sweep). Rescale originals stay at 461 MiB | **Bug, fixed.** The constructor always sizes `storage_image_views` to the image's levels (null handles), so `!storage_image_views.empty()` refused every image. The fix checks for a non-null view. The refusals are now split by reason. **Keep on**, but the path has never run: the next round must check it |
| W3 V2 DECODE_TRIM | decode classes 310 MiB -> 1-10 MiB in the race (8+21+14 classes, ~370 MiB trimmed); 5-19 misses per 10 s re-allocate small classes | **Works, keep** |
| W3 V3 categories | 2x RTs 1818 MiB (32), originals 461 MiB, ASTC BC7 ~500 MiB + BC1/3 ~370 MiB, other sampled ~590 MiB, buffers/staging/other ~1.75 GiB; host-resident 37-219 MiB at times | keep (diagnostic) |
| W3 V4 COORD | relocation skipped for the margin 137-329 per 10 s (loading only); emergency passes keeping hot images 30-50/s = **every frame** | keep. The cause is the pressure margin (see below), not V4 |
| W3 V5 RAMTIER (off) | not used | stays off |
| W4a REPEAT | 42-59 % of captures with textures identical; of those flagged at commit ~37 % refused "image" (CPU-modified/aliased), ~2 % "state". With CHECK all eligible went to the check: 0 taken by design, **0 mismatches in ~1.4 M checks**. Expected take rate ~25 % of textured draws | Correct, but the committer had 343-381 ms/s idle: it is not the ceiling, which was the plan's own condition for a gain. The sequencer key build and the committer memo fill cost something on every draw. **Default now OFF**, A/B with `=a` |
| W4b LOCKRUN | 2.0-2.5 draws/run. Runs ended by idle 40-60 %, other entry 10-30 %, flush ~4 %, contended 3-5 % (1.5-2.7 k/s), max 2-3 % | Saves about half the lock pairs, but runs are short: the committer catches up with the ring. No deadlock or watchdog report. **Keep**, A/B with `=a` and LOCK_STATS |

Checks: REPEAT_CHECK mismatches 0. ACQFWD_CHECK had nothing to check. No "C11 ... ERROR"
lines and no MT_WATCHDOG report. The error lines (16 "Unmapped Device ReadBlock", 13 "Uncached
storage pointer") are the same set as in c10-s1. Screenshots t0080/t0110/t0140 (race, 35-39 fps)
render correctly: HUD, minimap, track, items and karts. They do not cover DROP1X (it never
dropped) or the REPEAT fast path (CHECK mode runs the full path).

## Cross-cutting finding: the out-of-memory margin ratchet (cycle 10 code, not c11)

"Out of device memory: keeping N MiB more free" climbs by 512 MiB per pressure event to the 3 GiB
cap within ~20 s of the race in c11-s1, c11-chk and c10-s1. c11-s2 stopped at 896 MiB, and it is
the fastest c11 run. It decays by only 128 MiB per 600 quiet frames, and the events never stop.
With it, critical is ~3.2 GiB while the cache holds 5.8-6.0 GiB of images it touches every frame:

- the emergency collector runs every frame ("frames over critical 293 of 293");
- churn: evictions 84-222/s, ASTC re-created 5-97/s, disk cache hits 48-122/s, hash 150-313 ms
  per 10 s, all on the committer;
- host placement still happens (4 images / 74 MiB in one window).

cycle 10 A has the same problem. It may explain part of the run-to-run spread in both arms. A knob
was added: `EDEN_NX2_C11_PRESSURE_CAP_MB` (default 3072 = unchanged, 0..3072).

## Changes in this commit (uncompiled)

- `mt/c11_front.h`: `EDEN_NX2_C11_ACQFWD` default off (`1`/`a` on); start line updated.
- `mt/c11_commit.h`: `EDEN_NX2_C11_REPEAT` default off (`1`/`a` on); local `far` renamed
  `too_far` (`far` is a macro in `<windows.h>`: MSVC syntax error).
- `vk_texture_cache.cpp` `Image::C11DropOriginal`: storage-view refusal fixed (non-null handle,
  not a non-empty vector); refusals split (flags / storage views / blit helper).
- `mt/c11_vram.h`: counters `refused_views` / `refused_helper` in the "C11 VRAM in 10 s" line;
  `PressureCap()` + start line field.
- `texture_cache.h` TickFrame: pressure margin capped by `C11Vram::PressureCap()`.
- Portability (docs/portability-fixes.md, Eglantine-pub b8e2178c97 + 1010be1751, applied
  cleanly): shadowing renames (kepler_compute.h, gpuparam.h, list_dump.h, vk_mt_pipeline.h
  CaptureDraw `MaxwellRegs`, vk_mt_resolve.h), MSVC `_mm_pause` pauses (gpu_split.cpp,
  resolve_sync.h, vk_mt_pipeline.h), Linux-only `<sched.h>`/`<pthread.h>`, syncpoint_manager
  pthread guard, GCC `-Wdangling-pointer` pragma (selftest_phase5.h), `-Wclass-memaccess` cast
  (CommitCompute), C4244 casts (selftest.h, lsfg_mipmaps.cpp, vk_fsr3_upscaler.cpp,
  launch_params.cpp). I checked the new c11 headers and the c11 code in CaptureDraw for the same
  problems. The only new one was the `far` local; c11 code added no new `Maxwell::` uses in
  CaptureDraw. They have no x86 intrinsics or POSIX calls (spin_recursive_mutex.h already guards
  `_mm_pause` / aarch64 `yield`), and their u64 -> u32 narrowings are explicit casts.

## What the next test round must measure

Build first (nothing here was compiled). Same race, 2x, VSYNC 240, idle PC, boost recorded,
**no concurrent builds** (x1 was lost to one). The B runs must also carry the cheap counters
(`EDEN_NX2_MT_STATS=1 EDEN_NX2_VRAM_STATS=1 EDEN_NX2_TC_CHURN=1 EDEN_NX2_C11_REPEAT_STATS=1`)
so that each fps run explains itself.

1. **c10 B vs c11 B (new defaults)**, 3 runs each, alternating. Report the fps mean and spread,
   and the pressure margin each run reached.
2. **DROP1X correctness**, one run with `EDEN_NX2_C11_DROP1X_CHECK=1` plus screenshots (HUD,
   minimap, mirrors/water, results screen). Expect "dropped" ~32 images / ~460 MiB once; the
   re-created count should be ~0/s in the race (a steady stream means churn: raise AGE or turn
   it off); look at the refusal split. The texture cache device memory should fall by ~460 MiB.
3. **Pressure cap**, `EDEN_NX2_C11_PRESSURE_CAP_MB=1024` vs default, 2 runs each with DROP1X on.
   Measure evictions/s, ASTC re-created/s, hash ms, host-resident MiB, "frames over critical",
   fps and stalls. If 1024 wins without host placement or garbage textures, make it the default.
4. **REPEAT A/B**: `EDEN_NX2_C11_REPEAT=a` with REPEAT_STATS. Report the take rate and
   committer idle ms/s. Turn it on only if the committer idle falls under ~150 ms/s (ceiling)
   or ALT_RANDOM shows a gain.
5. **LOCKRUN A/B**: `EDEN_NX2_C11_LOCKRUN=a` plus one `EDEN_NX2_LOCK_STATS=1` run (waits by lock
   and by waiter/owner: do the guest cores wait on [buffer]/[texture] during runs?).
6. **S3 NOGUARD**: one run with `EDEN_NX2_C11_DISPATCH_CMD_NOGUARD=1` (collision behaviour,
   screenshots). Sequencer wait-committed should fall from ~30 to < 5 ms/s.
7. Where the front's time goes now. ACQFWD found nothing, so the front waits are elsewhere:
   one GPU_PROFILE run ("GPU split drains per second" by reason: puller / writers / position)
   and PULLER_STATS + ACQUIRE_STATS (which sites still wait). The POSFWD shape says the
   "past known" reads cover 24 bytes over a 16-byte release: find which macro (DMA_READ_WHY;
   `macro 0x0`) and what the 8 bytes past the release are.
