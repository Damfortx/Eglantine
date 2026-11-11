# Cycle 11 W3 "VRAM diet": implementation notes

Not built, not run (rules). Everything lives behind the switches in the new header
`src/video_core/mt/c11_vram.h` (backend neutral, no Vulkan types). The startup line is logged
once, at the first VRAM tick:
`Cycle 11 VRAM: drop1x .. (age .., check ..), decode_trim .. (.. frames), coord .. (margin .. MiB, hot .. frames), ramtier .. (.. MiB), category stats ..`

## Switches and defaults

| piece | switch | default | knobs |
|---|---|---|---|
| V1 drop 1x originals | `EDEN_NX2_C11_DROP1X` | on under `EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1` ('1' on without) | `EDEN_NX2_C11_DROP1X_AGE` (300 frames), `EDEN_NX2_C11_DROP1X_CHECK=1` (log each re-creation, first 128) |
| V2 decode class trim | `EDEN_NX2_C11_DECODE_TRIM` | on when `EDEN_NX2_ASTC_DISKCACHE=1`, else off | `EDEN_NX2_C11_DECODE_TRIM_FRAMES` (600) |
| V3 per-category accounting | `EDEN_NX2_VRAM_STATS=1` (existing) | off | category walk every 120 frames |
| V4 relocation / emergency coordination | `EDEN_NX2_C11_VRAM_COORD` | on under MT lite | `EDEN_NX2_C11_VRAM_COORD_MARGIN_MB` (256), `EDEN_NX2_C11_VRAM_COORD_HOT` (60 frames) |
| V5 RAM tier | `EDEN_NX2_C11_RAMTIER` | off (also needs `EDEN_NX2_ASTC_DISKCACHE=1`) | `EDEN_NX2_C11_RAMTIER_MB` (384, clamped 64..2048) |

`=0` on any of them is the cycle 10 code path. 'a' does nothing useful (read once, structural state).

## What was implemented

### V1 DROP1X (vk_texture_cache.cpp/.h Image, texture_cache.h C11VramTick)
- `TextureCache::C11VramTick()` (called from TickFrame just before `runtime.TickVram`) runs a sweep
  every 30 frames over `slot_images`. It looks at registered rescaled images that are not CPU-modified
  and whose aliases are all rescaled and not CPU-modified, and calls `Image::C11DropOriginal(age)`.
- `Image::C11DropOriginal` accepts only images that have stayed rescaled with no ScaleDown (download,
  upload, blacklist or copy) for `age` runtime frames. The image must also be a plain single-sample 2D image
  that BlitScale handles (`!NeedsScaleHelper()`, so no scale/normal helper views or framebuffers
  hold the 1x handle), with no sparse, unbacked, trimmed, converted, accelerated-upload,
  decoding, bad-overlap or dead flags, and no storage views. It erases the MSAA resolve shadow
  of the 1x handle, moves the 1x `vk::Image` into `TextureCacheRuntime::c11_dropped_originals`
  with `scheduler->CurrentTick()`, and resets `original_residency`. Runtime `TickFrame` destroys the
  image once `IsFree(tick)`.
- `Image::ScaleDown` re-creates a dropped 1x image (`C11RecreateOriginal`) with the same `MakeImage`
  call the constructor makes for these images, then blits scaled -> 1x. This covers every 1x
  consumer, because downloads, uploads (`ScaleDown(true)`), blacklisting, alias copies and
  `CopyImage` all go through `ScaleDown` first. The blit also happens for the upload path
  (`ignore=true`), so a partial upload keeps the rest of the image current instead of garbage.
  The cycle 10 code kept a stale 1x image there.
- `Image::MemoryBlock()` falls back to the scaled image when the 1x one is dropped, so the
  collector's block ordering still works. The "Missing image at download" critical log skips dropped images.
- Counters: dropped, MiB dropped, re-created (upload / scale-down), MiB re-created, refused (sweep
  candidates refused on each sweep, so the same image counts once per sweep). The current
  dropped count is in the V3 category line.

### V2 DECODE_TRIM (vk_texture_cache.cpp TickVram)
- Every 60 runtime frames, every persistent decode class (any size) unused for 600 frames and
  with `scheduler.IsFree(decode.tick)` is erased, which frees the image and its views. The next disk cache miss
  of that class allocates it again: allocation was already lazy (`persistent_decodes[key]` on
  first use). The batch 8 `EDEN_NX2_VRAM_DECODE_TRIM` (3600 frames, >= 16 MiB) is unchanged and
  still works on its own. Counted: classes trimmed and MiB.

### V3 categories (texture_cache.h C11VramTick, vk_texture_cache.cpp TickVram, vulkan_memory_allocator.cpp TickVramStats)
- Every 120 frames, with `EDEN_NX2_VRAM_STATS=1`, a walk over the images fills
  `C11Vram::g_categories`. Categories: render targets 1x (GPU-modified, not rescaled), 2x (scaled
  copies), rescale originals (kept 1x images) plus the count dropped, ASTC BC7 vs BC1/BC3 (through
  `runtime.TranscodeBcMode`), other sampled, sparse bound, and how much of all that sits in host
  memory. Sizes are real VMA allocation sizes (`C11OriginalBytes`/`C11ScaledBytes`). The runtime
  adds decode class bytes, transcode scratch bytes and the MSAA scratch count.
- `TickVramStats` now sums the device-local heaps' VMA allocation bytes and calls
  `C11Vram::LogStats`, which prints two lines every 10 s:
  `C11 VRAM categories (MiB, ...)` with the remainder "buffers/staging/other" (VMA device-local
  allocations minus the categories), and `C11 VRAM in 10 s:` with the V1/V2/V4/V5 counters.
  Buffers are not instrumented separately: buffer_cache is W4's.

### V4 coordination (texture_cache.h RelocateHostImages, RunGarbageCollector)
- Relocation of host-resident images (cycle 10 A) runs only when `used + margin < critical`, and
  the room it uses is `critical - margin - used`. Skipped passes are counted ("relocation skipped
  for the margin").
- The emergency pass (costly images not spared, `vram_emergency`) raises `ticks_to_destroy` to
  `CoordHotFrames` (60). Images used in the last 60 frames are no longer evicted by it, because
  evicting them would only re-create them the next frame. Counted per pass.

### V5 RAMTIER (vk_texture_cache.cpp T2 block)
- `C11Vram::RamTier` is an LRU of BC byte blobs keyed by the disk cache key (lo/hi), capped at
  `EDEN_NX2_C11_RAMTIER_MB`. A single blob is at most a quarter of the cap. It lives in
  `AstcDiskCacheState` and is only touched on the runtime's thread.
- In `TryTranscodeFromDiskCache`, after the hash and before the file lookup, a RAM hit uploads
  directly. Blobs are inserted after a disk hit and when a miss's readback completes
  (`PollAstcDiskCache`, before `Submit`). A RAM hit does not count as a disk cache hit:
  look at the ramtier hits in the C11 line.

### W4 hook: CPU-modified epoch (texture_cache_base.h, texture_cache.h)
- `std::atomic<u64> cpu_modified_epoch` with the getter `TextureCache::CpuModifiedEpoch()` (acquire load).
  It is bumped with a release `fetch_add` once per call of:
  - `WriteMemory`: when any image is under the write, on every path (UnwrittenSparse,
    SparseTiles ranges, layers, CpuModified). This is where `RasterizerVulkan::InvalidateRegion`
    lands.
  - `UnmapMemory`: when any image is deleted.
  - `UnmapGPUMemory`: when any image is marked CpuModified/Remapped.
- W4 compares it unchanged -> no RefreshContents needed for CPU writes since the memo. It does not
  cover GPU-side changes (rescale, alias sync); those move `view_epoch` / the image version.

## Risks
- V1: a 1x consumer that bypasses `Image::ScaleDown` and reads `original_image` while rescaled.
  I found none: the sparse paths are refused, and DownloadMemory, UploadMemory and copies scale down first.
  If textures go black or stale, set `EDEN_NX2_C11_DROP1X=0` and check `EDEN_NX2_C11_DROP1X_CHECK=1`
  for which images re-create.
- V1: a collector eviction of a dropped image that must be downloaded first now allocates the 1x
  image again just to download it, at a moment of pressure. The "re-created ... scale-down" count
  shows it.
- V1: the texture cache's own estimate of `total_used_memory` (only used without
  VK_EXT_memory_budget) does not subtract dropped originals. That is conservative. With the
  extension, usage comes from the driver and goes down by itself.
- V1 re-created images start in UNDEFINED layout and are blitted with `oldLayout=GENERAL`. This is the
  same pattern the existing code uses for a fresh `scaled_image` in ScaleUp.
- V2: a class trimmed right before a burst of misses costs one allocation per class (a 20-200 ms
  allocation stall when the heap is full).
- V4: the hot-frame floor makes emergencies end more slowly when everything is hot. The emergency
  still ends at `< critical`, and the "emergency passes keeping hot images" counter shows how often it happens.
- V5: RAM use up to the cap plus one transient copy, and the user's RAM is often full. That is why
  it is off by default.

## What to look at in the test round
- `C11 VRAM categories`: texture cache device memory should be under critical with ~300 MiB margin.
  Rescale originals MiB should go down after DROP1X and the dropped count up. Decode classes should be
  ~0 after 600 quiet frames with the disk cache on.
- `C11 VRAM in 10 s`: re-created/s should be ~0 in the race (a steady stream = churn: raise AGE
  or turn it off), and trim should reach a steady state.
- TC_CHURN evictions/s and ASTC re-created/s against B+c10 (target: far fewer), disk cache
  hits and hash ms per 10 s, stalls > 100 ms.
- Screenshots in the race window (HUD, water, mirrors, minimap: render targets read back or copied
  into 1x images are the DROP1X risk).
- Host-resident must stay 0, and relocations should only happen with the margin.
