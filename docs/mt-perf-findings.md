# MT throughput: profile findings (2026-11-08, mkw-mt 5a8724f, title 1 attract demo, 2x, VSYNC 240)

Data: `tools/runs/perf/p1-GPU.txt` (front, dwarf), `p1b-GPUBack.txt` (sequencer, dwarf),
`d1-GPUCommit.txt` (committer, dwarf), `d1-GPUResolve0.txt` (resolver, dwarf); stats in
`runs/log-p1.txt`, `runs/log-d1.txt` (EDEN_NX2_MT=1 defaults incl. RBUF, MT_STATS,
EDEN_NX2_DMA_READ_WHY in d1). `under.py <file> <func> [depth]` breaks them down.
perfthreads.sh now takes THREADS="..." and CALLGRAPH=1; only one `perf record` container works
at a time (the others write empty files), so profile threads one after another.

## Where the time goes (shares of each thread's samples)

Front (GPU): 61% waiting in GpuSplit::WaitConsumed (47% sched_yield + 15% spin). Callers:
29% Drain and 25% WaitForWriters, both from Maxwell3D::FrontDmaRead (MME DMA reads, ~300 a
second wait: 0 on GPU-written pages, ~60/s positions, rest writers/plain). Under MT these waits
also wait for `settled` (WaitSettled), i.e. sequencer + resolver + committer latency on every
read. This round trip is the main limiter: the committer and resolvers are mostly idle.

Sequencer (GPUBack, ~70% busy): Execute 69% [Maxwell3D draw path 43% = CaptureDraw 26.6%
(MtTakeUploads 7.8, IsContinuousRange 5.9 (page walk per bound SSBO/texel buffer, memoizable on
MemoryManager::LayoutGeneration like MaxContinuousRange), FinishOverlayCompact 4.5),
FlushCaching 9.9% (SeqInnerInvalidation + SpinRecursiveMutex/TrackerScope), EmitDraw 3.9%];
ProcessCBBind 8.5% (nearly all = the Uniform Command: BindGraphicsUniformBuffer -> Command ->
EmitDeltas 6.2%); KeplerCompute 7.8%]. 18% at libc `syscall` (shown as `setreuid`, no stack):
futex syscalls, most likely SpmcStream::Commit's notify_all (every commit wakes every sleeping
consumer: committer + 2 resolvers, ~400k commits/s). BackLoop own 11.8% = spin waiting for the
front. Per frame the sequencer costs about what the whole single-threaded back did (17-18 ms of
CPU per frame), so MT can only win if capture/emit get much cheaper or the syncs get shorter.

Committer (GPUCommit): 45% spinning in Idle, 26% spinning in BatchCursor::Wait (waiting for an
open batch: it is ahead of the sequencer; adaptive batches shrink them to 1.4-1.6 draws), real
work ~25%. Resolvers: ~85% waiting (Loop spin 30%, sched_yield 27%, futex 16%), ResolveOne ~5%;
stats: busy 2-3%, ~42% of draws slow (view-not-clean even with RREFRESH) and re-resolved by the
committer.

Stats (d1): draw packets 27-45k/s, uniform Commands 124-211k/s (~4.6 per draw), fence 3.6-5.9k,
query 1.3-2.4k, barrier 2-3.5k, batches 20-26k/s, commit wait 208-264 ms/s, marks 2.6-4.2k/s,
delta words ~0.9-1.5M/s.

## Ideas, with what was tried

Branch `mkw-mt-tp1` (5ce9671, compiles, startups OK, NOT validated for speed) has five
switches (all =a capable, default on there): EDEN_NX2_MT_UBIND (no Uniform Command: the
committer binds the uniform buffer from the CbBind entry it already applies to its mirror; the
committer applies it always, so the switch can alternate), EDEN_NX2_MT_WAKE (resolvers sleep on
a per-consumer wake word; the sequencer wakes one at its batch's first packet, BatchEnd, channel
barriers, stream-space pressure, Stop), EDEN_NX2_MT_LAZYPUB (delta entries committed without
publishing; WaitCommitted/Stop publish first), EDEN_NX2_MT_SYNCSTEAL (committer records a draw
itself instead of waiting for its resolver while the front is in WaitSettled), EDEN_NX2_MT_CONTMEMO
(IsContinuousRange memo). Its only stats run was taken while another build kept the machine at
load ~24 and showed ~20k draws/s and 17-19k marks/s (vs ~4k), so it is unmeasured; A/B it with
EDEN_NX2_MT_<SWITCH>=a on an idle machine before using any of it.

Bigger levers not tried: (1) let FrontDmaRead/WaitForWriters wait for `consumed` instead of
`settled` when no committer-side guest write (queries, fence callbacks, FlushRegion/DMA Commands)
can overlap the read (QMD releases are Fence Commands with known addresses: note them with
NotePendingWrite on the sequencer); (2) cheaper sequencer capture (uploads, FlushCaching locks);
(3) fewer resolvers or K=0 (they do ~3% work and add a hand-off per batch to every sync).

## Correctness notes from the same round

- EDEN_NX2_MT_SHADOW with RBUF reported "payload entry N differs" for hundreds of thousands of
  draws: the descriptor union's unused bytes (texel buffer entries use 8 of 24, image/address
  entries leave 4 bytes of padding) held stale data. Fixed on mkw-mt (DescriptorUpdateEntry zeroes
  them). Left after the fix: 2 mismatches in ~600k draws, both a storage/uniform buffer entry with
  the same offset/range but another VkBuffer (resolver vs committer), not investigated yet.
- VERIFY (with the tp1 switches on): 1 "upload ... changed: 11 bytes" in 120 s.
