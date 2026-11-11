# MT lite, iteration 3: the front (approach B, branch mkw-mt-lite from 2015a32)

Iteration 2 (docs/mt-lite-round1.md): parity with MT unset (~57.8 vs ~57.5 fps at 2x). The front
(GPU thread) is the limit: GPU_PROFILE ~93% busy (push 368 ms/s, front method 219, macro 153,
split drain 188); 9.7M 3D register writes/s, 5.4M same value. Waits/s: puller 2-2.4k (~470
waited, 68-87 ms, nearly all on the sequencer), writers (commands) ~13k (450 waited, 43 ms),
writers (dma read) ~900 (230 waited, 45 ms, committer 0), position ~250 (11-13 ms), writers (cpu
lists) 70-97k (2 waited). The sequencer is cheap (~4.4 us/draw), the committer idles 31%.

## Reading of the code
- **Known CPU command lists stopped working under L6.** DmaPusher::Step skips the writer wait for
  lists on pages noted as CPU command lists (IsKnownCpuCommandList, on for title 1), but pages
  are only noted after a wait that *waited*. With EDEN_NX2_MT_LITE_WRITERS the wait almost never
  waits (2/s), so no page is noted and every GPFIFO entry (the 70-97k/s "writers (cpu lists)")
  runs WaitForWriters: a FlushState (the gathered state block is cut at every entry: more State
  entries for the front to write and the sequencer to apply), the position loads and CountWait,
  plus a second page walk. Finding the writers already done tells as much as waiting for them.
- **Per-word dispatch dominates "push" + "front method".** Every 3D register word goes
  ProcessCommands -> CallMethod (semaphore watch, GpuSplit::Get, puller range, subchannel lookup,
  WritesMemory's engine switch, SCOPE_EXIT, profile scope, FrontMacros, engine type, native_turing)
  -> FrontCallMethod -> Front() -> FrontWrite. perf: FrontCallMethod 10.1% + DmaPusher CallMethod
  7.6% + ProcessCommands 6.5% + FrontWrite 5.4%. Upstream's EDEN_NX2_FRONT_BATCH (Zelda local
  test, off, never measured here) already sends incrementing runs to Maxwell3D::FrontWriteRun,
  but only runs > 1 word, and FrontWriteRun still calls the full FrontWrite per word.
- Macro emits: JIT -> CallEmit thunk -> lambda -> FrontWrite (out of line) for every write, even
  the same-value state writes FrontWrite drops at once (most of the 5.4M/s).
- writers (dma read): MME DMA reads of pages no noted write covers wait for the sequencer to run
  every copy/compute writer sent (~200 us each). EDEN_NX2_DMA_READ_KNOWN (off) skips that for
  pages a read already found not GPU-written after the writers ran - the same rule as the known
  command-list pages that gave title 1 its big gain.
- puller: SEM_EXECUTE acquires no queued release predicts drain and release fences (two waits)
  although an unmet one never blocks the channel (it logs and goes on): the drain only changes
  when the front continues. EDEN_NX2_SEM_EXECUTE_NODRAIN (off) skips both.
- Not done (need data or deeper hooks): per-page writer positions for GPU-written command lists
  (writers (commands) 43 ms/s: the list copy is made by the committer after the writer, so the
  wait would need the sequencer to record which position wrote each list page and the copy path
  to be ready earlier); unpredicted semaphore_trigger acquires (EDEN_NX2_ACQUIRE_STATS +
  EDEN_NX2_PULLER_STATS will show which sites/ops remain after L18).
- Two-stage front (parse thread + method/macro thread): not now. Puller acquires suspend parsing
  on values the method stage's sends decide, command-list fetches wait for writers the method
  stage sends, and macro DMA reads drain: each of those becomes a cross-thread wait on a ~100k
  entries/s handoff. Worth it only if, after L14-L18, the front's own work (not waits) is still
  > ~85% of the frame; the profile below decides.

## Pieces (front only; mt/lite3_front_switches.h)
On by default under EDEN_NX2_MT_LITE=1; `EDEN_NX2_MT_LITE_<X>=0` turns one off; `=1` turns it on
also without the MT back (plain split, MT unset), where every piece is off by default.
| id | switch | change | files |
|---|---|---|---|
| L14 | EDEN_NX2_MT_LITE_KNOWNLIST | a list whose writer wait found the writers done (and still not GPU-written, checked again) is noted as a known CPU list too | dma_pusher.cpp |
| L15 | EDEN_NX2_MT_LITE_FBATCH | EDEN_NX2_FRONT_BATCH defaults on, single words too; FrontWriteRun applies non-executable writes inline (shadow RAM, same-value skip, StateWrite), executable ones / Replay / instance-loop methods / SPLIT_VERIFY take FrontWrite | dma_pusher.cpp, maxwell_3d.cpp |
| L16 | EDEN_NX2_MT_LITE_FCALL | DmaPusher::CallMethod/CallMultiMethod go to the 3D front before the writer and engine checks (never true for Maxwell3D); the macro emit callback drops a same-value state write before FrontWrite | dma_pusher.cpp, maxwell_3d.cpp |
| L17 | EDEN_NX2_MT_LITE_DMAKNOWN | EDEN_NX2_DMA_READ_KNOWN defaults on | maxwell_3d.cpp |
| L18 | EDEN_NX2_MT_LITE_EXECNODRAIN | EDEN_NX2_SEM_EXECUTE_NODRAIN defaults on | puller.cpp |

## What to measure (BIN=$HOME/src/Eglantine-mtB/build/pgo/bin/eden-cli, from tools/)
1. fps: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 ./demo.sh $BIN 120 lite3-fps`
   vs MT unset (`lite3-off`), alternating, >= 3 each. Target: MT lite > MT unset.
2. Front-only pieces on the baseline (optional, tells what is MT-specific): MT unset +
   `EDEN_NX2_MT_LITE_KNOWNLIST=1 EDEN_NX2_MT_LITE_FBATCH=1 EDEN_NX2_MT_LITE_FCALL=1
   EDEN_NX2_MT_LITE_DMAKNOWN=1 EDEN_NX2_MT_LITE_EXECNODRAIN=1` (`lite3-offfront`).
3. Profile: `EDEN_NX2_GPU_PROFILE=1 EDEN_NX2_PULLER_STATS=1 EDEN_NX2_ACQUIRE_STATS=1`. Expected:
   "writers (cpu lists)" calls from 70-97k/s to ~0; "writers (dma read)" waited ~0; puller calls
   and ms down ("sem_execute skipped" appears); push + front method ms down (FBATCH moves the run
   writes from "front method" into "push": compare their sum). perf of GPU: CallMethod,
   FrontCallMethod, ProcessCommands shares down, FrontWriteRun up.
4. Correctness: VERIFY run (0 mismatches); `EDEN_NX2_SPLIT_VERIFY=1` run (front registers vs back:
   no differences logged; it forces FrontWrite in FBATCH, so also one run with it off to
   compare); `EDEN_NX2_DMA_READ_CHECK=1` run: "without a drain (N stale)" stays 0 (L17);
   picture check; watchdog run.
5. Bisect only on a loss/problem, one at 0 per run: DMAKNOWN, EXECNODRAIN (stalls/hitches: fences
   released later), KNOWNLIST, FBATCH, FCALL.

## Risks
- L14: a list page first written by a dispatch the sequencer has not captured yet, read right
  after it was noted, reads stale words: the same risk the known-list skip already takes; the
  second IsGpuWrittenCommands narrows the window to a writer captured after that check.
- L15/L16: must equal FrontWrite per word. Replay mode, executable methods, instance-loop methods
  and SPLIT_VERIFY keep the old path; the profile counters are kept.
- L17: an MME DMA read of a page that a queued dispatch writes for the first time reads stale
  words once (DMA_READ_CHECK counts it).
- L18: fences are released by the next SignalFence/TickWork instead of at the acquire, so CPU
  waits on them may end a little later.
- Expected gain: front work down ~100-200 ms/s (per-word dispatch, ~85k writer waits + state
  splits, ~45 ms writer DMA waits, part of the puller waits): +8-15% fps if the front stays the
  limit.
