# MT lite, iteration 2 (approach B, branch mkw-mt-lite from ef10090)

Round 1 (docs/mt-lite-round1.md): MT=1 MT_LITE=1 at 52-55 fps vs 62-63 MT unset (0.82-0.89).
The sequencer is the limit (~86% busy, BackLoop self 13.65% = waiting for the front) and the
front waits 42% (WaitConsumed 18.7%, sched_yield 16%, WaitForWriters 7%, DrainToSettleTarget 2%).
Every front wait that needs the sequencer to run out its queue leaves it idle until the front
sends again (the ping-pong), so throughput needs (1) fewer/shorter front waits, (2) no
tracker_mutex traffic between the two back threads, (3) a cheaper capture per draw.

## Reading of the code
- `settled` only moves at Marks, and the sequencer sends Marks only where it stores `consumed`
  (every 4096 ring words, at the front's wait_target, at the end of what was published). So the
  "committer only up to its guest writes" waits (DMA_CONSUMED, L4) still waited for the committer
  to record everything up to the next of those points: in practice the end. That is the
  WaitForWriters 7% / DrainToSettleTarget 2% (WaitSettled is inlined there).
- committer_write_position moves at every Query and Fence Command (~1 every 10 draws in MKW):
  even with the marks above, the writer waits (command lists, MME DMA reads the front noted no
  write for) would wait for nearly every draw. Those reads can only see bulk committer writes
  (DMA copies/clears, downloads): the front notes every release/report it sends.
- The writer wait before command lists (WritersCommands) waited for `settled` of the last writer
  (the whole committer queue), not even for the committer's writes only.
- Puller (MKW, EDEN_NX2_PULLER_STATS, older run): acquire drain ~190/s, acquire fences ~120/s,
  resume poll ~13k/s of which ~330/s drain a busy back. EDEN_NX2_RESUME_LAZY (drain every 8th
  poll when no queued release can meet it) was off by default; same for EDEN_NX2_DMA_READ_PARTIAL
  (position waits instead of full drains for MME DMA reads), measured only without MT.

## Pieces (all inert with EDEN_NX2_MT unset; on by default with EDEN_NX2_MT_LITE=1;
`EDEN_NX2_MT_LITE_<X>=0` turns one off, `=1` turns it on without LITE; read once)
| id | switch | change | files |
|---|---|---|---|
| L6 | EDEN_NX2_MT_LITE_WRITERS | writer waits before CPU command lists (new reason WritersCpuCommands) and MME DMA reads (WritersDmaRead): `settled` only up to the committer's last bulk guest write (new committer_bulk_write_position: Commands that write guest memory other than Query/Fence) | gpu_split.h/.cpp, dma_pusher.cpp, vk_mt_pipeline.h (Command) |
| L7 | EDEN_NX2_MT_LITE_DMAPARTIAL | EDEN_NX2_DMA_READ_PARTIAL defaults on: MME DMA reads under position-noted writes wait for those positions + writers, not everything sent | engines/maxwell_3d.cpp |
| L8 | EDEN_NX2_MT_LITE_RESUME | EDEN_NX2_RESUME_LAZY defaults on: suspended acquires with no queued release poll memory/latch and drain every 8th poll | engines/puller.cpp |
| L9 | EDEN_NX2_MT_LITE_WMARK | the sequencer sends a Mark after every ring entry that queued a committer guest write, so `settled` reaches the write without the rest (makes DMA_CONSUMED, L4 and L6 real) | gpu_split.h/.cpp |
| -- | (EDEN_NX2_GPU_PROFILE) | the "GPU split drains per second" line also shows per reason the ms spent waiting for the committer ("X ms committer") | gpu_split.cpp |
| L10 | EDEN_NX2_MT_LITE_CTRACK | committer: MarkWrittenBuffer's tracker half (written SSBO/texel/TFB ranges, fence download queue, read traps) queued by a nestable MtWrittenBatch and applied under one tracker_mutex hold per bind pass (BindHostStageBuffers, BindHostComputeBuffers, BindHostGeometryBuffers, MtCommitResolvedBuffers) instead of one per buffer; MtApplyStaleRanges only swaps the list under the lock. Safe: the sequencer marked those ranges GPU-modified at capture; nothing in a bind pass reads the tracker while recording a packet | buffer_cache.h, buffer_cache_base.h, buffer_cache_mt.h, mt/lite2_tracker_switches.h |
| L11 | EDEN_NX2_MT_LITE_SEQLOCK | sequencer: tracker_mutex released after capture pass 2 (FinishOverlay(Compact) and the upload memcpys run after it; tracker bits still taken under it); SeqInvalidateBuffers one hold for a single range too, MtPageHoldsGpuData skipped when no page of the range was ever GPU-written | vk_mt_pipeline.h |
| L12 | EDEN_NX2_MT_LITE_CAPMEMO | FinishOverlayCompact: 64-entry memo keyed on the raw_reads list (address, size, offset, kind) reuses the sorted list and slot eligibility; bytes still compared every draw (MT_STATS: "overlay memo hits/s") | vk_mt_pipeline.h |
| L13 | EDEN_NX2_MT_LITE_XLATE | 128-entry per-4 KiB-page GPU->device translation cache on the sequencer keyed on the layout generation (CONT_MEMO rule) and memory manager id: CaptureDraw/CaptureCompute (indirect, index, vertex, cbuf), SeqReadDescriptor, SeqRead u64 | vk_mt_pipeline.h, mt/xlate_cache.h, mt/lite2_capture_switches.h |

Left for later (hooks outside these files): one MtWrittenBatch around the whole ConfigureImpl
bind sequence (vk_graphics_pipeline.cpp ~802-913, after UpdateGraphicsBuffers, before
RecordDraw) for one hold per draw; page test before the TrackerScope in IsRegionGpuModified /
MtPageHoldsGpuData; translation cache for SeqRead<u32> (needs a MemoryManager syncpoint-shim
getter); single-page WriteBlockCached fast path (1.8% of the sequencer).

## What to measure (BIN=/home/appo/src/Eglantine-mtB/build/pgo/bin/eden-cli, from tools/)
Start line: `MT lite: true (...)` lists every switch above as true.
1. fps: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 ./demo.sh $BIN 120 lite2-fps`
   vs MT unset (`lite2-off`). Target: >= 0.95 of MT unset, then above it.
2. Waits: once with `EDEN_NX2_GPU_PROFILE=1` (+ `EDEN_NX2_PULLER_STATS=1`): per reason waited/s,
   ms and "ms committer"; expected: writers (cpu lists)/(dma read) committer ms near 0, puller
   ms down, resume poll behind ~40/s. With MT_STATS: "marks" up by ~fences+queries/s (L9 cost).
3. perf per thread (as round 1): front WaitConsumed+sched_yield+WaitForWriters share down, BackLoop
   self down; GPUCommit TrackerScope ~gone; GPUBack SpinRecursiveMutex::lock/TrackerScope,
   CaptureDraw, FinishOverlayCompact, GpuToCpuAddress down.
4. Correctness: `EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 EDEN_NX2_MT_VERIFY=1 EDEN_NX2_MT_STATS=1`: 0
   mismatches; `EDEN_NX2_DMA_READ_CHECK=1` run: "without a drain (N stale)" stays 0 (L6/L7);
   picture check; watchdog run (`EDEN_NX2_MT_WATCHDOG=1`) without "MT wait > 2 s".
5. Bisect only on a loss/problem, one at 0 per run (WRITERS, DMAPARTIAL, RESUME, WMARK, then
   the tracker and capture switches).

## Risks
- L6: a command list or an un-noted MME DMA read written by a fence callback or a query result
  copy that the front did not note would be read before the committer writes it (none known: the
  front notes every release/report it sends; DMA copies, downloads and other kinds still count).
- L7: measured -1.2 fps without MT (but no stale read in ~60k checked); under MT it avoids full
  drains, to be measured.
- L8: an acquire met by a write nobody noted (a shader) is seen up to 8 polls later.
- L9: ~5-7k more Marks a second (a stream entry + flush each, a committer wake if it sleeps);
  with resolvers (RESOLVERS>0) each also closes the open batch.
- L10: the per-stage hold covers several ranges (a little longer); if recording throws and the
  batch flush throws too, terminate.
- L11: upload bytes are read after the tracker take (a guest write in between traps and re-marks
  the range, so the bytes are as new or newer and the next capture takes them again); VERIFY's
  "upload changed" count may move. Bisect order on a picture problem: SEQLOCK, XLATE, CAPMEMO.
- L13: a remap without a layout generation bump would return stale translations (same rule as
  CONT_MEMO).
- Nothing measured yet; L11-L13 saving estimates come from the older p1b dwarf profile.
