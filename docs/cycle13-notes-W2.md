# Cycle 13 W2 notes: bank re-upload under MT / VERIFY fix

## What was implemented

New header `src/video_core/mt/c13_bank.h` (namespace `VideoCommon::MT::C13Bank`): switches,
counters, the sequencer's per-frame bank set, the VERIFY uncaptured-sync census and the
"C13 bank (10 s)" line.

- `buffer_cache.h` `BindHostGraphicsUniformBuffer` (bank block only): `EDEN_NX2_BANK_REUPLOAD`
  now read via `C13Bank::ReuploadMode()`. Mode `'0'` = the merged block verbatim. Otherwise:
  under MT (`MT::t_capture != nullptr`, committer recording a packet) the committer never marks
  and never takes the tracker lock for the bank (only counts; under CHECK it tests whether the
  bank is still CPU-modified). Without a capture (single thread, or MT Serial draws) the size test
  and the per-frame set test (`contains`) run first; TrackerScope + IsRegionGpuModified only for a
  bank not yet seen this frame (same GPU-modified semantics as before: a GPU-modified bank is not
  put in the set).
- `buffer_cache_mt.h` `MtC13CaptureBank` (new, sequencer): once per committer frame
  (`g_hot_page_epoch`, bumped by BufferCache::TickFrame) per 64 KiB native bank, under the
  (recursive) tracker lock: GPU-modified test, then `BumpWriteGeneration(..., true)` +
  `MarkRegionAsCpuModified` + `NoteGuestWrite` + the stale-vertex note, exactly like
  `MtTrapWrite`. The `SeqTakeUploads` that follows in the capture takes the bytes into the packet;
  the committer applies them via `MtApplyUploads`. Modes 2/3 of BANK_REUPLOAD (every bind) are
  honoured on the sequencer too.
- `buffer_cache_mt.h` `MtC13ClaimCpuWrites` (new, B2): `ClaimCpuWrites` for the MT sequencer's
  invalidations; caller holds `buffer_cache.mutex`.
- `buffer_cache.h` `SynchronizeBuffer`: VERIFY's uncaptured-sync branch feeds the CHECK census
  (`NoteUncapturedSync`); behaviour otherwise unchanged.
- `buffer_cache.h` `TickFrame`: calls `C13Bank::TickFrame()` (stats line every 10 s).
- `buffer_cache_base.h`: include of `c13_bank.h`, declarations of the two new functions, member
  `c13_seq_banks` (sequencer-only set).
- Hooks for D's `vk_mt_pipeline.h`: `docs/cycle13-hooks-W2.md` (H1 required for B1 to do the
  re-upload under MT; H2 for B2).

## Switches and defaults

| switch | default | values |
|---|---|---|
| `EDEN_NX2_C13_BANK` | `1` | `1` sequencer takes the bank (MT) / reordered tests (single thread); `0` merged path; `s` no re-upload under MT (c11 behaviour), single thread as `1` |
| `EDEN_NX2_C13_BANK_CHECK` | off | `1`: uncaptured syncs by address (top 4), how many on a bank marked this frame (+ rate-limited ERROR "C13 bank check: uncaptured sync on a re-uploaded bank"), banks still CPU-modified at the committer's bind |
| `EDEN_NX2_C13_CLAIM` | `0` | `1` (needs EDEN_NX2_MT): ClaimCpuWrites in SeqInvalidateBuffers (H2) |
| `EDEN_NX2_C13_CLAIM_CHECK` | off | `1`: calls / ranges claimed in the stats line |
| `EDEN_NX2_C13_STATS` | off | `1` (or any of the two CHECKs): "C13 bank (10 s)" line |

`EDEN_NX2_BANK_REUPLOAD=0` still turns the whole feature off in every mode. Startup line:
"C13 bank: mode ..., reupload ..., MT ..., check ..., claim ... , stats ..." (logged on first use).
No `a` mode: the bank state (CPU-modified bits) does not allow per-frame alternation cleanly.

## Risks

- If H1 is not applied, mode `1` under MT behaves like `s` (no re-upload under MT): the stats
  line then shows "sequencer banks 0" with "left to MT" > 0. MKW fine; Ocarina may flash.
- Re-upload timing: the sequencer's frame is the committer's epoch (lags the sequencer by the
  ring backlog); at most one frame of difference, same as single-thread mode 1.
- The mark on the sequencer makes VERIFY treat later CPU-modified hits on that range as
  "concurrent" (NoteGuestWrite), but the following take clears the bits in the same capture, so
  this should not hide anything; the CHECK census is independent of that classification.
- A draw path that skips the uniform loop (a future memo) would delay the re-upload to the next
  draw running the loop.
- B2 claims ClearDownload/subtract GPU ranges for CPU-written ranges before WriteMemory; on the
  sequencer only for ranges without GPU data on their pages / under the mutex path, all regions on
  the committer in the owner Serial. Off by default.
- Not compiled (rules): watch for template/name issues in `buffer_cache_mt.h`
  (`g_hot_page_epoch` from word_manager.h, `Core::DEVICE_PAGESIZE/PAGEMASK` as in the old block).

## What to look at in the test round

- Run 1 (c13-chk, add `EDEN_NX2_C13_BANK_CHECK=1`): MT_VERIFY mismatches back to 0-225 per run;
  "C13 bank" line: sequencer banks ~2/frame (two 64 KiB banks, ~128 KiB/frame), committer banks 0,
  "uncaptured syncs" ~0 and "on a bank" 0, "dirty at commit" ~0. If uncaptured syncs remain at
  other addresses/sizes, rerun with `EDEN_NX2_C13_CLAIM=1 EDEN_NX2_C13_CLAIM_CHECK=1` (needs H2).
- Committer busy / idle vs C12 (est. -3-8 % busy); "tracker walks" vs "set skips" on any
  single-thread run.
- Fallback: `EDEN_NX2_C13_BANK=0` (C12 path) or `=s`.
