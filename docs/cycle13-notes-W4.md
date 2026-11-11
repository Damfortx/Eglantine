# Cycle 13 W4 notes: 2P memos that survive view alternation + second-committer gate data

Not built, not run (rules). Everything header-only or in W4's own files/functions.

## What is implemented

New header `src/video_core/mt/c13_memo.h`: switches, counters, the generic `C13WaySet<Memo>`
(way 0 = the owner's existing cycle 12 memo, other ways allocated at first use, LRU clock),
the M3 helpers (`C13PromoteWay` / `C13DemoteFirst` / `C13FindWay`), the W4c sampled commit
profile, the start line and the "C13 memo (10 s)" line. Included from `mt/c12_capture.h` and
`mt/c12_commit.h` (so the buffer cache, the pipeline and the sequencer all see it).

- **M1 capture memo ways** (`vk_mt_seq.h` C12Begin -> new `C13BeginWays`, new `C13ViewTag`;
  `vk_mt_pipeline.h` C12Memo gets `c13_tag`, `c13_last_use`; table is `C12MemoSlots * ways`,
  a slot's ways contiguous). Lookup: this pipeline's ways, same view tag first then MRU; per way
  the cycle 12 checks (memory, layout, register key, then C12Validate); at most 2 validations per
  capture. Hit: that way's pass 1, `c12_repeat` = distance to that way's last packet (same view).
  Miss: records into the same-view way only when the tag separates the ways (exactly one way has
  it), else the LRU way of the slot (free first, any pipeline). Throttle per way: a pipeline
  is throttled only when all its ways are in a miss streak (counter on its MRU way); a new way
  inherits the pipeline's MRU streak. CAPTURE_REF is forced off with ways > 1 (one reader slot
  per memo slot; it is unwired anyway).
- **Texture memo by packet ways** (needed for M1 to pay on the committer: repeat_of now names the
  same view's packet, two draws back). `GraphicsPipeline::c13_tex_ways` + `C13SelectTextureWay`
  (vk_graphics_pipeline.cpp/.h): before the repeat path, the way whose packet_seq == repeat_of is
  swapped into `tex_memo`; when none matches, the active memo is kept as the newest way and the
  oldest way's storage is filled. Only for packet draws without TEX_MEMO (`memo_try`), ways are
  invalidated when the memo is off for a draw. All the existing checks (channel, view epoch,
  bindings serial, layout, TryRepeatGraphicsBindings) run unchanged on whichever way.
- **M2 buffer memo ways** (`buffer_cache.h` C12UpdateGraphicsBuffers rewritten, same signature):
  `GraphicsPipeline::c13_buffer_ways` passed through `MT::t_c13_buffer_ways` (set/cleared around
  the call in ConfigureImpl). Ways tried MRU first with the cycle 12 refusal tests (side-effect
  free); a miss counts the MRU way's refusal in the C12 counters and saves the full resolution
  into the LRU way. With 1 way the code path is the cycle 12 one (same tests, same order).
- **M3 ubind ways**: functions not owned by W4 -> exact hunks in `docs/cycle13-hooks-W4.md`
  (H1 member in buffer_cache_base.h, H2 BindGraphicsUniformBuffer, H3 UpdateUniformBuffers).
  W4 builds without them (only the ubind counters stay 0).
- **W4c commit profile**: `C13ProfBegin/Mark/End` in `CommitDraw` (vk_mt_pipeline.h) and
  `ConfigureImpl` (vk_graphics_pipeline.cpp); every 32nd packet, 12 phases (setup, uploads,
  pre-configure [validation + ConfigureDrawPipeline + locks], texture resolve, buffer resolve,
  desc acquire, stage bind, push descriptors, render targets, configure draw, draw record,
  tail). Line "C13 commit profile (10 s)": us per draw per phase, est. ms/s, resolve / encode /
  stage bind / other shares and "gate: encode > / <= 40%". Complements perfthreads on GPUCommit.

## Switches (all read once)

| switch | default | meaning |
|---|---|---|
| `EDEN_NX2_C13_MEMO_WAYS` | `2` under MT lite, else 1 | `0`/`1` cycle 12 paths exactly, `2`, `4`, `a` = 2 ways / 1 way by Nx2AltOn (per capture / per draw; storage shared, the 1-way half uses way 0) |
| `EDEN_NX2_C13_MEMO_TAG` | `1` | view tag for M1: `1` first uniform binding of the first stage with UBOs, `h` hash of all that stage's bindings, `0` none (pure MRU/LRU) |
| `EDEN_NX2_C13_MEMO_CHECK=1` | off | turns on `C12_CAPTURE_REPEAT_CHECK` and `C12_COMMIT_REPEAT_CHECK` (-> C11 repeat check): every way's hits compared with the full path; also turns on C13 stats |
| `EDEN_NX2_C13_STATS=1` | off | "C13 memo (10 s)" line (committer) and the commit profile |
| `EDEN_NX2_C13_COMMIT_PROFILE` | = C13_STATS | `1`/`0` force the W4c profile on/off |

Start line: "Cycle 13 memo: ways N (mode m), tag t, check, stats, commit profile".

## Risks

- A wrong way is a miss (every way is validated as the single memo was); CHECK must show 0
  "C12 capture repeat check" / "C12 commit repeat check" / "C11 repeat check" mismatches.
- More sequencer work per miss (up to 2 validations, tag hash, a sort of <= 4) and 2x/4x memo
  memory (C12Memo vectors: up to ~4 x 1024 entries). Watch C12 "validate ms" and seq ms/draw.
- If the default tag (`1`) does not tell views apart (e.g. the first binding is a driver cbuf),
  replacement falls back to LRU (designed to be safe: same-view replacement only when exactly
  one way carries the tag). The C13 line's "same view tag" hits and "victims: same view" show it;
  try `EDEN_NX2_C13_MEMO_TAG=h`.
- Texture ways: a TextureMemo swap per miss (vector moves, cheap). TEX_MEMO (`memo_try`) draws
  keep the 1-way behaviour.
- The profile's marks cost one thread_local bool read when not sampling; sampled draws pay ~13
  steady_clock reads. The helper-forked path's join wait lands in "texture resolve".
- Lambdas/locals named to avoid MSVC C4456-C4459 shadowing; explicit casts for C4244. Not
  compiled: the coordinator's build is the first check (watch vk_mt_seq.h C13BeginWays,
  buffer_cache.h C12UpdateGraphicsBuffers, c13_memo.h).

## What to look at in the test round

- run 2 (c13-ss-chk): C12/C11 check mismatches 0 with ways 2; "C13 memo" line: capture "other
  way" hits > 0 and C12 capture hits 44 -> ~55 %; textures "other way" > 0; buffers "other way"
  share; C12 buffer "taken" 3.6 -> 12-15 %.
- run 7 (c13-ss-1): "C13 commit profile" encode share -> the W4c gate (with perfthreads).
- run 10 (MEMO_WAYS=a): per-window hit rates and seq/committer ms per draw, on vs off; 1P must
  be ~0 change.
