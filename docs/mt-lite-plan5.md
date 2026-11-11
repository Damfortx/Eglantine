# MT lite, iteration 5: writer waits before GPU-written command lists (branch mkw-mt-lite from 3e7785788c)

Iteration 4 (docs/mt-lite-round1.md): MT lite ~+12-15% over MT unset, front drains ~254 ms/s.
The biggest one left is "writers (commands)": 11.7k calls/s, 408 waited/s, 147.7 ms/s (~0.36 ms
each), committer share 32.7 ms (so ~115 ms/s is the sequencer).

## Which fetches wait (reading of the code)
- The "writers (commands)" reason (DrainReason::WritersCommands) has exactly one caller left:
  `DmaPusher::ReadGpuWrittenCommands` -> `GpuSplit::WaitForWriters()`. Step's own writer wait
  (lists on pages never GPU-written) uses WritersCpuCommands under EDEN_NX2_MT_LITE_WRITERS
  ("writers (cpu lists)", 6/s): L14/KNOWNLIST works for those.
- So the 11.7k/s are command lists on pages with the GPU-written bit (`Nx2IsGpuWritten`, set at
  capture by the sequencer for every written SSBO/buffer view/DMA interval, never cleared). Step
  skips IsKnownCpuCommandList for them by design (`!gpu_written && ...`): **L14 cannot catch them,
  whatever the page history** - that is why KNOWNLIST did not remove them.
- For each such list the front waits for the sequencer past *every* writer sent
  (`last_writer_position`: any compute method that may write, every I2M method, DMA launches) and
  then for the committer past the same position (full settle; WritersCommands is not a
  LiteWritersReason), then reads each page via `Nx2ReadListCopy`: Synced (guest memory: CPU list
  on a page the GPU wrote once and that was flushed since) / Copied (copy recorded after the
  writer) / Unknown (drain + flush).
- An older note (memory, 2026-11-06) says MKW had 0 GPU-written lists in a DUMP_GPU_LISTS run
  (non-MT, pre frames-ahead). 11.7k/s now means either large written SSBO ranges cover command
  memory (pages marked GPU-written though the shader does not write the list bytes) or command
  memory reused from GPU buffers: both give **Synced CPU lists on once-GPU-written pages**, where
  the wait only guards against a queued writer writing the page for the first time. Real culling
  lists (Copied every frame) are the other possibility. The new statistics below tell which.

## Pieces (mt/lite5_list_switches.h; on by default under EDEN_NX2_MT_LITE=1, `=0` turns one off; need EDEN_NX2_MT)
| id | switch | change | files |
|---|---|---|---|
| L24 | EDEN_NX2_MT_LITE_LISTPOS | per-page writer positions: while the sequencer runs an entry, `Nx2MarkGpuWritten` records the command list pages it marks in a 16k-slot table (page hash -> 1 + entry position; collisions only lengthen waits) and the entry stores a mark (exec_wrote_committer). ReadGpuWrittenCommands then waits for the sequencer past every writer as before, but for the committer only up to max(slot positions of the list's pages, committer_write_position) instead of every writer. Off for a page's first list (writers captured before it became a list page are not recorded; such a read is Unknown and drains anyway) | nx2.h, gpu_split.h (WaitForListWriters), gpu_split.cpp (BackLoop), dma_pusher.cpp |
| L25 | EDEN_NX2_MT_LITE_LISTKNOWN | ring/CPU list pages that are GPU-written-once: a page read Synced 8 times in a row over >= 100 ms, with its slot unchanged (no capture wrote it), is trusted (per channel, ListTrust); lists whose pages are all trusted and whose slots are <= settled skip the writer wait. A trusted page read not Synced is banned forever and the list is read again after the full wait (fallback). Same policy as known CPU lists (EDEN_NX2_WRITER_WAIT, title default) | lite5_list_switches.h, dma_pusher.cpp/.h |
| check | EDEN_NX2_MT_LITE_LISTCHECK=1 (off, diagnostic) | every L24-shortened or L25-skipped fetch is read again after the full WaitForWriters; words compared, "stale" counted (first 16 logged), the second read used. Line "List check: N compared, M stale, K unverifiable; L25 fallbacks F" every 10 s | dma_pusher.cpp |
| stats | EDEN_NX2_LIST_STATS=1 (full) / EDEN_NX2_GPU_PROFILE (basic) | line "GPU-written list fetches per second": fetches, waited, new pages, multi-page; **waited because**: page writer queued (a writer of the list's pages was not yet run by the sequencer), page writer uncommitted (run, not committed), other writers (no writer of these pages pending: pure over-wait), first list of page; last writer kind (compute/dma/i2m/...); results synced/copied/unknown; L24/L25 counts. With LIST_STATS=1 also the page state before the fetch (no entry/synced/copy current/copy stale/draw-written, all and waited, via BufferCache::Nx2PeekListPage) and the 6 pages that waited most | lite5_list_switches.h, dma_pusher.cpp, buffer_cache_base.h, rasterizer_interface.h, vk_rasterizer.h |
Start line: `MT lite 5: listpos true, listknown true, listcheck false`.

## What to measure (BIN=$HOME/src/Eglantine-mtB/build/pgo/bin/eden-cli, from tools/)
1. Data first: `EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 EDEN_NX2_LIST_STATS=1 EDEN_NX2_MT_LITE_LISTKNOWN=0`
   (+ GPU_PROFILE): tells what the 408 waits/s are. Expected (hypothesis): results ~all synced,
   reasons mostly "other writers" (pure over-wait), state before "synced". If results are mostly
   "copied" and reasons "page writer queued", the lists are real GPU lists: L25 gains nothing and
   the next step is front-known writer ranges (I2M/DMA destinations, see below).
2. fps: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 ./demo.sh $BIN 120 lite5-fps`
   vs MT unset (`lite5-off`), alternating, >= 3 each; also LISTKNOWN=0 (L24 alone).
3. Profile: GPU_PROFILE + PULLER_STATS: "writers (commands)" 147.7 ms/s -> L24 alone ~115 (committer
   share -> ~0-10 ms); with L25, if the pages are synced CPU lists, waited/s -> near 0 and calls
   drop (L25 skips do not call WaitForWriters). "L25 skipped" in the list line ~ the fetch rate.
   MT_STATS marks/s may rise a little (one mark per list-page writer entry).
4. Correctness: `EDEN_NX2_MT_LITE_LISTCHECK=1` run: "stale" must stay 0 (L24 and L25); VERIFY run
   (0 mismatches); DMA_READ_CHECK 0; watchdog; picture check (missing/garbage geometry = stale
   lists); no device loss.
5. Bisect on a loss/garbage: LISTKNOWN=0 first (heuristic), then LISTPOS=0.

## Risks
- L25 is a heuristic of the same kind as the known-CPU-list skip MKW already uses: a trusted page
  whose list bytes a writer still in the ring (not yet captured) generates for the first time is
  read stale once. Narrowed by the streak, the 100 ms span, the slot <= settled check (captured
  writers), the fallback + ban when a committed write shows, and the policy gate; LISTCHECK counts it.
- L24: writes that reach a list page on the committer without a sequencer capture marking it
  (not a buffer-cache SSBO/view/DMA-interval write, not a committer guest write) are not waited
  for. Known paths are covered: captures (MtCaptureWrittenRange, MtCaptureDmaCopy) mark at the
  sequencer; DMA/download/guest writes are in committer_write_position. NativeQmdScheduler-held
  dispatches captured later behave as before (same hazard as the full wait).
- The slot record costs a TLS store per entry and a bitmap AND per written range on the sequencer;
  list-page writer entries add a mark.
- MT unset: unchanged behavior (positions are only recorded with the MT back; the new code paths
  need MtActive; statistics only with GPU_PROFILE/LIST_STATS).

## Not done (needs the statistics)
- Front-known writer ranges: I2M (KeplerMemory) and DMA copies have destination registers the
  front could mirror; a list whose pages no pending I2M/DMA can write and with no pending compute
  could skip the sequencer wait exactly. Worth it only if "last writer kind" shows i2m/dma
  dominating the waits. Compute dispatch destinations (SSBOs from const buffers) are only known
  at capture.
- Snapshot-at-submit for synced pages (submitted_words) is not used: Step reads GPU-written pages
  from memory on purpose (older frames' words).
