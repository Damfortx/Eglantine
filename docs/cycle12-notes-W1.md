# Cycle 12 W1 "front drains, second pass": notes

Not built, not run (rules). Start line `Cycle 12 front: posadj ... listrange gate ... listwait ...`
(once, GpuSplit::MaybeStart). Stats line every 10 s `C12 front per 10 s: ...` (DmaPusher::
DispatchCalls) with EDEN_NX2_C12_STATS=1, GPU_PROFILE, or any C12 check / LISTWAIT / LISTRANGE
gate switch. All code in mt/c12_front.h + W1 files; no hooks needed elsewhere.

## Switches

| switch | default | what |
|---|---|---|
| EDEN_NX2_C12_POSADJ | on under MT lite (`0` = c11 path, `a` per read) | P1 per-byte overlay rule |
| EDEN_NX2_C12_POSADJ_CHECK=1 | off | P1: also wait + compare every word; waited words used |
| EDEN_NX2_C12_LISTRANGE=1 | off | P2 gate: turns C11 LISTRANGE on with probation check |
| EDEN_NX2_C12_LISTRANGE_CALLS / _PCT | 20000 / 30 | P2 gate length / "shorter" threshold |
| EDEN_NX2_C12_LISTWAIT=1 | off (contingent) | P3 same-content speculation (no rewind, see risks) |
| EDEN_NX2_C12_LISTWAIT_CHECK=1 | off | P3 content census: pre-read before each writer wait, compare |
| EDEN_NX2_C12_LISTWAIT_TRUST | 64 | P3 consecutive "same" results before a list is trusted |
| EDEN_NX2_C12_STATS=1 | off | the C12 line (also P3 kind/program census) |

## P1 POSADJ (gpu_split.h C12KnownPendingWords, maxwell_3d.cpp FrontDmaRead)
- Root cause found in code: the c11 rule refused a read that reaches past a direct 16-byte
  release's known words even when the excess bytes lie PAST THE END of that release (bytes
  16-23 of the `macro 0x0 +0 24B of 16B direct` shape). Those bytes are not that write's.
- New rule: a note refuses only if the read covers bytes of that note whose value is unknown.
  Direct 16-byte releases are fully known: payload, 0, and words 2-3 = front GetTicks() (the
  POSFWD value; the sequencer writes a later tick). Bytes past them come from other notes (laid
  in note order: a neighbouring direct release lays its own payload) or from memory when no
  queued note covers them. Caller still refuses GPU-written pages, writers, drain-known notes,
  table overflow, open I2M; notes without a value still refuse (refusal 1).
- Stats: posadj reads / served (= c11 rule would have waited) / residual refusals, and "past
  bytes" census for reads the c11 rule calls past-known: [queued note tag T direct? same/other
  channel adjacent?] / [settled note] / [unnoted]. PendingWrite has a new `channel` field for it.
- CHECK: for served reads, WaitForPosition(position, position) then compare every word.
  "payload mismatch" (must be 0, ERROR "C12 posadj check") vs "timestamp mismatch" (words 2-3 of
  a direct release: EXPECTED nonzero, front clock is earlier).

## P2 LISTRANGE gate (c11_front.h C12Shared, c12_front.h C12ListRangeGate, gpu_split.h
WaitForListWriters, dma_pusher.cpp C11 check block)
- No data round, so the plan's gate runs in-run: EDEN_NX2_C12_LISTRANGE=1 forces C11ListRange
  on and C11ListRangeCheck on during probation (shortened fetches re-read, second read used).
  After 20000 list calls: on if shorter > 30 % of calls and 0 stale, else off (c11 sequencer
  target; F2 bookkeeping keeps running). Any stale -> off at once. One INFO line
  "C12 listrange gate: on/off (...)"; state also in the C12 line. Needs C10_LIST_PRECISE (on
  under LITE) and the two c11 hooks (present). Default left off (plan: flip only on data).
- No refusal-class fix (stale cbuf / unknown program): no data shows one.

## P3 writer census + LISTWAIT (dma_pusher.cpp ReadGpuWrittenCommands / C12VerifyListSpec,
kepler_compute.cpp dispatch callback, gpu_split.h NoteWriter)
- Census (C12_STATS): GPU-written list fetches that find their writers queued, by last writer
  kind (compute / dma copy / memory=I2M / 2d / 3d), wait count and ms, top compute programs
  (QMD code address of the last mirrored dispatch of the writer call; 0 = not mirrored).
- LISTWAIT_CHECK: list read before the wait too; "same / changed / unreadable" per kind (the
  plan's > 90 % "same content" criterion), per-list trust built, and "would N (differ M)" =
  what LISTWAIT would have done (M must be 0 before ever turning LISTWAIT on).
- LISTWAIT (off): a trusted list (same content 64 times in a row, never changed) skips the
  writer wait; one speculated read at a time is verified on the front once consumed/settled
  pass the writers target (re-read + hash). Mismatch with no later writer = ERROR "C12 listwait
  verify" + ban; with a later writer = "inconclusive" + ban. NOT UNDONE: the plan's rewind is
  not implementable here (methods reach the ring at publish_every; front shadow state is
  mutated while parsing). LiteListCheck re-reads speculated lists too; L25 trust is not fed by
  speculated reads.
- Plan said "census only unless >= 100 waits/s of one kind": LISTWAIT is the off-by-default
  contingent part; run LISTWAIT_CHECK first.

## Risks / what to look at in the test round
- P1 is default ON under LITE: CHECK run must show payload mismatch 0; timestamp mismatches are
  expected. If the macro actually uses words 2-3 (MKW timing logic), behaviour may differ
  subtly (same risk POSFWD had): compare with POSADJ=a, watch fps/frame pacing and screenshots.
  Expected: "posfwd past-known" (C11 line) falls from 59-96/s to ~0, lite6 position waits drop,
  "served" ~= old past-known rate. If the census says [queued note ... no value] the read still
  waits (refused no value): then the neighbour is a counter report, next step would be its value.
- P2: read the gate line; if "shorter too rare", F2 is not worth it.
- P3: LISTWAIT_CHECK costs one extra list read per waited fetch (may wait for a GPU copy
  tick): run it only in a check run. LISTWAIT must not be turned on without differ = 0 over
  whole races (a stale culling list = billions of instances / device loss).
- Per-call cost with all off: one flag test in NoteWriter, the per-read GetTicks with POSADJ on,
  one extra KnownPendingWords loop per successful overlay read (to know "served").
