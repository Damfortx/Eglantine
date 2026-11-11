# MT lite, round 6: GPU params v2, front register dedup, front drains, front split study, split-screen bench

Branch mkw-mt-lite from 6523cf9. State (docs/mt-lite-round1.md, docs/gpuparam-plan.md): MT lite is
+9-15% over MT unset at 2x and correct; the FRONT thread is the limit: own work ~600 ms/s (push 304,
macro 162, front method 130) plus split drains ~290 ms/s (writers(commands) 108 ms = waiting for
the culling compute that writes macro-40 parameters; position 109 ms; puller 40 ms). 10.2M 3D
register writes/s, 5.5M same value. GPUPARAM v1 (EDEN_NX2_MT_LITE_GPUPARAM, off) removes the
writers(commands) wait correctly but is slower (-8.6 +/- 4.7 fps): one indirect draw per call
(~10.5k/s, 76% culled ones the CPU used to skip) plus a per-call buffer copy cb+0x640/0x644 (a
Serial on the sequencer).

Method: all pieces implemented in parallel by sub-agents on disjoint files/functions of the same
worktree (sub-agents do not build); the coordinator builds, integrates, commits each piece; then
ONE test round (docs/mt-pending-runtime-checks.md, "Round 6"). Every piece has its own switch
(Tegra::Nx2EnvMode / Nx2Switch); MT unset unchanged unless a switch says otherwise. No new .cpp
files, no CMakeLists.txt edits.

## File / function ownership

Shared .cpp files are split by FUNCTION. Agents edit with small, local Edit calls (re-read before
editing: other agents edit other functions of the same file concurrently), never rewrite a whole
shared file. nx2.h is read-only for everyone (switches go to each piece's own header).

| piece | owns |
|---|---|
| W1 GPUPARAM v2 | mt/gpuparam.h, new mt/gpuparam2.h; maxwell_3d.cpp: GpuParamCall, GpuParamDraw, GpuParamConstantWords, new GpuParam* functions appended at the end; maxwell_3d.h: GpuParam declarations, EngineHint/HLEReplacementAttributeType; dma_pusher.cpp: TryGpuParamCall, the gpuparam block of Step, end of DispatchCalls; gpu_split.h: the new "front hold" member + one-line hold-break calls at the top of the emission/wait functions (Method, MultiMethod, MacroWrite, StateWrite, Defer, DeferFunction, Publish, Drain*, Wait*, Run*); shader_environment.h/.cpp, shader_recompiler/** (cbuf replacement), fixed_pipeline_state.*, vk_pipeline_cache.*, gl_shader_cache.cpp (key only), draw_manager.*, vk_rasterizer.cpp indirect draw path, buffer_cache indirect path, mt draw packet / capture files only where engine_state / replacement must travel with the draw |
| W2 REGDEDUP | new mt/regdedup.h; dma_pusher.cpp: CallMethod, CallMultiMethod, ProcessCommands; maxwell_3d.cpp: FrontWrite, FrontWriteRun, FrontCallMethod, FrontCallMultiMethod, FrontRunMacro, CallMethod, CallMultiMethod, ProcessMethodCall; macro JIT/interpreter write callbacks (macro*.cpp/h) |
| W3 drains | new mt/lite6_drain_switches.h; puller.cpp, gpu_split.cpp, gpu_split.h (everything except W1's hold lines), maxwell_3d.cpp: FrontDmaRead, FrontNoteWrites; kepler_compute.cpp, maxwell_dma.cpp, kepler_memory.cpp |
| W4 FRONT2 | new mt/front2.h; design section of this doc; code hooks only if a clean design exists (DispatchCalls entry, single call site) |
| W5 split-screen bench | $HOME/src/eglantine-buildenv/tools/splitscreen/ only (README + scripts); no src edits |
| coordinator | this doc, build, integration fixes, commits, docs/mt-pending-runtime-checks.md |

## W1 GPUPARAM v2 (EDEN_NX2_MT_LITE_GPUPARAM2, on under MT_LITE; EDEN_NX2_GPUPARAM2 without MT)

Keeps v1's front side (TryGpuParamCall, classifier, zero-word call, CHECK) and changes what the
back draws:
1. **No constant buffer copy: draw parameters as shader built-ins.** The engine already has the
   mechanism the HLE indirect macros use: Maxwell3D::engine_state = OnHLEMacro + replace_table
   (bank 0, 0x640 -> BaseVertex, 0x644 -> BaseInstance) -> FixedPipelineState::app_stage (a
   separate pipeline variant) -> GraphicsEnvironment::GetReplaceConstBuffer ->
   constant_propagation_pass replaces those cbuf reads with the BaseVertex/BaseInstance
   built-ins (SPIR-V DrawParameters). v2 draws through it when the CbPlan is exactly
   {pre +0x640 = base vertex word, pre +0x644 = base instance word, post constants} and the
   selected constant buffer (const_buffer.address) is the vertex stage's bank (the replacement
   key uses that bank). The cb words keep the zero-call values in memory (no copy, no Serial, no
   GPU-modified marking). Thread safety: the replacement table must not be read live from the
   back's Maxwell3D by a pipeline built later on another thread: v2 uses a dedicated hint value
   with a FIXED table (environment derives the replacements from the hint captured in the
   pipeline key), not the mutable replace_table. Pipelines whose non-vertex stages read the
   replaced words cannot use built-ins: detected at pipeline build (stats/log) and such calls
   fall back to v1's copy (or are not grouped).
2. **Groups: one multi-draw for consecutive calls.** The list dump shows macro-40 records in runs
   of 4 contiguous 20-byte records (separate GPFIFO entries, CPU entries of 8-9 words between).
   The front holds the deferred draw instead of emitting it ("front hold" in GpuSplit): the next
   deferred call joins when it is the same macro/kind/p0/cb plan and its records follow the
   group's last record (address + 20), up to a cap; anything else the front would send to the
   back (any split emission or wait, including state writes, macro writes other than the joining
   call's own zero-call writes, puller methods, drains, publish at the end of a submission)
   first releases the hold: one Defer -> GpuParamDraw with max_draw_counts = N, stride 20,
   vkCmdDrawIndexedIndirect(drawCount N) (the GPU skips the culled records). Same-value writes
   never reach the split (skipped on the front; W2 makes that earlier), so they do not break a
   group. Stats count group sizes and the first method that broke each group (histogram), so
   the next round knows what sits between calls if groups stay small.
3. Correctness: register side effects = the zero-word call (unchanged); ordering against the
   compute writer as v1 (compute -> graphics barrier covers DRAW_INDIRECT); the held draw only
   moves later past emissions that are the joining calls' own zero-call writes. CHECK mode
   (EDEN_NX2_GPUPARAM_CHECK=1) as v1 per call, plus: group records' contiguity, and the
   constants the shader would see (record words 3/4 = the cb words the macro writes) compared
   with the macro's cb words.

## W2 REGDEDUP (EDEN_NX2_MT_LITE_REGDEDUP, on under MT_LITE; =1 also without MT)

FrontWrite already skips same-value non-executable writes, after DmaPusher dispatch, Front(),
the shadow-RAM bookkeeping and profile counting. W2 filters them at the earliest point: in
DmaPusher (CallMethod / CallMultiMethod / ProcessCommands batch path) for a 3D subchannel and in
the front macro's register-write callback, using a per-engine table of "latch-only" registers
(not in execution_mask, not macro/shadow-control/cb_data/inline/instance-loop/draw methods, not
handled outside the draw manager), with the shadow RAM in Track/TrackWithFilter and the shadow
word equal too (so skipping changes neither regs nor shadow), never in Replay. Without the
split (MT unset, =1): the same filter before the method sink. Exact behaviour preserved for
executable/trigger methods. Counters: dedup'd writes/s.

## W3 front drains (EDEN_NX2_MT_LITE_<X>, each on under MT_LITE)

Position waits (~109 ms/s; only caller Maxwell3D::FrontDmaRead) and puller drains (~40 ms/s):
reasons from the code and iteration 4/5 notes; candidates: overlay coverage for more read
shapes, sequencer-side early answers (the sequencer publishes the value a noted write will
leave when it captures it, so the front waits for the sequencer only, not the committer), a
mark right after the awaited entry (the wait ends at that entry, not at the backlog), and
whatever the reasons breakdown (new stats) points to. Details: the W3 section added below.

## W4 FRONT2 (EDEN_NX2_MT_LITE_FRONT2, default off)

Two-stage front (parse thread: GPFIFO/DmaPusher command decoding; execute thread: puller, macro,
3D methods) only if a clean design exists; else documented why not (section below).

## W5 split-screen benchmark

TAS scripts + instructions for a repeatable 2-player VS race in MKW (tools/splitscreen/).

---------------------------------------------------------------------------------------------
# Implementation notes (round 6, compiled, NOT run)

Integration note: duplicated sub-agents collided on some files during the round (W1a, W1b, W2,
W3, W5 each ran twice for a while); the surviving versions were reconciled, and the tree as
committed builds (build/pgo, rc=0). W3 was redone by a single agent after the collision.

## W1 GPUPARAM v2 (front/back draw side; mt/gpuparam2.h, mt/gpuparam.h, maxwell_3d.cpp GpuParam*, gpu_split.h hold)
Switches: EDEN_NX2_MT_LITE_GPUPARAM2=0/1/a (default ON under EDEN_NX2_MT_LITE=1 with EDEN_NX2_MT);
EDEN_NX2_GPUPARAM2=0/1/a (default off, no MT back: copy-free only, no groups);
EDEN_NX2_GPUPARAM2_GROUP=<max> (default 64, 1 = no groups). v2 on implies v1's deferral path
(GP::On = v1 || v2). v1 switches unchanged (off).
- Copy-free: an IndexedSingle call whose CbPlan is BuiltinPlan (pre +0x640=word3, +0x644=word4, post
  constants at both) gets Call::builtins. Back (GpuParamDraw): GpuParamVertexBank() = bank i of
  state.shader_stages[0] (vertex) enabled with address == regs.const_buffer.Address(); bank 0 ->
  engine_state = OnGpuParamDraw, gpuparam_cb_bank = 0, indirect draw, engine_state = None; no
  FlushCaching, no copies. No bank / bank != 0 -> v1 copy path per record (counted). Other plans or
  multi calls: v1 (copies).
- Groups (split front + MT back only): GpuSplit "front hold" (hold_fn/hold_ctx; ReleaseFrontHold at
  the top of Method, MultiMethod, MacroWrite, StateWrite, Defer, DeferFunction, Drain, DrainForRead,
  Run, RunFlush, WaitForPosition, WaitSequencer, WaitForWriters, WaitForListWriters; end of
  DmaPusher::DispatchCalls; Stop drops it). Publish has no release on purpose (it runs inside
  Reserve). A copy-free call whose verdict is group_safe (cached, state-independent, zero call draws
  nothing and writes only non-executable state, cb_data, const_buffer.offset, index_buffer.first)
  holds its draw. The next call joins when the hold is still ours, same macro, same p0, SamePlan,
  records == first + N*20, N < cap. Its zero-word call runs with the hold "tolerant" (only
  StateWrite/MacroWrite of those registers pass; anything else releases first, counted "zero-call
  breaks"). Release = one Defer -> GpuParamDraw, max_draws N, stride 20, no count buffer
  (vkCmdDrawIndexedIndirect drawCount N; multiDrawIndirect is mandatory; the MT packet carries the
  full IndirectParams). Full group releases at once.
- Stats appended to "GPU params in 10 s": "; v2: N calls, N copy-free, v1 copy: plan/multi, back: N
  built-in draws (N records), fallback to copies ..., groups N (N records; sizes 1:..2:..3:..4:..
  5-8:..9-16:..17+:..), released by: <kinds>, methods: <top 6>, not joined: other/macro/begin/plan/
  address/unsafe, zero-call breaks N; v2 check N compared, N mismatched".
- CHECK (EDEN_NX2_GPUPARAM_CHECK=1): v1's per-call compare + BuiltinsMatch (the macro's pre-draw cb
  words at 0x640/0x644 equal record words 3/4). CHECK makes groups size 1 (its writer wait releases).
- Risk: held draws go out after the joining calls' cb/offset writes: correct only because the
  built-ins don't read cb+0x640/0x644 (pipelines whose non-vertex stages read them are logged, below).

## W1 shader side (shader_environment, recompiler, FixedPipelineState, pipeline caches)
- Hint EngineHint::OnGpuParamDraw (=2) + Maxwell3D::gpuparam_cb_bank. FIXED table cbuf[bank][0x640]
  -> BaseVertex, [0x644] -> BaseInstance, vertex stages (VertexA/VertexB) only, never the mutable
  replace_table. Key: FixedPipelineState::gpuparam_bank (raw2 bits 20-24, 0 without the hint: old
  keys/disk caches unchanged, no CACHE_VERSION bump); GL key raw bits 14-18.
- Thread safety: the pipeline lookup runs in PrepareDraw on the sequencer at the draw; new pipelines
  are built via RunOnCommitter (sequencer blocked) where the environment is constructed; the runtime
  build also re-derives the hint from the key; disk/boot builds replay serialized replacements.
- Non-vertex stages, sub-word/LDC.64 reads of 0x640..0x647 and dynamic-offset reads of that bank keep
  memory reads (which hold 0 under v2): counted and logged
  WARNING "GPU params v2: pipeline <hash> stage <n> reads the draw parameter words from memory".
  INFO "GPU params v2: pipeline <hash> built with draw parameter built-ins (cb<N>) ...".
- SPIR-V: DrawParameters capability now emitted whenever BaseVertex/BaseInstance/DrawID is loaded
  (was missing when VertexId was not loaded; also affects HLE-macro pipelines: validity fix).
- Not done: per-pipeline flag for the back to fall back to copies; LDC.64 folding at 0x640.

## W2 REGDEDUP (mt/regdedup.h; dma_pusher.cpp CallMethod/ProcessCommands; maxwell_3d.cpp Front*)
EDEN_NX2_MT_LITE_REGDEDUP=0/1/a, ON under MT_LITE; =1 also without MT/split. Start line
"MT lite 6: regdedup on". Latch table (built once; "Register dedup: N latch-only 3D registers"):
methods 0x40..0xDFF not executable, not FrontHandledOutsideDrawManager, not instance-loop, not
0x157/0x158. Drop when the register (and in Track/TrackWithFilter the shadow word) already holds the
value, skip_same set (front) / SkipSameValueWrites && !front_macros (no split); never in Replay.
Hooks: DmaPusher::CallMethod (both front branches, before FrontCallMethod), FrontWriteRun loops,
FrontRunMacro emit callback; FrontCallMultiMethod trims leading same-value words (whole run only
when !draw_pending); no-split: before the method sink (sink tracker; anything pushed by others
disables dropping until the sink empties). Stats (GPU_PROFILE or CHECK): "Register dedup: N
dropped/s early (dma), M (macro), S (sink), K (multi); check: ...". CHECK
EDEN_NX2_MT_LITE_REGDEDUP_CHECK=1: nothing dropped early; predictions verified ("would have emitted"
must be 0).
**Bug fix (unconditional):** FrontRunMacro's FCALL fast_same skipped same-value writes to 0x157/0x158
(MME_DMA_READ / _FIFOED, not in execution_mask) -> a repeated macro DMA read with the same count
skipped FrontDmaRead (stale data RAM / empty FIFO) under MT_LITE whenever profiling was off since
iteration 3. Expect "writers (dma read)" drains to rise back somewhat.

## W3 front drains (mt/lite6_drain_switches.h; gpu_split.*, puller.cpp, maxwell_3d.cpp FrontDmaRead/FrontNoteWrites)
All on under MT_LITE (=0 off; =1 without LITE). Start line "MT lite 6: reportsize true, overlay64
true, seqpos true, i2mpos true, pendacq true, pendpoll true, pendcheck false".
| id | switch | change |
|---|---|---|
| D1 | REPORTSIZE | short_query 3D reports noted as 4 bytes, not 16 |
| D2 | OVERLAY64 | direct payload releases (sequencer-written u64 payload, word1 = 0) flagged WriteDirect64: overlay knows words 0 and 1, reads within 8 bytes need no wait |
| D3 | SEQPOS | position waits whose unsettled writes are all sequencer-written wait for the sequencer only (settle_position = newest non-direct write; no committer wait when 0); not when writer-known notes overlap |
| D4 | I2MPOS | correctness: I2M note stays open from launch_dma to the next launch, its position moves with each inline_data entry (incl. FrontCallMultiMethod runs, never noted before); drains keep it |
| D5 | PENDACQ | unpredicted trigger acquire whose pending noted writes all have known values that don't meet it (no adds, no valueless/writer notes, no open I2M, not QMD/DMA target, page never GPU-written, memory doesn't meet it) suspends without the drain |
| D6 | PENDPOLL | resume polls of such acquires: latch every poll, drain every 8th |
| - | PENDCHECK=1 | D5/D6 skips drain anyway and count "pendacq would meet" |
Stats (GPU_PROFILE/PULLER_STATS): "MT lite 6 position waits per second (newest noted write): |
payload-seq | fence | counter | i2m | unknown | sequencer-only | settle | overlay reads, refused ...";
PULLER_STATS sites "pendacq nodrain", "pendpoll lazy", "pendacq would meet". Not done: SEQWRITERS
(QMD release words are written by committer fence callbacks), RESUMEPOS (D6 covers it), counter
reports (no cheap answer).

## W4 FRONT2: not implemented (decision b); census only (mt/front2.h, hook at DispatchCalls entry)
"push 304 ms" is mostly execution, not parsing: under FBATCH, Maxwell3D::FrontWriteRun (most of the
10.2M register writes/s) runs without its own profile zone and is counted as push; CPU command lists
are already copied at submit (nvhost_gpu SubmitGPFIFO -> submitted_words). Movable parse work is
estimated at ~20-40 ms/s; a per-run hand-off ring costs ~15-25 ms/s; barriers (GPU-written entries
~11k/s, semaphore acquires that suspend mid-list and force re-snapshots, GPUPARAM decisions in Step,
channel switches, subchannel binds for FBATCH eligibility) ~15-20k/s => expected net -5%..+3% with
real ordering risk. Prerequisites to revisit: GPUPARAM v2 removing most GPU-written list reads, a
profile zone for FrontWriteRun, and the census: EDEN_NX2_FRONT2_STATS=1 logs "FRONT2 census" per
channel every 10 s (lists, entries, headers, records, data/puller words, GPU-written/no-snapshot
entries, barriers, words between barriers histogram, dry decode us/s). Revisit only if barriers
< ~2k/s and dry decode > ~60 ms/s. EDEN_NX2_MT_LITE_FRONT2=1 only logs a warning.

## W5 split-screen bench ($HOME/src/eglantine-buildenv/tools/splitscreen/)
eden-cli cannot play TAS scripts (only the Qt frontend drives Tas::UpdateThread / Ctrl+F5), so the
default route uses eden-cli's existing EDEN_NX2_KEY_SCRIPT (key presses by present count) with a
config copy where player 2 is a second Pro Controller on its own keyboard keys (mkconfig.py; the
user's sdl2-config.ini is only read; controller applet HLE auto-accepts). timeline.txt -> gen.py ->
keys script + TAS scripts; splitscreen.sh <eden-cli> <secs> <tag> (demo.sh conventions); fps.py
measures from the race marker (F12 = "Key script: press key 69"). tas-autostart.patch (not applied)
adds EDEN_NX2_TAS_AUTOSTART to eden-cli for INPUT=tas. Route: title -> L+R -> Multiplayer -> 2P ->
Grand Prix (fewer screens than VS; note in timeline for VS) -> mash A -> hold A. Timings are guesses:
one manual calibration (INPUT=record, F12 at "GO", rec2timeline.py) is required first.
