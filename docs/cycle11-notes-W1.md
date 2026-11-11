# Cycle 11 W1 "front forwarding": notes

Not built, not run (rules). Start line: `Cycle 11 front: acqfwd ... listrange ...` (once, from
GpuSplit::MaybeStart). Stats line every 10 s: `C11 front per 10 s: ...` with EDEN_NX2_C11_STATS=1,
EDEN_NX2_GPU_PROFILE, EDEN_NX2_PULLER_STATS or a check switch (logged from DmaPusher::DispatchCalls).

## Switches

| switch | default | what |
|---|---|---|
| EDEN_NX2_C11_ACQFWD | on under MT lite, `a` alternates (read per call) | F1 acquire forwarding |
| EDEN_NX2_C11_ACQFWD_VERIFY | on (with ACQFWD), `0` off | back-side ordered fence check of every forwarded acquire |
| EDEN_NX2_C11_ACQFWD_CHECK=1 | off | front also drains + releases fences and compares |
| EDEN_NX2_C11_POSFWD | off (`1` on, `a` alternates) | F1b timestamp words of direct 16-byte payload releases in the overlay |
| EDEN_NX2_C11_LISTRANGE | off (`1` on; contingent, data missing) | F2 compute write ranges for list waits (needs C10_LIST_PRECISE) |
| EDEN_NX2_C11_LISTRANGE_CHECK=1 | off | re-read F2-shortened lists after WaitForWriters, count stale |
| EDEN_NX2_C11_LISTRANGE_IMAGES | on (`0`: storage image writes make a dispatch unknown) | F2 detail |
| EDEN_NX2_C11_STATS=1 | off | the C11 stats line |

## F1 ACQFWD (puller.cpp C11ForwardAcquire, GpuSplit::C11PredictAcquire)
- Case: semaphore GEQ/EQ acquire (SEMAPHORE_TRIGGER path after MetInMemory / PredictedRelease /
  ledger / FOREIGNACQ / PENDACQ, before QMDLEDGER and C10 B1; also in CanResumeAcquire) with a
  pending release for the word. Prediction = last release noted for the word (plain/max) plus
  the QMD add releases noted after it (new per-note order). This is the "site 1 op 41 pending"
  shape: a QMD plain release resets a counter that adds then raise; PredictedRelease only looked
  at the payload. GEQ: lower bound >= value -> forwarded, no wait. EQ: only exact (plain
  release not settled yet, no add queued before it).
- Refused (stats per reason): GPU-written page, pending-write overflow, no/stale release,
  release or add noted by another channel (DmaPusher::DispatchCalls sets the channel),
  copy/I2M/2D writer sent after the release, drain-known / I2M / report / other noted write over
  the word (only the release notes themselves at that address are allowed), open I2M note,
  plain release + adds short of the value ("short" = known not to meet), max release short
  ("max short"), EQ not exact, mask acquires. Front-side puller effects between release and
  acquire: none (syncpoint/fence/refcnt/WFI all go through the ring).
- VERIFY: split.Defer at the acquire's ring position -> rasterizer->SignalFence(check) (a
  committer Command under MT; runs after every release callback queued before it, in both
  delayed and immediate fence modes). Unmet -> ERROR "C11 acquire forwarded but not met". Costs
  one (possibly skipped) fence + GPU cache invalidation per forwarded acquire; VERIFY=0 drops it.
- CHECK: drain + ReleaseFences + read; unmet -> ERROR "C11 acquire check" and the old path runs.

## F1b POSFWD (maxwell_3d.cpp FrontDmaRead, GpuSplit::KnownPendingWords)
- Stats always available (DMA_READ_WHY or C11 stats): "posfwd past-known N, shapes: [macro,
  offset in the noted write, read bytes, write bytes, direct]" = the data the plan asked for.
- With POSFWD on, a read reaching words 2-3 of a direct 16-byte payload release (within its 16
  bytes) gets them from the front's GPU().GetTicks() instead of waiting for the sequencer.
  Off by default: the timestamp differs from the one the sequencer writes (earlier by the
  backlog); turn on only if the shapes show the reads are these words.

## F2 LISTRANGE (gpu_split.h WaitForListWriters / C11ListSeqTarget, kepler_compute.cpp mirror)
- Per DmaPusher call, KeplerCompute::FrontApply marks QMD launch methods (0xae-0xb0, 0x107) as
  "described" and any other memory-writing compute method as unknown; the QMD mirror's dispatch
  callback (new, no effect on the mirror) computes each dispatch's SSBO write ranges from the
  sequencer-published program table (hook 1) and the QMD constant buffer words (refused if the
  page is GPU-written or any queued write may change them: noted writes, 3D CB_DATA ranges
  (FrontNoteWrites; DMA multi-method runs unranged unless hook 2), copy/I2M/2D writers, unknown
  or overlapping compute writers, all against the committer position). NoteWriter(compute)
  logs the call as a ranged writer (ring of 256) or raises the unknown-writer position.
- The list wait's sequencer target becomes max(copy/I2M/2D writers, unknown compute writers,
  ranged writers overlapping the list bytes or in another address space, B2 noted bound); the
  committer bound is unchanged. Only when B2 found no writer-known note over the list bytes.
- Positions of gathered (unsent) entries are resolved in FlushMacroList / FlushStateSlow.

## Hooks (docs/cycle11-hooks-W1.md)
1. vk_mt_pipeline.h CaptureCompute: C11PublishComputeWrites (program table). Without it F2 does
   nothing (every dispatch "unknown program").
2. maxwell_3d.cpp FrontCallMultiMethod: C11NoteCbData for CB_DATA runs. Without it F2 refuses
   while any DMA multi-method CB_DATA run is queued ("stale cbuf").

## Risks / what to look at in the test round
- ACQFWD: "verify ... unmet" must be 0 (and ACQFWD_CHECK unmet 0). Watch "refused:" reasons: if
  "short" dominates, MKW's words are not the reset+adds shape and F1 gains nothing (then the
  remaining site-1 waits are an un-noted writer). Compare puller waits/s and ms/s with
  ACQFWD=a (ALT_RANDOM). Compute dispatches per draw (the game may adapt).
- VERIFY adds one SignalFence per forwarded acquire (fence count/s, GPU cache invalidations).
- POSFWD: off; read the shapes line first.
- LISTRANGE: off; needs both hooks; turn on with LISTRANGE_CHECK=1 (stale must be 0) and look at
  "listrange calls/shorter" and "dispatches known / unknown program / stale cbuf". Residual risk:
  a shader at a reused code address with different SSBO writes (table keyed by address space +
  code address), CPU rewriting a constant buffer between the front's read and the capture.
- New per-note fields (order, channel) in PendingRelease/PendingAdd; C11BeginCall/NoteWriter add
  one flag check per DmaPusher call with the switches off.
