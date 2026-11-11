# Batch 9: upstream worker-thread pipelines, VRAM budget for 8 GB cards, 1-player race benchmark

Branch mkw-mt-lite from 91b569e. Same method as batches 7/8: one sub-agent per workstream, launched
exactly once, on disjoint files; sub-agents do not build; the coordinator builds, integrates,
commits each piece and appends ONE test round ("Batch 9") to docs/mt-pending-runtime-checks.md.
Every piece has its own switch (Tegra::Nx2EnvMode / Nx2Switch, src/video_core/nx2.h read-only).
No new .cpp files, no CMakeLists.txt edits. Nothing is run (user is playing): compile only.

User data (1 player, GUI, 2x, 60 Hz FIFO): stable ~24 fps; MT lite + BC7 + ASTC disk cache ~30 fps
after warm-up; first ~3 min choppy (runtime pipelines built on the GPU thread, texture loading),
0.1-2.7 s stalls; VRAM 6.9-7.4 of 8 GB; one run with 4.77 GiB available VRAM: ~4 fps, 0.1-3 s
fence waits.

## Ownership

| piece | owns |
|---|---|
| U1 upstream | own worktree $HOME/src/Eglantine-b9up, branch b9-upstream (from 91b569e); any file except CMakeLists.txt, no new .cpp; commits there; coordinator cherry-picks onto mkw-mt-lite after U2 |
| U2 VRAM budget | texture_cache/texture_cache.h + texture_cache_base.h (budget/GC functions), vulkan_common/vulkan_device.* (budget query), vk_texture_cache.cpp/.h (BC1 opaque selection in the ASTC BC7 path, budget getters), mt/astc_bc7.h, mt/lite8_vram.h, new header mt/lite9_vram.h |
| U3 race benchmark | $HOME/src/eglantine-buildenv/tools/race/ (new dir) only |
| coordinator | this doc, build, integration, commits, docs/mt-pending-runtime-checks.md |

## U1 upstream development branches
origin/fix/oot-map-change (+ origin/t3/port-orchid-log-fix): worker-thread pipeline translation
(0f1ae997c2, 40633dc5ce), async shaders default (2d61242e29), stage cache (d1195b7d49, needs new
stage_cache.cpp -> header-only port or report), shader recompiler races/perf, write-protect skip
(f27522bfb9), per-thread page translation cache (b1e650849e), HOST_IMPORT default (56e70161d7),
Orchid geometry passthrough (4233c05d8b), diagnostics. Must stay compatible with MT (pipeline
creation is Serial on the sequencer). Every behaviour change switchable.

## U2 VRAM budget (EDEN_NX2_TC_BUDGET_AUTO, EDEN_NX2_ASTC_BC7_OPAQUE_BC1, default on)
Critical/expected caps from VK_EXT_memory_budget (budget - margin) instead of the fixed 5 GiB
critical cap; proactive eviction before the budget; BC1 for opaque ASTC under BC7; one startup
warning line when available VRAM < ~5.5 GiB at 2x.

## U3 1-player race benchmark (tools/race/)
EDEN_NX2_KEY_SCRIPT timeline from title screen to a single-player race, calibration mode with
portal screenshots every N s, race-window fps summary, README. Not run.

## Coordinator log
- 2026-11-09: plan written; U1 (af2a90a2384e9eac0), U2 (acdd9bb278344c38b), U3 (a78c306585b0934e5) launched once each.
- 2026-11-09: all three reported done. U2 + coordinator churn stats committed (85a0be2381, build
  rc=0); b9-upstream (16 commits) cherry-picked without conflicts, build rc=0; worktree and
  branch b9-upstream removed. tools/race/ reviewed (bash -n / py_compile ok).

## Implementation notes
- U1 taken (switch, default): worker-thread pipeline translation EDEN_NX2_ASYNC_TRANSLATION (on
  with async shaders; MT: environments built+detached on the committer via RunOnCommitter, the
  sequencer never waits: EDEN_NX2_ASYNC_TRANSLATION_MT_WAIT_MS 0, GPU thread without MT 2 ms,
  4 ms a frame; adopted pipelines stored to disk via PostToCommitter), translators =
  driver compile workers below normal priority (EDEN_NX2_TRANSLATION_THREADS), async shaders
  settings default true (user's configs already true), stage cache header-only
  (src/video_core/stage_cache.h, EDEN_NX2_STAGE_CACHE on/0/verify/a), per-thread GPU page
  translation cache (EDEN_NX2_TRANSLATION_CACHE on), hot-page write-protect skip
  (EDEN_NX2_HOT_PAGES, OFF here, upstream on), geometry passthrough fix
  (EDEN_NX2_GS_PASSTHROUGH_FIX on), 6 shader recompiler race/perf fixes (no switch), swapchain
  VK_INCOMPLETE retry (c40fca7da4, no switch).
- U1 not taken: HOST_IMPORT default flip (56e70161d7), BIND_STATS / back-thread diagnostics
  (7da9d855a7, 321aaa0662, 145c283b0a, 62564b004d), shader bench test (1cbc9a404c).
- U2: see commit 85a0be2381 and mt/lite9_vram.h. Opacity for BC1 is decided before image
  creation from the guest ASTC blocks; the disk cache key carries the BC mode, so BC7 entries
  from batch 7/8 stay valid and BC1 entries are new.
- Coordinator: EDEN_NX2_TC_CHURN=1 (diagnostic) for the suspected evict/re-decode loop over the
  critical cap.
