# Cycle 13 W1 hooks (for the coordinator)

W1 needs **no hook** in another agent's file or function to build and run: every edit is in
W1-owned files/functions (see docs/cycle13-notes-W1.md).

Integration notes:

- `texture_cache/texture_cache.h` `RelocateHostImages` was restructured (shared `evict` lambda,
  C13 path first); the cycle 10/11 path after it is the same logic. `C11VramTick`, `TickFrame`,
  `ImageCanRescale` and `ScaleUp` have local additions only. Merge conflicts with D (repeat
  functions) should not touch these.
- `texture_cache_base.h` gained `#include "video_core/mt/c13_vram.h"` and seven `c13_*` members
  after `budget_auto_caps`.
- `vk_texture_cache.h` gained `#include "video_core/mt/c13_vram.h"`; `MakeImage` in
  `vk_texture_cache.cpp` has a new trailing default parameter `c13_device_only`.
- The test round's runs need `EDEN_NX2_C13_OOM=1 EDEN_NX2_C13_RELOC=1` set explicitly (W1
  switches are off by default as instructed), plus `EDEN_NX2_C13_RT_DIET=1` for run 9. The
  `C13 vram census` line also needs `EDEN_NX2_VRAM_STATS=1`.

Not done (would need an `ImageFlagBits` value in image_base.h, which nobody owns this cycle): the
V3 census' "presented to the swapchain" and "uses per frame" columns. The census prints the
frames since the last LRU touch instead.
