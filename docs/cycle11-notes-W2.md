# Cycle 11 W2 "no back bubbles": notes

Plan: docs/cycle11-plan.md #2. Not built, not run (rules). Switch header: src/video_core/mt/c11_serial.h
(also holds the S2 audit list). All three switches are on by default under EDEN_NX2_MT=1
EDEN_NX2_MT_LITE=1; `=0` gives the pre-cycle-11 path, `=1` turns them on without LITE.
Startup line (once, first bind or collision dispatch): `Cycle 11 serial: samechan .., chancmd .., dispatchcmd .. (sequencer write guard ..)`.
Stats line (EDEN_NX2_MT_STATS, every 10 s, after "MT lite 7 capture/s"):
`C11 serial/s: channel binds same X (+Y back), command Z, serial: <refusal> N... | mirrors created, rebind register diffs | collision packets, serial | publish guard checks, waits (ms)`.

## S1 EDEN_NX2_C11_SAMECHAN
RasterizerVulkan::BindChannel to the channel already bound (same 3D engine and MemoryManager) returns
at once: no Serial, no cache rebinds, no state_tracker.InvalidateState(). Checked on the sequencer
(Nx2DrawPipeline::SeqBindChannel, vk_mt_seq.h) and on the committer/single-thread path (BindChannel).
Note: GPU::Impl::BindChannel (gpu.cpp) already skips binds of the bound channel id, so "same" is
probably ~0 and the ~188/s are real switches: that is what S2 is for. The line answers plan §5 item 5.

## S2 EDEN_NX2_C11_CHANCMD
A real switch on the sequencer:
1. CloseBatch + EmitDeltas (the old channel's register writes go to its mirror first).
2. Sequencer switches at once: rasterizer ChannelSetupCaches::BindToChannel (maxwell3d, kepler_compute,
   gpu_memory, program_id) and pipeline_cache.BindToChannel (pipeline lookups run here).
3. Snapshot of the new channel's real engines (regs, state, root tables, draw state, native_turing,
   compute regs + QMD) into a heap struct; the real engine's dirty flags all set (what the Serial's
   InvalidateState did to them: RefreshStages must re-read the programs); mt_record set,
   mt_written / cb binds / root ops cleared (BindCurrentChannel's real-engine half, now done by
   the engines' owner).
4. Command(Kind::Channel) -> committer C11CommitChannel: texture/buffer cache BindToChannel (both
   locks), query cache BindToChannel (with overrides off, as the Serial), bound_channel_id, mirrors
   from the snapshot (create or diff-apply), pin, channel_gen, state tracker flags -> mirror +
   InvalidateState.
Pin (mt/engine_ptr.h): EngineOverrides<T>::pinned; on the committer every non-null ThreadEnginePtr
resolves to the current channel's mirror while overrides are active (a Serial clears `count`, so it
still runs against the real engines). Needed because the rasterizer's stored pointers (e.g.
ConfigureDrawPipeline's SetEngine(maxwell3d.Real())) may already name the next channel while the
committer still records the previous one. BindCurrentChannel also sets the pin when CHANCMD is on.
Refused -> old Serial path (counted by reason): CHANCMD off, resolvers running, no channel bound /
no engines, different MemoryManager (rasterizer.gpu_memory would change under the committer),
different program id. ReleaseChannel stays Serial.
Kind::Channel added to WritesGuestMemory=false and NoteAsyncWork's no-op kinds (no guest write, no
GPU work).

## S3 EDEN_NX2_C11_DISPATCH_CMD
Collision-map program 0x6e0ae200ee9aa220 goes through CaptureCompute + EmitCompute like any compute
packet. CaptureCompute(pipeline, native_key) also captures the CB0+0x38 output handle and its TIC
entry; DispatchComputeBody's publish now reads the handle with MT::ReadCaptured (identical outside
packets: t_capture null) and VisitImageView reads the TIC through the overlay, so the publish runs
inside the packet's commit (texture cache locks held, as before) on the captured state.
After the packet: NoteCommitterWrite(bulk) (front reads after this entry wait for the committer),
NoteAsyncWork(Dispatch), and the **write guard**: g_c11_publish_position = stream end; the
sequencer's next guest-memory write (BeforeWrite hook: payload reports written at once, I2M, DMA...)
first WaitCommitted until the publish is done. Reason: the old Serial ordered every later sequencer
write (e.g. a non-fence semaphore release the CPU polls before reading the height map) after the
publish; without the guard the release could become visible before the map.
EDEN_NX2_C11_DISPATCH_CMD_NOGUARD=1 (off, not LITE): the plan's version without the guard.

## Files touched
- new src/video_core/mt/c11_serial.h
- src/video_core/mt/engine_ptr.h (pinned view)
- src/video_core/renderer_vulkan/vk_mt_seq.h (SeqBindChannel, C11CommitChannel, includes)
- src/video_core/renderer_vulkan/vk_mt_pipeline.h: include; WritesGuestMemory and NoteAsyncWork
  (+Channel); declarations of SeqBindChannel / C11CommitChannel / C11PublishGuard / CaptureCompute's
  native_key; BeforeWrite (guard call, 3 lines); BindCurrentChannel (pin set/clear); CaptureCompute
  (collision handle capture); SeqDispatch; C11PublishGuard; LogStats (C11 line).
- src/video_core/renderer_vulkan/vk_rasterizer.cpp: BindChannel (sequencer -> SeqBindChannel, S1 on
  the other path); DispatchComputeBody publish lambda, one line (ReadCaptured). BeforeWrite,
  BindCurrentChannel, NoteAsyncWork and DispatchComputeBody are in no agent's list; edits are minimal.
- No hooks needed in other agents' files. vk_state_tracker / cache BindToChannel functions unchanged.

## Risks / what to look at in the test round
- S2 is structural (not alternated): compare whole runs; watch MT_VERIFY / MT_WATCHDOG, rendering
  after scene changes (menus <-> race), the "C11 serial/s" line: command ~= old "serial channel"
  rate, refusals ~0, rebind register diffs 0 (non-zero = engine state changed while unbound; it is
  applied, but it means the snapshot assumption is loose). MT stats "serial ... ms wait" should
  fall from ~48 toward <10 ms/s and "channel" move from serial to commands.
- Formal data race (benign on x86): the committer reads the rasterizer's ThreadEnginePtr pointer
  word while the sequencer rewrites it at a bind (the pin makes the value irrelevant); the
  sequencer reads buffer_cache.channel_state (MtSkipCacheSize) while the committer rebinds it.
- If a crash/garbage appears only with CHANCMD: first suspect a committer path reading a
  sequencer-switched pointer other than those audited (rasterizer.channel_state, pipeline_cache
  state on the committer outside Serials). EDEN_NX2_C11_CHANCMD=0 isolates it.
- W4 LOCKRUN: C11CommitChannel takes both cache mutexes (recursive spin mutexes; fine if a run
  holds them). W4 REPEAT keys include the channel; channel_gen is still bumped per bind.
- S3: "publish guard waits" ~ collision packets means the next sequencer write follows the
  dispatch closely and S3 gains little; NOGUARD=1 then removes it (check water/collision
  behaviour + screenshots). Overlay misses for the publish read would show as capture misses.
- S3 does not cover sequencer guest READS of the published image or writes outside MemoryManager
  (none known).
