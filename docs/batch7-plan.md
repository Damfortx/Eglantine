# Batch 7: BC7 ASTC recompression, transcode disk cache, GPUPARAM2 robustness, cheaper capture

Branch mkw-mt-lite from c6b7c99. Same method as round 6 (docs/mt-lite-plan6.md): one sub-agent per
workstream, launched exactly once, on disjoint files / functions of the same worktree; sub-agents
do not build; the coordinator builds, integrates, commits each piece, then appends ONE test round
to docs/mt-pending-runtime-checks.md ("Batch 7"). Every piece has its own switch
(Tegra::Nx2EnvMode / Nx2Switch, src/video_core/nx2.h is read-only). No new .cpp files, no
CMakeLists.txt edits (host shaders: only existing .comp files may be edited; they are already
compiled to SPIR-V headers by the existing build rules).

## Ownership

Shared .cpp files are split by FUNCTION. Edit with small local Edit calls, re-read before editing.

| piece | owns |
|---|---|
| T1 BC7 recompression | host_shaders/bc_encoder.comp (BC7 path added to the existing shader, selected by push constant: no CMake change); vk_compute_pass.h/.cpp: BcEncoderPushConstants, BcEncoderPass; vk_texture_cache.cpp: anonymous-namespace ASTC helpers (IsOpaqueBc1Astc, AstcHostFormat, WillUseGpuBcnTranscode, ...), TranscodeBcMode, EncodeTranscoded (except the OnTranscodeEncoded line), UseOpaqueAstcFormat, the constructor's pass/view-format setup, image creation format selection; maxwell_to_vk.cpp (ASTC case); new mt/astc_bc7.h (switches) |
| T2 transcode disk cache | new video_core/texture_cache/astc_disk_cache.h (header-only); vk_texture_cache.cpp: the "Batch 7 T2" block (TryTranscodeFromDiskCache, OnTranscodeEncoded) + its members in the "Batch 7 T2" block of vk_texture_cache.h; one polling line in the function that retires pending_transcodes (TickFrame-like, ~line 2615) and one in the destructor if needed |
| R1 GPUPARAM2 robustness | mt/gpuparam.h, mt/gpuparam2.h; maxwell_3d.cpp/.h GpuParam* functions; dma_pusher.cpp TryGpuParamCall + gpuparam block; shader_environment.*, shader_recompiler/** (LDC.64 replacement), vk_pipeline_cache.*, fixed_pipeline_state.*, gl_shader_cache.cpp (key only), vk_graphics_pipeline.* (per-pipeline flag), vk_rasterizer.cpp indirect draw path, draw_manager.*, buffer_cache indirect path |
| R2 cheaper capture | vk_mt_pipeline.h (CaptureDraw, FinishOverlayCompact, SeqReadDescriptor, ...); gpu_split.cpp/.h back side (BackLoop and what it calls; NOT the round-6 hold lines / ReleaseFrontHold); mt/capture.h, capture_plan.h, packet_compact.h, draw_packet.h, seq_diet.h, tracker_lock.h, xlate_cache.h; common/spin_recursive_mutex*.h; memory_manager.h/.cpp WriteBlockCached; maxwell_3d.cpp: only the functions containing the WriteBlockCached calls (not GpuParam*); new mt/lite7_capture_switches.h |
| coordinator | this doc, the T1/T2 hook stubs (done before launch), build, integration, commits, docs/mt-pending-runtime-checks.md |

## T1 BC7 recompression (EDEN_NX2_ASTC_BC7, default off)
GPU BC7 encoder (mode 6, optionally mode 1/5 tries for alpha) in bc_encoder.comp, push constant
mode 0/1/2 = BC1/BC3/BC7. BC7 = 16 bytes/block like BC3, so every size computation (util.cpp,
surface.cpp) stays valid. Only images that go through the GPU transcode (WillUseGpuBcnTranscode)
become BC7; everything else (CPU path, huge arrays, 3D) keeps BC3 (the CPU encoder has no BC7).
Opaque -> BC1 under BC7: EDEN_NX2_ASTC_BC7_OPAQUE_BC1=1 keeps it (default: opaque also BC7).

## T2 transcode disk cache (EDEN_NX2_ASTC_DISKCACHE, default off)
Key = XXH3-128 of the guest ASTC bytes + format/size/levels/layers/BC mode/encoder version. Value =
the BC buffer exactly as EncodeTranscoded lays it out. Lookup before the decode; hit = upload the
cached bytes through a staging buffer and image.UploadMemory. Miss = after the encode, copy the BC
buffer into a host-visible download buffer; when its tick is done, hand the bytes to a writer
thread (file per entry under the per-title shader cache dir, size cap + LRU by mtime/index).
Stats every 10 s: hits / misses / bytes read / written / evicted.

## R1 GPUPARAM2 robustness (on under MT_LITE via GPUPARAM2; each sub-switch)
(a) pipelines whose non-vertex stages read the draw-parameter words: those calls fall back to the
v1 copy path instead of drawing with 0; (b) LDC.64 (GetCbufU32x2) at bank cb+0x640 replaced by
(BaseVertex, BaseInstance); (c) macro 44 multi-draw (count + N records) via
vkCmdDrawIndexedIndirectCount with the count from GPU memory, when the classifier accepts it.

## R2 cheaper sequencer capture (each behind a switch, on under MT_LITE)
From the iteration-2 profile (~4.4 us/draw): memoization keyed on pipeline/plan + unchanged inputs,
no redundant descriptor reads, shorter lock holds.
