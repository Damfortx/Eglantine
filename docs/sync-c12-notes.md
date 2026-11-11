# Cycle 12 over the upstream-synced base (branch mkw-c12s, 2026-11-10)

`git merge mkw-c12` (fb1d9ed529, cycle 12 W1/W2/W3) into mkw-sync (54ce6fbeaa = mkw-c11 + merge
of origin/nx2, see sync-nx2-notes.md). Merge base caa189ca66.

## Conflicts

Only one textual conflict:

- vk_graphics_pipeline.h, GraphicsPipeline members: upstream's `sampled_images` (for
  SampledImages()) and cycle 12's `c12_buffer_memo` both kept.

Everything else auto-merged (buffer_cache.h/_base.h, dma_pusher.cpp/.h, kepler_compute.cpp,
maxwell_3d.cpp, gpu_split.cpp, c11_front.h, vk_graphics_pipeline.cpp, vk_mt_pipeline.h,
vk_mt_seq.h). Checked by hand:

- `compute_ssbo_memo` stays sized NUM_COMPUTE_STORAGE_BUFFERS (64); graphics memo unchanged.
- dma_pusher.cpp: upstream's "entry parsed from memory differs" diagnostic (skipped for
  GPUPARAM) and cycle 12's census/LISTWAIT/PRE-READ paths are in different spots, both present.
- Bank re-upload (EDEN_NX2_BANK_REUPLOAD, under MT::TrackerScope) lives in
  BindHostGraphicsUniformBuffer, after UpdateGraphicsBuffers, so W2's buffer memo does not skip
  it.

## W2 buffer memo vs upstream storage-buffer sizing

`C12UpdateGraphicsBuffers` compares each enabled storage binding's derived (device_addr,
gpu_addr, size) with the memo before reusing its buffer id. Those are the *output* of
BindGraphicsStorageBuffer / MemoStorageBufferBinding, which now apply upstream's rules (table
pointer through `indirect_offset`, native_turing = bare/mapping-sized pointer, 64 MiB / 8 MiB caps,
MaxContinuousRange clamp). Any change in the sizing inputs (holder pointer, native flag, size
word, mapping size) changes the derived binding and the memo misses; mapping changes also bump
the layout generation, which the memo checks. is_written is covered by the
`written_storage_buffers` masks. The resolution itself (FindBuffer + ResolveMultiRangeStorage)
depends only on those fields, and any buffer register/unregister bumps `c12_buffer_epoch`
(ChangeRegister). No code change was needed.

## W3 capture memo vs upstream storage bindings

The graphics capture's pass 1 now reads the table holder (`SeqRead`, so it lands in the memo's
reads/pages and is validated byte-for-byte) and reads the size word only when
`!(table && !written) && !native_turing`. `table_pointer` is per descriptor (fixed for the
pipeline); `native_turing` is engine state that changes which reads pass 1 makes and how the
binding is sized, and it was not in the memo key. **Added** `native_turing` to C12BuildKey
(vk_mt_seq.h). Mapping-dependent sizing (GetMemoryLayoutSize / MaxContinuousRange) is covered by
the memo's layout generation.

## Risky spots

- W2/W3 counters and checks were measured on the c11 base only; on this base upstream's 64 MiB
  table-pointer bindings (OoT) may make more multi-range storage results, which W2 never memoizes
  (segment_count != 0) - only a hit-rate concern.
- W3 key change means the memo also misses whenever native_turing flips (never for MKW).
