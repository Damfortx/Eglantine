# Cycle 10 plan: config B (MT lite + BC7 + ASTC disk cache) in the 1-player race

Data: docs/cycle10-data.md. Target config B = `EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1
EDEN_NX2_ASTC_BC7=1 EDEN_NX2_ASTC_DISKCACHE=1`. Stable 22.1 fps; B is bimodal: 49-52 fps (fast
runs) or 26-32 fps (slow runs, GPU-bound at 28.6 ms GPU per frame with low power). Per the user,
the BC3/no-disk-cache 4-5 fps case and the evict/re-decode loop are out of scope (disk cache stays
on). Only big items; small tweaks below noise are skipped. Every piece has a switch (default on
when it should help; `=0` restores today's behaviour). No CMakeLists.txt edits, no new .cpp.

## Bottlenecks (ranked)
1. **Silent system-memory placement (slow mode, -40 %).** The process sits at the driver budget
   (usage 6.25-6.46 of 6.4-6.5 GiB). MemoryAllocator creates images and device-local buffers with
   VMA AUTO_PREFER_DEVICE and only a *preferred* DEVICE_LOCAL flag: when vkAllocateMemory fails on
   the device heap (hundreds of "failed" per 10 s in the stats) VMA quietly uses a host memory
   type; no pressure event, no warning, and the resource stays in system memory for good. GPU
   time per frame rises from < 20 ms to ~29 ms (GPU waits on PCIe: high utilisation, ~120 W
   instead of ~150 W). Taken to the extreme this is the 3-6 fps collapse (dc1/old1/s2).
2. **Front thread waits on the back (fast mode limiter).** At 50 fps the front (GPU thread)
   spends ~38 % in WaitConsumed / sched_yield / TimedWaitSettled; back 75 %, committer 79 % busy:
   a ping-pong. Sources (per s): writers (commands) 800-1180 drains (~250 real waits, ~95 ms),
   puller acquires 90 (all waited, 36-58 ms; ~63/s are "site 1 op 41 pending met": a release for
   the word is queued but its value is not predicted, so the front drains everything), position
   ~30 (all waited, 18-37 ms).
3. Back/committer per-draw cost and lock contention (SpinRecursiveMutex ~5 % each): becomes the
   limit once 2 is gone; no structural change ready this cycle (noted for cycle 11).

## Workstreams (one sub-agent each, disjoint files, launched once)
### A. VRAM placement guard (EDEN_NX2_VRAM_GUARD, default on; 'a' not needed)
Owns: src/video_core/vulkan_common/vulkan_memory_allocator.cpp/.h, new header
src/video_core/mt/c10_vram.h, src/video_core/texture_cache/texture_cache.h,
texture_cache_base.h, src/video_core/renderer_vulkan/vk_texture_cache.cpp/.h,
vk_buffer_cache.cpp/.h (only if needed for buffer placement).
- Images and DeviceLocal/Stream-less device buffers: first allocation attempt with
  requiredFlags = DEVICE_LOCAL (no silent host fallback). On failure: count a pressure event,
  ask the texture cache for an emergency collection (non-blocking flag consumed at the next safe
  point, plus an immediate synchronous trim when the caller is the texture cache itself), retry
  device-local once, and only then fall back to host memory (counted, logged rate-limited).
- Count resources living in non-DEVICE_LOCAL memory (images and buffers: count and MiB, current
  and created per 10 s) and print them in the EDEN_NX2_VRAM_STATS line and in one always-on
  WARNING per minute while > 0 (that is the slow-mode detector).
- Texture cache: images placed in host memory are flagged; when the device has room again
  (usage < critical) the collector evicts a bounded number of them per frame (not in use this
  frame, not render targets currently bound) so they are re-created in VRAM (from the disk cache
  for ASTC). Emergency collection: when the allocator reported a failure, the next GC pass runs
  aggressive with costly protection off until usage < budget - margin.
- Correctness: never free memory in use by the GPU (use the existing sentenced/deferred
  deletion), keep the unbacked sparse fallback for huge images.

### B. Front waits (EDEN_NX2_C10_FRONT_*, default on under MT lite)
Owns: src/video_core/gpu_split.cpp/.h, src/video_core/dma_pusher.cpp/.h,
src/video_core/engines/puller.cpp/.h, src/video_core/mt/lite5_list_switches.h, new header
src/video_core/mt/c10_front.h.
- B1 acquires (site 1, pending release of unpredicted value, ~63/s): wait only as far as needed
  (the sequencer position of the last pending release for that word, or resolve its value on the
  front when it is a known kind: fence/report payload, add) instead of a full DrainBack; keep the
  existing suspension path when still unmet.
- B2 writers (commands): command-list reads on GPU-written pages wait for every writer sent.
  Make the check byte/range-precise (wait only if a pending writer's range overlaps the bytes
  the list reads; trust pages whose writers are known not to touch list bytes), extend the
  existing L24/L25 machinery; LISTCHECK-style verification switch for the test round.
- B3 position waits (~30/s): same treatment if the cause is shared with B1/B2.
- Diagnostics: per-reason counts in the existing "GPU split drains per second" line.

### Coordinator
Build, integration, commits, launcher (EDEN_NX2_PRESENT_MAILBOX=1 for the user's 60 Hz FIFO
display so 50 fps is not quantised to 30), docs/cycle10-results.md, one final test round.

## Final test round
Race, stable vs B+cycle10 (3 each, alternating), with nvidia-smi; one VRAM_STATS run (host
resources 0?), chk1/chk2-style checks (+ LISTCHECK / new checks), screenshot; bisect with
EDEN_NX2_VRAM_GUARD=0 / EDEN_NX2_C10_FRONT_*=0 only if something regresses.
