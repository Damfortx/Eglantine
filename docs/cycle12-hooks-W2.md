# Cycle 12 W2 hooks (committer whole-draw repeat)

W2 needs no lines in another agent's file. What it relies on, for the coordinator to keep when
merging:

## From W3 (packet layout, already in the tree)
- `DrawHeader::c12_repeat` (u32) and `DrawHeader::c12_flags` (u32), `CompactDrawHeader::c12_repeat`
  (u32) and `CompactDrawHeader::c12_flags` (u8), set by EmitDraw in both layouts (0 when not a
  repeat). Meaning used by W2: seq distance to the same pipeline's earlier packet whose capture
  pass 1 was byte-identical; W2 only uses it with `c12_flags & C12SameReads` (bit 0).
- W2 reads them only through templates in `mt/c12_commit.h` (`C12SameAsDelta`,
  `C12CopyRepeatFlag`), so a rename or removal does not break the build (the flag then just reads 0).

## ParsePacket (W2's function), compact layout
W2 added, right after `header.repeat_delta = packed.repeat_delta;`:

    VideoCommon::MT::C12CopyRepeatFlag(header, packed);

It copies `c12_repeat` and `c12_flags`. W3's own ParsePacket hook (`PacketDecoder::C12ApplyRef`,
docs/cycle12-hooks-W3.md, for EDEN_NX2_C12_CAPTURE_REF) goes in the same function and does not
conflict: put it where W3 says (after the decode); the flag copy above stays before the cursor
line.

## No epoch hooks needed in W4's files
The new buffer cache epoch `c12_buffer_epoch` is bumped in `BufferCache::ChangeRegister` (the only
writer of the buffer page table: every creation and deletion), in buffer_cache.h (W2). The texture
cache needs no bump (the texture half keeps the cycle 11 REPEAT checks: view epoch, bindings
serial, CPU-modified flags, layout generation).
