# Cycle 11 W4 notes: capture repeat (W4a) + one lock per run (W4b)

Not built (rules). Switches and counters in `src/video_core/mt/c11_commit.h` (new).

## W4a EDEN_NX2_C11_REPEAT (on under LITE; '0' old path, '1', 'a' per capture)
- Sequencer, `CaptureDraw`: builds a key per capture: TIC/TSC table addresses, limits,
  via_header_index, channel (`&memory`), native tiling, then per texture element
  (handle pair, 32 TIC bytes, 32 TSC bytes) and per image element (handle, 32 TIC bytes).
  A direct-mapped table (256 slots by pipeline pointer) keeps the last capture's key and the seq
  its packet gets (`next_seq`). Identical key -> `c11_repeat_delta = seq - previous seq`. A
  capture that goes Serial leaves the pipeline's slot invalid. Not with resolvers.
- Packet: `DrawHeader::pad2` -> `repeat_delta` (u32), `CompactDrawHeader::pad` -> `repeat_delta`
  (u16; delta > 0xffff is not flagged, counted "too far"). Set in `EmitDraw` (2 lines),
  read back in `ParsePacket`. Layout unchanged.
- Committer, `CommitDraw`: sets `GraphicsPipeline::mt_repeat_draw` {seq, repeat_of, layout_gen}
  around Configure (REPEAT != '0', no resolver result).
- `GraphicsPipeline::ConfigureImpl`: with mt_repeat_draw set the TextureMemo is kept for every
  packet draw (fill on the full path; TEX_MEMO's TryReuseGraphicsBindings is still only tried
  with EDEN_NX2_TEX_MEMO). `tex_memo.packet_seq` = the seq of the packet that last set/used it;
  any other Configure of that pipeline (Serial draws, non-MT) resets it to 0. Repeat is taken
  only if `packet_seq == repeat_of` AND channel tag, view epoch, bindings serial (after
  SynchronizeDescriptors), page-table layout gen and sizes match AND
  `TextureCache::TryRepeatGraphicsBindings` (new, next to TryReuseGraphicsBindings) finds every
  image clean (not CpuModified, no aliases, no rescaled image under a blacklisted view). Then:
  no read_handle, no GetSamplerId/FillImageViews/PrepareImage, no PushImageDescriptors (memo
  entries replayed, rescaling words from memo, written images still MarkModification, LRU touch
  once per frame, scale_rating reset like FillImageViews). Buffers (config_stages(1)),
  UpdateRenderTargets, CheckFeedbackLoop unchanged.
- CPU-modified epoch: W3 provides it as `TextureCache::CpuModifiedEpoch()` (not
  `GetCpuModifiedEpoch()` as the brief said; I call the name that exists). I still check the
  CpuModified flag of every memo image (same cache line as the alias check; PrepareImage's
  UnwrittenSparse path sets CpuModified without WriteMemory, so the epoch alone is not enough).
  The epoch is recorded in the memo and counted ("cpu epoch moved") only.
- Not done from the plan: "skip emitting those overlay entries". The overlay must stay complete
  because the committer falls back to the full path (refusals, CHECK) and reads handles/TIC/TSC
  from it; with COMPACT the TIC/TSC entries are already 2-byte slot refs.
- EDEN_NX2_C11_REPEAT_CHECK=1: the repeat-eligible draw runs the full path and compares view ids,
  sampler ids, written images, entries (raw bytes, padding is zeroed), stage ranges, rescaling.
  Mismatches -> counter + first 16 logged as ERROR "C11 repeat check:".
- EDEN_NX2_C11_REPEAT_STATS=1 (or EDEN_NX2_MT_STATS=1): every 10 s "C11 repeat (10 s): N captures
  with textures, M identical (x%), ... taken (refused: seq/state/image) ..." = the go/no-go.
- Cost when on: every packet draw with textures now fills the memo on a non-repeat (vector
  copies, same as TEX_MEMO's fill) and the sequencer builds a key (~10 words per element).

## W4b EDEN_NX2_C11_LOCKRUN (on under LITE; '0', '1', 'a' per run)
- `CommitterLoop`: a run takes buffer+texture mutexes (std::lock) at a draw packet and keeps them
  across following draw packets and Deltas/ComputeDeltas/CbBind/RootTable/Mark entries. Ends:
  any other entry (Command, Serial, compute packet, BatchEnd...), no entry ready (before Idle),
  hook request, EDEN_NX2_C11_LOCKRUN_MAX draws (default 16), a contender (checked before every
  entry), and in `CommitDraw` before a FlushWork that would dispatch/submit
  (`(draw_counter+1) & 31 == 0`, 3 on Android). Inside, CommitDraw's own locks are recursive.
- `SpinRecursiveMutex` (common): `waiters` counter (inc/dec only in LockContended, i.e. the
  contended path) + `Contended()`; `SelfId()` = thread_local cached id when `g_lock_tls_id`
  (EDEN_NX2_C11_LOCKRUN != '0' and MT+LITE, or '1'/'a'), else get_id() as before.
- Off with MT_VERIFY / lockstep; no run while resolvers run (checked per run).
- EDEN_NX2_C11_LOCKRUN_TRACKER=1 (contingent, off): the run also holds tracker_mutex with a
  HeldTrackerNote, so the draws' TrackerScopes skip (needs MT_TRAPLOCK, LITE7_LOCKSKIP). Use only
  if EDEN_NX2_LOCK_STATS shows tracker waits GPUCommit<->GPUBack; the sequencer's captures then
  wait for the run (it ends as soon as they spin).
- Stats (with REPEAT_STATS): "C11 lockrun (10 s): runs, draws/run, ended by max/contended/flush/
  other entry/idle".
- Start line: "Cycle 11 committer: repeat ..., lockrun ..., lock tls id ...".

## Risks / what to look at in the test round
- REPEAT_CHECK mismatches must be 0 (run once with EDEN_NX2_C11_REPEAT_CHECK=1); screenshots for
  stale textures. Watch "refused image" share (CPU-written/aliased textures), and repeat rate:
  if "identical" is low (< ~20%) the feature is not worth its memo-fill cost, set it to 0.
- LOCKRUN: RecordDraw, UpdateDynamicStates and the query segment now run under the cache
  mutexes during a run (as before LOCKSCOPE existed). Deadlock watch: MT_WATCHDOG; any hang with
  the committer in a run -> EDEN_NX2_C11_LOCKRUN=0. Guest trap latency: "ended by contended"
  rate and EDEN_NX2_LOCK_STATS waits on [buffer]/[texture] by guest cores should not rise.
- A/B: C11_REPEAT=a and C11_LOCKRUN=a are safe to alternate (state carried across switches is
  revalidated by seq/epochs).

## Files touched
- new `src/video_core/mt/c11_commit.h`
- `src/common/spin_recursive_mutex.h` (SelfId, waiters/Contended, g_lock_tls_id)
- `vk_mt_pipeline.h`: header fields, members, CaptureDraw, EmitDraw (2 lines: header field),
  ParsePacket (1 line), CommitDraw, CommitterLoop, new BeginRun/EndRun
- `vk_graphics_pipeline.h/.cpp`: MtRepeatDraw, TextureMemo fields, ConfigureImpl
- `texture_cache.h` / `texture_cache_base.h`: TryRepeatGraphicsBindings (new, beside
  TryReuseGraphicsBindings)
