# Cycle 13 W1 notes: VRAM pressure signal, hot relocation, render-target diet

Not built, not run (agent rules). Everything is behind `EDEN_NX2_C13_*` switches, all **off by
default** ('1' on; anything else off; 'a' not supported: structural state). New header:
`src/video_core/mt/c13_vram.h` (backend neutral). Startup line (first VRAM tick):
`Cycle 13 VRAM: oom .. (check ..), reloc .., rt diet .., staging trim .., stats ..`.

## Files / functions touched (all W1-owned)

- `mt/c13_vram.h` (new): switches, counters, gauges, failure log, `OomState`, `DietShape`,
  `NoteFrame`, `TickStats`, `LogStart`, staging census struct.
- `texture_cache/texture_cache_base.h`: include + members `c13_oom`, `c13_old_margin`,
  `c13_budget`, `c13_floor`, `c13_old_critical`, `c13_new_critical`, `c13_reloc_backoff_until`.
- `texture_cache/texture_cache.h`: `TickFrame` (V1 rule switch, floor, shadow criticals,
  per-frame accounting + stats line), `RelocateHostImages` (restructured: shared `evict` lambda,
  C13 path first, cycle 10/11 path unchanged after it), `C11VramTick` (floor walk + RT census at
  the end), `ImageCanRescale` and `ScaleUp` (diet guard).
- `renderer_vulkan/vk_texture_cache.h/.cpp`: `TextureCacheRuntime::C13AllocatedBytes`,
  `C13DeviceVmaBytes`, `c13_retired` / `c13_checks` deques (drained in `TickFrame`), LogStart call
  in `TickVram`; `MakeImage(..., c13_device_only)`; `Image::C13Rebind`, `Image::C13HostBytes`.
- `vulkan_common/vulkan_memory_allocator.h/.cpp`: `C13CreateDeviceImage`, `C13NoteDeviceFull`
  (called at the device-full points of `CreateImage` / `CreateGuardedBuffer`), census line in
  `TickVramStats`.
- `renderer_vulkan/vk_staging_buffer_pool.h/.cpp`: `StagingBuffer::c13_trim`, `C13Trim`,
  `C13Census` (from `TickFrame`).

## V1 `EDEN_NX2_C13_OOM` (+ `_CHECK`)

- Allocator: at each device-full failure (guard path: strict device-local failed past the budget;
  no guard: the first "over the memory budget" failure; buffers: `CreateGuardedBuffer`) it records
  `Device::GetDeviceMemoryUsage()`, `GetDeviceMemoryBudget()` and the failed size (images: the host
  allocation's size once known, else texels x 4). Only when OOM or OOM_CHECK is on.
- Texture cache (`OomState::Tick`, every frame from TickFrame once native thresholds apply):
  failures < 60 frames apart = one episode; margin = running max over the episode of
  `usage + failed - budget + 256 MiB` (>= the margin before the episode), capped by
  `EDEN_NX2_C11_PRESSURE_CAP_MB`. Applied at once (a max, not a sum: no ratchet). Load episodes
  (driver allocations > 1 GiB/s over 30 frames, or a frame > 100 ms, during the episode) hold it
  until 300 frames after the last load frame, then drop back to the pre-episode margin in one
  step (plan says 0; pre-episode margin is 0 unless an earlier non-load episode left one). Else
  decay 256 MiB / 120 frames while `usage + margin < budget`.
- Floor (OOM on): every 120 frames `C11VramTick` measures device-resident GPU-written image bytes
  + (VMA device-local allocation bytes - device-resident image bytes) + (driver usage - VMA block
  bytes); `critical = max(critical, min(floor + 512 MiB, budget - 256 MiB))` at each budget check
  (expected raised by the same amount). Env overrides (`EDEN_NX2_TC_*_MIB`) still win.
- The cycle 12 rule keeps running: applied with OOM off, as a shadow (`c13_old_margin`, no log,
  no recompute) with OOM on. CHECK logs each episode: `C13 oom episode: failures N, MiB,
  usage/budget at the worst failure, deficit, frames, load; old margin a -> b, new margin c -> d`,
  load-hold releases, and counts frames over one rule's critical but not the other's
  (`old only` / `new only`) and `refails` (failures within 300 frames after a close or drop:
  must be 0 in the race). With OOM off + CHECK, the new rule is the shadow.
- Known gap: `EDEN_NX2_VRAM_SLOWPRESSURE` (off by default) feeds only the cycle 12 rule.

## V2 `EDEN_NX2_C13_RELOC` (+ `_CHECK`)

- Needs the cycle 10 guard (residency tracking). Each frame, if host-resident images exist and
  no back-off: pick the largest host-resident registered, non-decoding image (hot ones too).
  Room: `total_used_memory + its host bytes + 512 MiB < last budget read`, else counted
  `no room`.
- GPU-written images: `Image::C13Rebind` allocates device-local memory within the budget only
  (`MemoryAllocator::C13CreateDeviceImage`, no host fallback, no pressure event), for each host
  part (1x original / scaled copy), then records `vkCmdCopyImage` (all levels/layers, GENERAL ->
  TRANSFER -> GENERAL), retires the old backing plus every storage / helper view and helper
  framebuffer (destroyed once the GPU passes the tick), resets the residency. Then the cache calls
  `InvalidateScale(image)` (views, framebuffers, render-target ids, version, view epoch, table
  invalidation: the rescale path). Allocation failure -> 120-frame back-off. Refused (sparse,
  converted/ASTC, trimmed levels, decoding) -> cycle 10 evict path.
- Other images: cycle 10 evict path (download if downloadable, one a frame), then a 2-frame pause.
- CHECK: two 64x64 tiles (corner, centre) of level 0 / layer 0 of old and new read into a
  deferred download buffer (zeroed first), compared in `TextureCacheRuntime::TickFrame` once the
  GPU is done; `check tiles` / `mismatches` counters, first 32 as `C13 reloc check` errors.
  Single-sample, non-block-compressed only; depth formats compare the depth aspect.
- Counters also feed the cycle 10 "relocated" line.

## V3 `EDEN_NX2_C13_RT_DIET` (default off; census with `EDEN_NX2_C13_STATS`)

- `DietShape`: 2D, single-sample, square, side in `EDEN_NX2_C13_RT_DIET_MIN..MAX` (1024..8192),
  format D32 / D16 / X8D24 / D24S8 / S8D24 / D32S8 / R32F / R32UI / R32SI. Diet on: refused in
  `ImageCanRescale` (before the Rescaled early-out, so aliases follow) and in
  `TextureCache::ScaleUp` (counted `scale-ups refused`).
- Not implementable here without hooks: "only sampled", "framebuffer has no other attachment",
  "presented", "uses/frame". If a dieted depth map shares a framebuffer with colour targets, those
  colour targets drop to 1x for that pass (the existing `can_rescale &=` logic). Watch the census
  and the screenshots.
- Census (`C13 rt census`, every 1800 frames with STATS): every GPU-written image that is rescaled
  or has the diet shape: `WxHxLayers fFormat MiB 2x|1x-diet [HOST] age N @addr` (age = frames since
  the committer's last LRU touch), top 40 by size plus totals.

## V4 `EDEN_NX2_C13_STAGING_TRIM` (+ `_CHECK` = census only)

- Every 60 staging frames: free every free (not deferred, GPU done) buffer unused >= 300 frames in
  all three pools; outside loads (`g_loading` from the V1 tick: needs OOM or OOM_CHECK on, else
  never "loading") cap the device-local pool at `EDEN_NX2_C13_STAGING_CAP_MB` (256), oldest first.
- Census every 600 staging frames (STATS or CHECK): per pool count / bytes / idle >300 / bytes in
  device-local memory types, deferred count, stream buffer size and placement. Printed in the
  `C13 vram census` line together with VMA device-local allocations per memory type (with the
  DeviceBuffers and SparsePages pools' share), texture-cache device bytes, decode classes,
  transcode scratch and `unattributed` (what is left). That line is printed from
  `TickVramStats`, so it needs `EDEN_NX2_VRAM_STATS=1` too (the plan's runs set it). The census
  calls `Device::GetSeparatePool` for device-local types in use, which creates an empty pool for a
  type that had none (no memory).

## Stats line

`C13 vram (10 s, ..)`: applied / old / new margin, floor and its parts, budget, critical (old /
new with CHECK), failures, episodes (load), load drops, decays, refails, floor raises, frames
over critical (old only / new only), reloc rebinds / deletes / no room / no alloc / refused,
check tiles / mismatches, diet 1x images and scale-ups refused, staging trimmed, loading. On with
`EDEN_NX2_C13_STATS=1` or any W1 CHECK.

## Risks

- V1: floor can lift critical up to budget - 256 MiB (by design: the collector cannot get below
  the non-evictable set). If it causes failures they show as `refails` / new episodes.
- V1: failures with usage far under the budget (VMA's own budget, fragmentation) give a 0 deficit:
  margin stays; the cycle 10 emergency collection still runs on each failure.
- V2: rebind behind live descriptors: done between frames under the texture cache lock from
  TickFrame, same invalidation as a rescale; old handles live until the GPU tick passes.
  Fallback `EDEN_NX2_C13_RELOC=0`. Evicting frame-hot non-RT images (delete path) re-uploads them
  next frame.
- V3: visible change; wrong matches (square screen-sized targets) appear in the census.
- V4: trimming the device-local staging pool may re-allocate buffers in bursts (vkAllocateMemory
  counts in the VRAM line).

## What to look at in the test round

1. c13-chk / c13-ss-chk with `EDEN_NX2_C13_OOM=1 EDEN_NX2_C13_RELOC=1` + CHECKs: episode lines
   (old vs new margin), `refails 0` in the race window, `frames over critical` < 30 %,
   `old only` large; `C13 reloc check` mismatches 0; 2P `host memory now 0 images` within ~1 s.
2. Census lines: which 2x RTs exist (are the 3072^2 ones diet candidates?), what `unattributed`
   holds; decide STAGING_TRIM from the census.
3. c13-ss-diet with `EDEN_NX2_C13_RT_DIET=1`: `diet 1x images`, VRAM saved, shadow screenshots.
4. Suggested defaults after the round: OOM and RELOC on under MT lite if checks pass.
