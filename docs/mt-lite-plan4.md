# MT lite, iteration 4: the front's drains (approach B, branch mkw-mt-lite from 91633a9)

Iteration 3 (docs/mt-lite-round1.md): MT lite 66.1 vs MT unset 59.2 fps (+12%). The front is
cheaper but "split drain" grew to 324 ms/s, nearly all of it waiting for the SEQUENCER: puller
366 waited/s 98 ms (acquire drain 164/s, acquire fences 104/s, resume poll ~100/s behind),
writers (commands) 371/s 83.5 ms, position 67/s 83.5 ms (~1.2 ms each), writers (dma read)
70/s 46 ms. Target: front drains < 120 ms/s without breaking ordering.

## Reading of the code
- **Position waits come from L7 shadowing the overlay.** The only WaitForPosition caller is
  Maxwell3D::FrontDmaRead. With EDEN_NX2_MT_LITE_DMAPARTIAL on, `partial` is true for every read
  under position-noted writes, and `overlay` (EDEN_NX2_DMA_READ_OVERLAY: lay the payloads of
  queued fence/semaphore releases over memory, no wait at all) requires `!partial`: since
  iteration 2 every overlay-able read waits instead, for a position that is usually the newest
  release (the sequencer's whole backlog: ~1.2 ms, vs ~0.25 ms for puller waits taken right after
  other drains). Also only 4-byte writes carry values: 16-byte releases (payload + timestamp,
  written at once on the sequencer) and 3D payload reports (noted 16 bytes, no value) always wait.
- **Acquire drains that cannot help (site2 op40 none unmet, 69/s + their 69 fence waits/s).**
  An unpredicted GEQ acquire with no queued release, no noted pending write, no queued add, on a
  word no QMD/DMA release ever targeted and on a page the GPU never wrote, can only be met by the
  CPU or another un-noted writer: the drain + fence release then suspend the channel anyway
  (690/10 s stay unmet). The fences matter for progress (a fence callback may let the CPU release
  the word), not their wait: sending them is enough.
- **Fence releases after an unmet acquire (104/s)** are a second full round trip right after the
  drain; the resume poll sees whatever they write (memory + the waiter latch, registered first).
- **op43 none met (52/s)**: GEQ acquires on words QMD *max* releases write, met after the drain,
  with no release the front noted (the front mirror misses some launches; its entries are also
  dropped once memory met them). Max releases only raise the word: once the sequencer has run the
  QMD, an in-flight max release of payload >= wanted meets the acquire whatever else happens (the
  same ordering argument as the front's own predictions: everything after the acquire goes behind
  it). A sequencer-side ledger of in-flight max releases (added when the QMD runs, removed by its
  fence callback once memory has it) predicts them; when the QMD may still be in the ring, a
  sequencer-only wait (no committer settle) and a second look avoids the committer part.
- **The sequencer marks every entry while the front waits for the committer.** BackLoop stores
  `consumed` + SeqMark when `read_position >= wait_target`; the front clears wait_target only
  after its settle wait, so meanwhile *every* entry pays a Mark (stream push + flush + wake) and a
  seq_cst consumed store: slower sequencer and more committer entries exactly while the front
  waits.
- Not done: writers (commands) (GPU-written command lists need every queued writer captured and
  the list copy recorded by the committer; per-page writer positions would only cut the committer
  share, ~25 ms/s, and a missed write path means stale lists / device loss); writers (dma read)
  on QMD release words (Writers-known, values unknown); UBO_FAST from A (B's packets already take
  the decision at capture, L1).

## Pieces (mt/lite4_switches.h; on by default under EDEN_NX2_MT_LITE=1; `EDEN_NX2_MT_LITE_<X>=0`
turns one off; front pieces (L19-L22) `=1` also on without the MT back; L23 needs EDEN_NX2_MT)
| id | switch | change | files |
|---|---|---|---|
| L19 | EDEN_NX2_MT_LITE_OVERLAY | FrontDmaRead tries the overlay before the partial wait; wider noted writes keep their first word's value (16-byte releases), 3D payload reports (Release, report Payload, no reduction) note their payload; an overlay is refused when the read touches bytes past a known first word | maxwell_3d.cpp, gpu_split.h |
| L20 | EDEN_NX2_MT_LITE_FOREIGNACQ | semaphore_trigger acquire (Ampere) not predicted, with nothing pending that could write the word (no queued release/add, no unrun noted write, not a QMD/DMA release target (new never-cleared bitset written by the back), page never GPU-written): no drain; waiter registered, fences released without waiting (Defer), memory/latch checked, channel suspended. Resume polls of such acquires drain every 64th poll (not 8th) | puller.cpp, kepler_compute.cpp, maxwell_dma.cpp, mt/lite4_switches.h |
| L21 | EDEN_NX2_MT_LITE_ASYNCFENCES | an acquire still unmet after its drain sends the fence release without waiting for it; the resume poll sees what it writes | puller.cpp |
| L22 | EDEN_NX2_MT_LITE_QMDLEDGER | sequencer-side ledger of in-flight QMD max releases (device address -> max payload; cleared by the fence callback that writes memory): GEQ acquires (trigger and resume) it meets pass without a drain; on a word QMD max releases target, an unpredicted acquire first waits for the sequencer only and looks again before the full drain | kepler_compute.cpp, puller.cpp, gpu_split.h/.cpp, mt/lite4_switches.h |
| L23 | EDEN_NX2_MT_LITE_SEQTARGET | the sequencer clears wait_target (CAS) once it stored `consumed` past it: one Mark at the target, not one per entry for the rest of the front's wait | gpu_split.cpp |
| -- | (EDEN_NX2_PULLER_STATS / ACQUIRE_STATS) | new sites: "acquire nodrain" (L20), "acquire ledger" (L22 met), "acquire seq wait" (L22 first stage); ACQUIRE_STATS site 3 = L20 suspensions (met = memory already met), kind 4 = DMA semaphore target | puller.cpp |

## What to measure (BIN=$HOME/src/Eglantine-mtB/build/pgo/bin/eden-cli, from tools/)
Start line: `MT lite 4: overlay true, foreignacq true, asyncfences true, qmdledger true, seqtarget true`.
1. fps: `env NOPROF=1 EDEN_NX2_VSYNC_HZ=240 EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 ./demo.sh $BIN 120 lite4-fps`
   vs MT unset (`lite4-off`), alternating, >= 3 each.
2. Profile: `EDEN_NX2_GPU_PROFILE=1 EDEN_NX2_PULLER_STATS=1 EDEN_NX2_ACQUIRE_STATS=1`. Expected:
   split drain 324 -> ~170-200 ms/s; position waits ~67 -> <15/s; puller waited ~366 -> ~150/s
   (PULLER_STATS per 10 s: "acquire fences" ~0 behind, "acquire nodrain" ~700, "acquire
   ledger" up to ~500, "acquire drain" ~1640 -> ~500); resume poll behind down; MT_STATS marks/s down a little (L23).
3. Correctness: VERIFY run (0 mismatches); `EDEN_NX2_DMA_READ_CHECK=1` ("without a drain (N
   stale)" stays 0: L19 overlays more reads); `EDEN_NX2_VERIFY_ADDS=1` unchanged; watchdog run
   (`EDEN_NX2_MT_WATCHDOG=1`, no "MT wait > 2 s"); suspension log (`Channel X waits on semaphore`
   must not appear more than before: L20/L21 rely on the resume poll); picture check.
4. Bisect on a loss/hang/hitch, one at 0 per run: FOREIGNACQ, ASYNCFENCES (late resumes =
   hitches), QMDLEDGER (a wrong pass = flicker/corruption), OVERLAY (stale MME reads), SEQTARGET.

## Risks
- L19: a read of a word whose 16-byte release is fence-based and already "settled" reads memory
  before the fence wrote it (the same hazard every position-noted write already had); the
  overlay itself only adds values that are exact (non-fence payloads are written on the sequencer).
- L20/L21: an acquire a fence callback or an un-noted GPU write would meet resumes on a later
  poll (latency, not order); a bitset/hash miss can only add latency (the resume poll still drains
  every 64th poll; the old path every 8th).
- L22: a CPU reset of a counter while a max release is in flight is ignored (the release leaves
  >= payload anyway); entries live from QMD run to fence callback; the table is capped (cleared =
  more drains, never wrong passes). The ledger is keyed by device address (channels' address
  spaces differ).
- L23: a new wait_target equal to the old one cleared by the CAS is already passed (no wait).
- Expected: front drain -120..-150 ms/s (position ~-65, puller ~-55, sequencer marks); if the
  front stays the limit, +5-10% fps.
