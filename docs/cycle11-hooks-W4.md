# Cycle 11 W4 hooks in other agents' code

None needed. W4 uses W3's CPU-modified epoch getter as it exists in texture_cache_base.h:
`texture_cache.CpuModifiedEpoch()` (the brief said `GetCpuModifiedEpoch()`; if W3 renames it,
change the two calls in vk_graphics_pipeline.cpp ConfigureImpl).

EmitDraw (unowned) got two one-line edits for the packet field:
`header.repeat_delta = static_cast<u16>(c11_repeat_delta);` (compact) and
`header.repeat_delta = static_cast<u32>(c11_repeat_delta);` (old layout).
