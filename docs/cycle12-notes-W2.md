# Cycle 12 W2 notes: committer whole-draw repeat

Not built (agent rules). Header-only; no new .cpp, no CMakeLists change. Read with
docs/cycle12-hooks-W2.md.

## What it does
EDEN_NX2_C12_COMMIT_REPEAT (mt/c12_commit.h), on by default under MT + LITE, `0` off (cycle 11
path), `1` on, `a` decided per draw at commit. Four pieces, each `=0` alone:

| piece | switch | what is skipped | validity |
|---|---|---|---|
| texture half | EDEN_NX2_C12_TEXTURE_REPEAT | C11 REPEAT (FillImageViews / TIC-TSC reads / descriptor build from the TextureMemo): `C11RepeatMode()` now returns `1` under C12 unless EDEN_NX2_C11_REPEAT=0. Also takes W3's `c12_repeat` (with `C12SameReads`) as `repeat_of` when the C11 key did not flag the draw | unchanged cycle 11 checks (memo set by that packet, channel, view epoch, bindings serial, layout gen, clean images) |
| buffer half | EDEN_NX2_C12_BUFFER_REPEAT | `UpdateGraphicsBuffers`' uniform and storage resolution (FindBuffer, ResolveStorageBinding, BufferOperations loop): `BufferCache::C12UpdateGraphicsBuffers` with the pipeline's `C12GraphicsBufferMemo` restores the ids; index/vertex updates still run (own memos) | same channel, memory manager, odd layout generation, `buffer_deletion_epoch`, new `c12_buffer_epoch`; equal enabled/written masks; every uniform (device addr, size, id if already set) and storage (device addr, GPU addr, size) binding equal; no texture buffers, transform feedback, indirect, accurate GPU fences; falls back to the full path if the index/vertex update created/deleted a buffer |
| uniform bind memo | EDEN_NX2_C12_UBIND_MEMO | `GpuToCpuAddress` in BindGraphicsUniformBuffer (per slot, same GPU addr under the same odd layout generation) and `FindBuffer` in UpdateUniformBuffers (per slot, same device addr+size under both epochs). Applies to every draw, not only repeats | committer thread only (`OnCommitter`); translation and resolution kept separately, resolution only read under the cache mutex |
| index memo | EDEN_NX2_C12_INDEX_MEMO | the existing EDEN_NX2_INDEX_MEMO turned on (committer) | existing (deletion epoch, layout gen, channel, start addr, size <= kept) |

The new epoch `c12_buffer_epoch` is bumped in `ChangeRegister` (every page table insert/remove =
every buffer creation and deletion). Deviation from the plan: it is NOT bumped by CPU writes,
uploads or MarkWrittenBuffer, because none of those changes what FindBuffer returns (without
accurate GPU fences, where all c12 memos are off) and on the committer `SynchronizeBuffer` is a
no-op (the packet carries the bytes, MtApplyUploads applies them). Everything that depends on
writes still runs every draw: BindHostStageBuffers (streamed-uniform decision from the packet,
descriptor entries, MarkUsage, MarkWrittenBuffer), uploads, render targets.

Not done (reason): replaying the buffer descriptor entries / skipping BindHostStageBuffers
("bind repeat"): on the committer its per-binding work is TouchBuffer + MarkUsage + one
descriptor entry (SynchronizeBuffer returns at once), and streamed uniforms change the offset
every draw, so the gain is small against a real staleness risk (written storage, LRU, usage
tracking). Render targets / framebuffer: UpdateRenderTargets keeps its own dirty-flag path;
untouched.

## Files touched (W2's)
- new `src/video_core/mt/c12_commit.h`: switches, counters, start line, 10 s line, W3-flag readers.
- `mt/c11_commit.h`: C11RepeatMode/Check/Stats follow C12; new C11RepeatExplicit().
- `buffer_cache_base.h`: C12GraphicsBufferMemo, C12UpdateGraphicsBuffers decl, c12_buffer_epoch,
  c12_ubind_memo, helper decls.
- `buffer_cache.h`: BindGraphicsUniformBuffer, UpdateUniformBuffers, UpdateIndexBuffer (use_memo
  line + hit counter), ChangeRegister (epoch), new C12UpdateGraphicsBuffers / C12SameBindings /
  C12ApplyBindings / C12SaveGraphicsBuffers / C12CheckGraphicsBuffers.
- `vk_graphics_pipeline.h`: MtRepeatDraw gets `c12_buffers`, `textures`; pipeline member
  `c12_buffer_memo`.
- `vk_graphics_pipeline.cpp` ConfigureImpl: `repeat_draw` is null when only the buffer half is on
  for the draw; both UpdateGraphicsBuffers calls go through `update_graphics_buffers`.
- `vk_mt_pipeline.h`: CommitDraw (per-draw C12 decision, W3 flag, MtRepeatDraw fields,
  C12CommitMaybeLog), CommitterLoop (C12CommitLogStart), ParsePacket (C12CopyRepeatFlag).

## Stats / checks
- Start line: `Cycle 12 committer: commit repeat on (texture .., buffer .., ubind memo .., index memo ..), check .., stats ..`.
- EDEN_NX2_C12_STATS=1 (or EDEN_NX2_MT_STATS=1): `C12 commit repeat (10 s): ...` with draws,
  W3-flagged, buffer tries/taken/refusal reasons/fallback/saved, ubind translations and
  resolutions reused, index memo hits, check/mismatches. The texture half reports in the existing
  `C11 repeat (10 s)` line (turned on by C12 stats).
- EDEN_NX2_C12_COMMIT_REPEAT_CHECK=1: buffer half re-resolves every taken draw with the full path
  and compares ids / addresses / sizes / segments (the full result is used); ubind hits re-translate
  and re-FindBuffer; also turns on C11_REPEAT_CHECK (views, samplers, descriptor entries,
  rescaling). ERROR `C12 commit repeat check: ...`, count in the stats line. Must be 0.

## Risks / what to look at in the test round
- Compile risks (not built): the `requires` checks in templates (`C12SameAsDelta`), designated
  initializers of MtRepeatDraw (order seq, repeat_of, layout_gen, c12_buffers, textures),
  `ForEachEnabledBit` lambdas in a const member.
- C12 `a` A/B: the off half runs without the packet texture memo (like the c11 default) but the
  uniform-bind and index memos are decided per call, so they alternate too.
- Stale id risk: a FindBuffer result that changes without a page table change. None known;
  CHECK must stay 0 over a whole race (1P and 2P).
- Expected size: texture half 25 % of textured draws (cycle 11 numbers); buffer half most draws
  of pipelines without texture buffers once bindings repeat; ubind memo should hit most binds
  (MKW re-binds the same constant buffers). Look at the committer idle ms/s and the C12 line's
  taken % next to the plan's §1 numbers; in 1P the committer is not the ceiling (expect ~0 fps),
  the 2P committer ceiling is where it should show.
- Screens: HUD, minimap, water, both 2P views (stale uniform = wrong transforms/colours).
