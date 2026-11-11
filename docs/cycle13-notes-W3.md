# Cycle 13 W3 notes: front list writer waits (L0-L2)

Not built, not run (agent rules). Hook lines for D's files: docs/cycle13-hooks-W3.md (the
plan's "hooks-C"). New header: `src/video_core/mt/c13_front.h`.

## What was implemented

| piece | where | switch (default) |
|---|---|---|
| L0 wiring for the CHECK run | c11_front.h `C13Shared::ListCheckEnv`, `C12Shared::ListRangeGateEnv`; c12_front.h `C12ListWaitCheck` | `EDEN_NX2_C13_LIST_CHECK=1` (off) turns on `C12_LISTWAIT_CHECK`, the `C12_LISTRANGE` gate and `C13_KICK_CHECK` unless each is set to `0` itself; the explicit `=1` variables work as before |
| L0 run totals | c13_front.h `C13FrontStats::NoteListContent`, dma_pusher.cpp (after `c12.NoteContent`) | in the C13 line: listwait check same/changed/unreadable over the whole run, "would (differ)", a "LISTWAIT eligible so far / not eligible" verdict (same > 90 % and 0 would differ), and the LISTRANGE gate state |
| L1 census by shader hash | c13_front.h `C13FrontStats`, `C13ProgramHashes`; dma_pusher.cpp `ReadGpuWrittenCommands`; gpu_split.h `WaitForListWriters` timing, `C11BeginCall` call counter | `EDEN_NX2_C13_STATS=1` (also on with C13_KICK_CHECK / C13_LIST_CHECK) |
| L2 early kick | gpu_split.h `C13NoteComputeWriter` (from `NoteWriter`), `C13EndWait` (replaces the six `wait_target.store(~0)` of the front waits), `C13SeqBeforeKick`, `C13KickMark`; kepler_compute.cpp records the dispatch program also for C13 | `EDEN_NX2_C13_KICK` (on under MT_LITE, needs MT_LITE_SEQTARGET; `0` cycle 12, `a` Nx2AltOn per kicked dispatch), `EDEN_NX2_C13_KICK_LEARN` (16), `EDEN_NX2_C13_KICK_CHECK`, `EDEN_NX2_C13_KICK_SEQ` (on with kick), `EDEN_NX2_C13_KICK_RUN` (on with kick) |

Start line: "Cycle 13 front: kick ..., stats ..., list check ... (listwait check, listrange gate,
listwait)". Stats line every 10 s: "C13 front (10 s): compute list writer waits by program:
[hash 0x...: N waits X ms (Y ms each; seq A ms, committer B ms), writer->fetch U us, C calls,
W words, backlog seq S / committer T words; kicked K (ms), not kicked (ms)] ... | kick: ... |
L0 run: ...". Top 8 programs by wait time, table of 128 keys.

### L1 details
- Key: the compute pipeline's shader hash published by the sequencer (hook H1) for (address
  space, QMD program address); `@0x...` (the program address, as the C12 census) when no hash
  is published yet or the hook is missing. Program 0 (writer call with no mirrored dispatch:
  C12's dominant "0x0") stays `@0x0`.
- Only fetches whose last writer is compute (`LastWriterKind() == 1`), not on a new list page,
  with <= 4 pages, the back behind the writers target, and that actually waited (not an L25
  skip, not a C12 speculated read).
- seq / committer split = the time in WaitConsumed vs TimedWaitSettled inside
  WaitForListWriters (0 for the non-LISTPOS WaitForWriters path). "calls" = DmaPusher method
  calls (C11BeginCall), "words" = ring words sent since the writer; backlog = ring words
  between what the front sent and `consumed` / `settled` at the wait.

### L2 details
- Learning: a program address (stable within a run; the hash is only for the census) becomes a
  writer after `C13_KICK_LEARN` waited fetches on it. Learning runs whenever the kick is
  configured or the census runs.
- Kick: in `NoteWriter` for a compute writer call whose last mirrored dispatch is a learned
  program: FlushState, `wait_target = write_position` (everything sent incl. the dispatch),
  Publish at once. The sequencer's existing L23 path Marks there (and clears the target); the
  committer settles at the Mark. Front waits that set their own target now put a still pending
  kick back (`C13EndWait`, counted "restored").
- Hooks (D's files): H2 the committer ends its lock run at the kick's Mark (so the front's
  list read does not meet the cache mutexes held); H3 the sequencer skips C12 memo *record* on
  a plain miss for draws before the pending kick (not validation, not the check branch).
- 'a': each learned dispatch decides kick/not (Nx2AltOn); the line gives kicked vs not-kicked
  waits and ms (totals and per program) = the per-wait A/B the plan asks for in run 10.
- CHECK: kicked waits that still found the back behind the kicked position at the fetch
  ("check behind kick", count and ms). The wait itself is the cycle 12 wait in every case.

## Risks / what to look at in the test round

- Kicks add publishes and Marks (more lock run ends, more `settled` stores): compare the C11
  lock-run line (end_entry, runs) and committer idle with KICK on vs `a`. With ~460-650
  compute-writer waits/s, kicks should be in the same order (one per learned writer call).
- If "check behind kick" stays close to "kicked waits" with ms per wait unchanged, the 1.9 ms
  waits are GPU/committer work, not backlog: then the census' committer share (and backlog)
  says where; the kick cannot help that.
- `C12LastWriterProgram` is now recorded whenever the kick is configured (default under lite):
  one store per mirrored dispatch.
- Program 0 writers (C12: 84 % of the waits) are never kicked (no learned program). If they
  dominate the remaining time, a next step is to learn by the writer call's method/engine
  instead.
- L0: run 1 should show `listwait check` same/changed > 0 and the gate deciding within the
  first 20 000 list calls ("C12 listrange gate: on/off" INFO). L3 (LISTWAIT default) is the
  results commit's call: needs "would (differ 0)" over the whole race.
- Portability: no intrinsics, no new .cpp; casts for ptrdiff_t; names checked for shadowing.
