# Cycle 12 W3 notes: sequencer capture repeat with write generations

Not built, not run (rules). Files: new `src/video_core/mt/c12_capture.h`; `vk_mt_pipeline.h`
(include, DrawHeader / CompactDrawHeader fields, C12 members, CaptureDraw, EmitDraw);
`vk_mt_seq.h` (the C12 functions, appended at the end); `mt/packet_compact.h`
(`PacketDecoder::C12ApplyRef`). Hooks for others: docs/cycle12-hooks-W3.md.

## What it does

CaptureDraw's pass 1 (storage descriptors and texture/image handles read from constant buffers,
TIC/TSC entries, then MtStorageBinding / SeqIsContinuous / SeqTicValid / the C11 key) is a pure
function of a register key and of the guest bytes it reads. Per pipeline (1024 direct-mapped
slots) the sequencer keeps the last pass 1: reads in capture order + raw bytes, ssbo_entries,
the storage bindings, tic_valid, the C11 repeat key, and per read its device page / offset.

Next capture of the same pipeline (C12Begin, before pass 1):
1. same memory manager and page table layout generation, same key (TIC/TSC addresses and
   limits, sampler binding, native tiling, manager, and address + enabled of every constant
   buffer the pipeline's descriptors read from; all 18 of a stage with a handle program);
2. C12Validate, per memo page (<= 32 pages, reads <= 4096, no read crosses a 4 KiB page):
   - GENS: under the tracker lock, a page that is protected (not CPU-modified, not a query
     page) with the generation armed at the last validation (same epoch) is skipped;
   - every other page: memcmp of its reads against the kept bytes through the host pointer
     (exact: the same bytes pass 1 would read). Its generation is read before the compare and
     armed when the compare passes.
   A page found unprotected 8 times in a row is not asked again (no lock) except every 64th.
3. Hit: pass 1 is skipped (the stage loop runs 0 stages) and C12AfterPass1 restores its
   results; pass 2 (tracker, uploads, uniforms, written ranges) and FinishOverlay* run as before.
   Miss: full pass 1, then the memo is recorded (translations, page list, and the bytes compared
   once through the host pointers: a shim read or a concurrent write gives no memo). A slot with
   8 misses in a row only validates/records every 16th capture.

Packet layout (W3 owns it; W2 already reads it through `C12CopyRepeatFlag` / `C12SameAsDelta`):
- `DrawHeader::c12_repeat` (u32) + `c12_flags` (u32); `CompactDrawHeader::c12_repeat` (u32),
  `c12_ref_slot` (u16), `c12_ref_op` (u8), `c12_flags` (u8). The compact header grew from 16 to
  17 words (the old one had no padding left). Words are derived from sizeof everywhere.
- `c12_repeat` = seq distance to the same pipeline's earlier packet whose pass 1 was
  byte-identical (the memo's last packet; 0: none). `c12_flags` (MT::C12PacketFlags):
  SameReads | SameSsbo | SameTicValid always with c12_repeat, plus SameGeometry (index/vertex
  entries), SameUboBinds (stage, slot, size, address, cached/streamed of every uniform entry),
  NoUploads (this packet carries none). On a throttled or 'a'-off capture in between, the
  distance points past it (W2's memo-by-seq then just refuses).

Contingent piece CAPTURE_REF (off, and compiled out of use by `kC12CaptureRefWired = false`):
a hit whose memo's overlay the readers stored goes with an empty overlay (FinishOverlay skipped,
no inserts on either side), `c12_ref_op = Use`; the first compact packet after a record is
`Store`. Needs hook 2 (one line in ParsePacket). Not with resolvers.

## Switches (all need EDEN_NX2_MT=1; read once)

| switch | default | meaning |
|---|---|---|
| EDEN_NX2_C12_CAPTURE_REPEAT | on under LITE; `0` = cycle 11 capture; `a` per capture | the repeat. Inactive with MT_VERIFY, lockstep, resolvers |
| EDEN_NX2_C12_CAPTURE_REPEAT_GENS | on under LITE when REPEAT is not 0; `0` = memcmp only (exact); `a` | generation skip; turns the generation tracking (g_ubo_gen_on) on, not UBO_GEN's uniform reuse |
| EDEN_NX2_C12_CAPTURE_REPEAT_CHECK=1 | off | hits run pass 1 too and compare reads/bytes, ssbo entries and bindings, tic_valid, C11 key; skipped pages are compared too ("gen stale"); full results used |
| EDEN_NX2_C12_CAPTURE_REF=1 | off (inert until wired) | overlay by reference |
| EDEN_NX2_C12_STATS=1 (or MT_STATS=1) | off | "C12 capture (10 s)" line |

Startup line: "Cycle 12 capture: repeat .., gens .., check .., ref .. (wired ..), stats .."
(first capture).

## Test round: what to look at

- "C12 capture (10 s)": hit % (c11 found 42-59 % of textured captures identical; ours also needs
  the storage descriptors and the key equal), misses by class, validate ms/s, pages gen-skipped
  vs compared vs unprotected. If "unprotected" ~ "compared" the generations buy nothing (TIC/TSC
  table pages are likely never uploaded by the buffer cache, so never protected) and GENS=0 is
  the cheaper setting (no tracker lock per hit, no bumps on the write paths).
- CHECK run: `mismatches` and `gen stale` must be 0 for a whole race, MT_VERIFY's concurrent CPU
  writes 0. Note: with EDEN_NX2_MT_LITE7_DESCDEDUP=a the read list differs per capture and CHECK
  reports "read count" mismatches that are not bugs; keep DESCDEDUP at its default for CHECK.
- Sequencer "draws" zone ms/s vs c11 (A/B: CAPTURE_REPEAT=a, or whole runs 0 vs default) and
  draws/frame (the game might change pattern).
- Packets: +8 bytes per compact draw header.

## Risks

- GENS: a guest write that bypasses the tracker and the generation bumps (e.g. a texture cache
  download into a page the buffer tracker protects) would make a skipped page stale. CHECK's
  "gen stale" is the alarm; GENS=0 removes the risk entirely (memcmp validation is exact).
- Pass 1 inputs missed by the key: everything it reads from registers is in it as far as read
  (tables, limits, sampler binding, tiling, manager, constant buffer addresses/enabled, layout
  generation); MtStorageBinding / SeqIsContinuous / SeqTicValid depend only on the page table.
  CHECK compares the derived results too.
- Memory: up to 1024 memos of a few hundred bytes to a few KiB each.
- Uploads and uniform bytes were not memoized (they need the tracker every draw; uniforms
  already have UBO_GEN / compact patches).
