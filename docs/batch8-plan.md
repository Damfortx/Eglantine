# Batch 8: upstream sync, frame generation, front macro cost, VRAM pressure at 2x

Branch mkw-mt-lite from b10b1fa. Same method as batch 7 (docs/batch7-plan.md): one sub-agent per
workstream, launched exactly once, on disjoint files / functions; sub-agents do not build; the
coordinator builds, integrates, commits each piece, then appends ONE test round to
docs/mt-pending-runtime-checks.md ("Batch 8"). Every piece has its own switch
(Tegra::Nx2EnvMode / Nx2Switch, src/video_core/nx2.h is read-only). No new .cpp files, no
CMakeLists.txt edits.

Context: MT lite +9-15% over MT unset at 2x; the front (GPU) thread is the limit (push ~304 ms/s,
macro ~162 ms/s, front methods ~130 ms/s). At 2x the RTX 3070 Ti (8 GB) runs at 70-80% with
7.3-7.8 GB VRAM in use (user desktop ~1 GB); driver allocations under that pressure caused
multi-second stalls (fixed for ASTC decode images by persistent decode images).

## Ownership

U1 works in its OWN git worktree (separate branch b8-upstream based on b10b1fa) and commits the
cherry-picks there; the coordinator cherry-picks that branch onto mkw-mt-lite after U2-U4 are
committed. U2-U4 edit the main worktree concurrently: small local Edit calls, re-read before
editing, never rewrite a shared file.

| piece | owns |
|---|---|
| U1 upstream sync | its own worktree / branch b8-upstream only (any file except CMakeLists.txt; no new .cpp files) |
| U2 frame generation | renderer_vulkan/present/frame_gen*, present/fsr3_frame_gen*, present/lsfg_*, present/layer.*, vk_fsr3_upscaler.*, vk_present_manager.*, vk_blit_screen.*, vk_swapchain.*, renderer_vulkan.cpp/.h, video_core/frame_gen/*, host_shaders/*fsr3*/*frame_gen* (existing files only); new header renderer_vulkan/present/framegen_switches.h |
| U3 front macro cost | macro.cpp/.h, macro_turing_jit.h, macro HLE code; engines/maxwell_3d.cpp: FrontRunMacro, FrontWrite, FrontWriteRun, CallMacroMethod/ProcessMacro* and the macro write callbacks (NOT GpuParam*, NOT WriteBlockCached functions); maxwell_3d.h: a "Batch 8 U3" block; new mt/lite8_macro.h |
| U4 VRAM pressure | vulkan_common/vulkan_memory_allocator.*, vulkan_common/vulkan_device.* (budget query only), vk_staging_buffer_pool.*, vk_buffer_cache.* (stream/staging sizes), vk_texture_cache.cpp/.h: Image/ImageView/Framebuffer construction + allocation and the scratch/render-target reuse functions (NOT the Batch 7 T1/T2 ASTC blocks), texture_cache/texture_cache.h + texture_cache_base.h (GC/budget functions), vk_scheduler.* (only a per-frame stats tick if needed); new mt/lite8_vram.h |
| coordinator | this doc, build, integration, commits, docs/mt-pending-runtime-checks.md |

If a piece needs a one-line hook in a file it does not own, it reports the exact line instead of
editing.

## U1 upstream sync
Compare origin/nx2 (and the v0.1.0-rc* tags) with mkw-mt-lite BY CONTENT (the branch took
upstream up to ~2026-11-08 midday + #737 water fixes via cherry-picks, so merge-base is useless).
Take correctness/performance fixes relevant to MKW / Switch 2 rendering; skip CI, docs,
branding, UI, Android. Never take CMake hunks; fixes needing new .cpp files are listed as not
taken.

## U2 frame generation (EDEN_NX2_FRAMEGEN, default off)
The tree already contains upstream's FSR 3 frame-generation path (present/fsr3_frame_gen.*,
frame_gen.*, frame_gen_pacer.*). Selevet build 10 only flips the frame_gen default plus the
Lichii build-88 FSR 3 fixes (history/current copied before the flow, motion vectors at flow
resolution with the right sign). Port the platform-neutral fixes and add the env switch.

## U3 front macro cost (EDEN_NX2_MT_LITE8_<X>, on under MT_LITE)
Per-call overheads of FrontRunMacro thunks: thunk dispatch per emitted method, FrontWrite per
emission, parameter copies, macro cache lookups; batched emits; HLE fast paths for the hottest
macros by hash (tools/mmedis.py, docs/gpuparam-plan.md).

## U4 VRAM pressure (EDEN_NX2_VRAM_<X>)
Render-target / scratch reuse, staging/stream buffer sizes, texture cache budget against the
8 GB device with ~1 GB desktop use, no dedicated allocations for transient images; stats line
"VRAM in 10 s" (VK_EXT_memory_budget usage/budget per heap, allocation count/latency, max).

## Coordinator log
- 2026-11-09: U1 (worktree $HOME/src/Eglantine-b8up, branch b8-upstream), U2, U3, U4 launched once each.

## U2 implementation notes
- How it works (ENABLE_LSFG=ON by default -> HAS_LSFG in the Linux build): Composite blits the real
  frame, FrameGen::Process runs the FSR 3 type (in-tree compute shaders fsr3_optical_flow.comp +
  fsr3_frame_interpolation.comp; no FidelityFX SDK/DLL), then per generation takes another
  PresentManager Frame, interpolates into its storage view, Flush + Present; real frame last. FIFO
  forced by vk_swapchain (beats PRESENT_MAILBOX); frame_gen_pacer picks generations per frame.
  MT/front split: no conflict (back thread only, Record/Flush; fine with PRESENT_DEFER/async present).
- Ported from Selevet build 9/10 (Lichii #194): history/current rotated BEFORE the flow (before, both
  copies were the current frame, so interpolation mixed identical frames); flow reads our copies
  (set written once); motion converted with the flow image size and the right sign; GENERAL layouts
  throughout; destination discarded to GENERAL before the write; interpolation sets from a
  tick-recycled DescriptorAllocator (old single set was rewritten while in flight); m_current gets
  TRANSFER_SRC; FSR 3 upscaler IsSupported checks atomic/sampled. Not taken: LSFG DLL/Windows bits,
  (std::min) spellings, DKB trace, Selevet's older present/swapchain. Extra: no frame is marked
  generated before the flow is valid.
- Switches (present/framegen_switches.h, read once, INFO "FrameGen: ..."): EDEN_NX2_FRAMEGEN unset =
  settings (default off), 1 = on + FSR 3, 0 = off ('a' = unset); EDEN_NX2_FRAMEGEN_MULT=2..4;
  EDEN_NX2_FRAMEGEN_RATE=<Hz> adaptive target (0 = always MULT-1).
- Check at runtime: (1) display must refresh >= MULT x game rate (MKW 60 -> 120 Hz), else FIFO
  back-pressure halves the game rate (RATE=display Hz avoids it). (2) Picture: ghosting at HUD/fast
  edges (block-match flow, +-3 px at half res); smearing the wrong way = sign error. (3) Pacing:
  generated + real presents queue back to back; check present stats/frame-time graph. (4) Latency
  +~1/2 game frame. (5) VRAM: history + current (4 B/px, window size) + RG16F half-res motion
  ~33 MB at 1440p / ~75 MB at 4K, independent of the 2x scale, plus up to MULT x present frames.

## U4 implementation notes
Findings: allocation is VMA 3.3 (16 MiB blocks; >8 MiB or driver-preferred = dedicated, i.e. one
vkAllocateMemory per image, NVIDIA prefers it for many render targets). The texture cache already
follows VK_EXT_memory_budget (heapBudget = what the driver grants this process, other programs
taken out; re-read every 30 frames, minus 1/8 margin, block slack and OOM margin) but
ApplyThresholdOverrides caps critical at MaxCriticalMemory = 5 GiB: on the 8 GB card that cap
binds, so driver usage ~= 5 GiB + VMA slack + driver internals (EDEN_NX2_TC_CRITICAL_MIB tunes).
Stream buffer already 128 MiB; MSAA scratch / resolve shadows / temp buffers already pooled.
Churn found: the staging pool sweep frees every free buffer of a level every 64 frames (BC
encoder outputs, uploads re-allocated); ASTC persistent decode classes are never freed (all
classes 64..4096 = ~350 MiB). Rescaled images keep their 1x original (+25% at 2x): not changed.
Implemented (mt/lite8_vram.h): EDEN_NX2_VRAM_TIMING (on): VMA's vkAllocateMemory/vkFreeMemory via
counting wrappers (vulkan_device.cpp, VmaVulkanFunctions); WARNING per allocation >= 50 ms.
EDEN_NX2_VRAM_STATS=1 (off): two "VRAM in 10 s" lines (TickVramStats in allocator.cpp, via
TextureCacheRuntime::TickVram from TextureCache::TickFrame): per device-local heap driver
usage/budget/size, VMA blocks/allocations/slack; allocs, MiB, dedicated, failed, >=20 ms, total/
max/avg ms, frees; nodedicated images; texture cache used/expected/critical, pressure events.
EDEN_NX2_VRAM_STAGING_KEEP (default 1): staging buffers <= 64 MiB used in the last 600 frames
survive the sweep; 1 = host pools only, 2 = also device-local (VRAM), 0 = old.
EDEN_NX2_VRAM_NODEDICATED (off, 'a' ok; _KIB limit 4096): small images suballocated (vkCreateImage
+ vmaAllocateMemory + bind) ignoring the driver's dedicated preference. Risk: lost RT compression.
EDEN_NX2_VRAM_SLOWPRESSURE (off): allocations >= 20 ms count as texture-cache pressure events.
EDEN_NX2_VRAM_DECODE_TRIM (off): decode classes >= 16 MiB freed after 3600 frames unused.
Runtime checks: STATS=1 at 2x: is budget < size - desktop use (else the driver budget ignores
other programs)? dedicated count/latency per 10 s; then NODEDICATED=a and STAGING_KEEP=2 A/B.

## U3 implementation notes
Path (split front): DmaPusher -> FrontCallMethod/MultiMethod (params into fs.parameters, the JIT
reads the vector: no copy) -> FrontRunMacro: position array index (no hash/memo lookup), JIT
Prepare check, Execute -> per emitted word an indirect thunk + emit lambda (dedup, FCALL) ->
FrontWrite -> GpuSplit::StateWrite (already coalesced per method) or MacroWrite (lists, cb data
mergeable: already batched, so no emit batching added). No locks/atomics per call.
Estimated shares of ~111 ns/call (162 ms/s / 1.46M, ~5.9 emits/call): emit handling (thunk,
lambda, FrontWrite, StateWrite) ~50-60%, JIT entry/fixed ~15%; under GPU_PROFILE the zone's two
clock reads per call (~20-30%) and FCALL's same-value skip being off inflate the zone.
Implemented (new mt/lite8_macro.h; maxwell_3d.cpp FrontRunMacro only):
- EDEN_NX2_MT_LITE8_MEMIT: FrontWrite's non-executable state branch inline in the emit lambda
  (FBATCH's conditions/effects; executable, 0x157/0x158, instance loop, Replay, SPLIT_VERIFY and
  REGDEDUP_CHECK still take FrontWrite).
- EDEN_NX2_MT_LITE8_MHLE: macros matched by exact code words (any start; re-matched when position
  or code generation changes): 41 DrawElementsInstanced, 45 clamp-byte, 14/27 cb select, 1 cb
  bind. C++ makes the same emits (order) and data RAM writes through the same lambda; calls it
  can't take whole (few params, data RAM index out of range, >500k instances) run the JIT.
  Offline randomized selftest HLE vs TuringMacroJit: ~580k calls (emit failures too), 0 diffs.
Switches: both 0/1/a, default on under EDEN_NX2_MT_LITE=1, else off ('1' works without MT).
EDEN_NX2_MT_LITE8_MACRO_CHECK=1: HLE into a recorder + JIT for real, compared, 16 logged. Stats
"Front macros in 10 s" (calls, fast emits, HLE per kind, top 10 macros start/hash/emits):
EDEN_NX2_MT_LITE8_MACRO_STATS=1, GPU_PROFILE or CHECK.
Risks: hot macros may not be these 5 (top-10 line says what to add next); MEMIT mirrors
FrontWrite's branch (keep in sync). Checks: CHECK run (0 mismatches), STATS run (HLE share),
GPU_PROFILE macro ms/s and fps A/B with =a.

## U1 implementation notes (coordinator summary)
36 upstream picks (b8-upstream, cherry-picked onto mkw-mt-lite after the U2-U4 commits). The
branch had taken upstream only through the squashed 77e46bc/4957e00 picks: 165 of 187 later
commits had no patch/subject match. Taken: MKW docked photo upscale stand-in (EDEN_NX2_PHOTO_UPSCALE=0
off), intel arc MKW fixes (task payload zero, tracker ClearChannel, removed_conditional_barrier warp
size), shader-build wait before cache save on close, 100 us idle spin for NX2 titles
(EDEN_NX2_IDLE_SPIN_US), UMWAIT retry, occlusion counter pause at submit, skip upload of fully
cleared new render targets, RangeMap last-interval cache, alpha-ref rounding in the pipeline key,
16 pipeline-use sessions, refused tessellation pipelines skipped, fence_manager SubmitDeferred
before own-fence wait (only when GpuSplit::OwnsScheduler()), macro query flush before MME DMA reads
(split off only), shader fixes (global atomics, SHF.R.U64, ISBEWR, subgroup broadcast, PTP gathers,
nested return/demote, CF scans, mesh point/line outputs, IMul 0/1 fold), Ampere lowering series
(extended forms on; EDEN_NX2_LOWER_EXTENDED=0 off), dynarmic VCLZ16, LayeredFS guard, TC threshold
overrides capped at the device budget. Not taken: fe2cd8be2b (CPU writes vs downloads: needs a
port into the batched InvalidateGPUCache -> separate task), 66b9ff826f / 90ff431884 / 405e5f7ac8
(conflict with the MT split design), uniform re-upload chain, OoT/Star Fox/font/AMD-only items,
debug server (new .cpp), diagnostics, CI/docs/UI. No CACHE_VERSION bump (picks change shader
translation: delete the per-title Vulkan pipeline cache once if pictures look stale).

## Coordinator log (cont.)
- U2, U3, U4 reported; build rc=0; commits 76c756c (U2), 6943362 (U3), b16a61f (U4).
- b8-upstream cherry-picked cleanly (36 commits) on top; final build verified before the docs commit.
