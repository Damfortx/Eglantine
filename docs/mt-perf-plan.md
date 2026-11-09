# MT back throughput plan (branch mkw-mt-perf)

Data: 120 s attract demo at 2x. MT unset 47.9-51.1 fps (~65k draws/s). EDEN_NX2_MT=1: 35 fps,
~36-45k draws/s. Profiles (perf, dwarf, runs/perf/p1b-GPUBack.txt; d1-GPUCommit.txt,
d1-GPUResolve0.txt; stats lines of runs/log-p1.txt, log-d1.txt, log-rv.txt, log-rsh.txt).

## Findings

### Sequencer (GPUBack), share of its samples
| what | % | notes |
|---|---|---|
| `syscall` (shown as libc `setreuid+0x1d`; no user stack) | 18.3 | futex wakes: `SpmcStream::Commit` does a seq_cst store + `waiters` load per entry and `notify_all` whenever a consumer sleeps. Resolvers wait 92 % and the committer idles ~45 %, so they are often asleep: ~6 entries per draw, most of them wake someone |
| BackLoop own (spinning for the front) | 11.8 | idle, not cost |
| SeqDraw | 42.6 | CaptureDraw 26.6, FlushCaching 9.9, EmitDraw 3.9, CurrentGraphicsPipeline 1.9 |
| - CaptureDraw: MtTakeUploads | 7.8 | CPU-modified bytes of bound ranges (uploads ~2.7 KB/packet) + re-protect (UpdatePagesCachedBatch 3.6). Inherent (the old path uploads the same) |
| - CaptureDraw: IsContinuousRange | 5.9 | storage buffer bindings checked page by page on every draw |
| - CaptureDraw: FinishOverlayCompact | 4.5 | sort + descriptor replica compare, ~10 TIC/TSC refs per draw |
| FlushCaching / SeqInnerInvalidation | 12.2 | MtTrapWrite 4.7, texture mutex 3.4 (taken for every flush; contended with the committer), MtPageHoldsGpuData 2.5; tracker lock taken twice per range (TrackerScope self 3.5) |
| ProcessCBBind -> BindGraphicsUniformBuffer | 8.5 | one "uniform" Command per bind (~165-190k/s, 4.5 per draw); each runs EmitDeltas (5.7) and a stream entry, and splits the draw's deltas into several entries |
| EmitDeltas (all callers) | 7.1 | called per Command too |
| KeplerCompute dispatch | 8.0 | SeqDispatch 6.9 |
| SpinRecursiveMutex::lock self | 4.5 | texture mutex in SeqInnerInvalidation (3.3), tracker |

Per draw: ~8 us (RBUF=0 run, 45k draws/s) to ~13 us (p1, 36k/s) of sequencer time, i.e. as much
as the whole single-threaded draw at 1x. Stream: delta words 1.24 M/s (~34/draw), marks 2.3k/s,
26k batches/s for 36k draws (1.4 draws per batch).

### Batches
1.4 draws per batch: the adaptive close (committer waiting for the open batch -> close after
the next draw) fires on nearly every draw because the committer is starved, and every Command
but the uniform binds (fence 4.2k, barrier 2.6k, query 1.6k, clear 0.8k, cond-render 0.8k /s)
closes the batch too. Each batch costs a BatchEnd entry, a resolver wake-up (futex on the
sequencer), StampBatch and CollectEpochs (both cache mutexes) on the committer.

### Committer (GPUCommit)
Idle 45 % (stream empty: waits on the sequencer), WaitResolved 26 % (the batch of the next draw
is still open or its resolver just woke), real work ~27 %: ConfigureDrawPipeline 13 % = the
slow path of the ~45 % draws that come back "view-not-clean" (16k/s in every run, RREFRESH on or
off), CommitCompute 4.6 %, SignalFence 2 %. It does not limit throughput today, but it will once
the sequencer is ~2x faster, and the view-not-clean slow path is most of its work.

### Resolvers
Busy 2-3 %, wait 92 % (sched_yield + futex). Cheap; their cost is wake-up latency and the futex
wakes they cause on the sequencer.

### VulkanWorker
No usable profile (empty capture); the other agent's numbers show it far from saturated.

## Plan (each piece behind its own switch, src/video_core/mt/perf_switches.h)
| id | switch (default on under MT) | change | expected |
|---|---|---|---|
| P1 | EDEN_NX2_MT_LAZY_WAKE | SpmcStream: Commit = release store of `published`; `Flush()` does the Dekker check + wake; flush points: CloseBatch, SeqMark, Serial, WaitCommitted, stream full (WaitForSpace), Stop, every 32 KiB of entries. Consumers spinning see entries at once; sleepers wake at the next flush | most of the 18 % syscall time: ~-1.5 us/draw on the sequencer |
| P2 | EDEN_NX2_MT_UBIND_FOLD | no uniform Command: the committer applies each CbBind pseudo-delta to its buffer cache (Bind/DisableGraphicsUniformBuffer) when it applies the entry (RunEntry, not the resolvers' mirrors); kept as a Command when the engine does not record binds (mt_record off) | -8 % sequencer, -170k entries/s, deltas in one entry per draw |
| P3 | EDEN_NX2_MT_CMD_IN_BATCH | query/fence/barrier/clear/cond-render Commands stay inside the open batch (the committer runs them in order anyway; marks and waits still close it) | ~10k fewer batch closes/s |
| P4 | EDEN_NX2_MT_BATCH_MIN=4 | adaptive close only once the batch has 4 draws | batches ~1.4 -> ~4-6 draws: 3-4x fewer BatchEnd/wakes/CollectEpochs |
| P5 | EDEN_NX2_MT_CONT_MEMO | IsContinuousRange memo for storage buffers keyed by (gpu_addr, size), valid while LayoutGeneration is the same even value | -5 % sequencer |
| P6 | EDEN_NX2_MT_INVAL_TEXFILTER | SeqInnerInvalidation takes the texture mutex only for ranges Nx2MayHoldTexture says may hold images | -3 % sequencer, less committer/sequencer contention |
| P7 | EDEN_NX2_MT_INVAL_ONELOCK | one tracker lock scope over SeqInvalidateBuffers' loop | -2 % |
| P8 | EDEN_NX2_MT_EBR_LAZY | CollectEpochs skips both cache mutexes when nothing is retired (atomic pending count) | committer -1-2 %, fewer lock collisions with the sequencer |
| P9 | EDEN_NX2_MT_RALIAS | resolver accepts views whose image only HasAliases (not CPU-modified, no other unclean flag); committer runs SynchronizeAliases for them under the texture mutex before Validate (a rescale bumps the version -> slow path); breakdown of view-not-clean by flag in "MT resolvers/s" | committer slow path 45 % -> (the alias share) |
Not planned: value-dedup of deltas (#3712 made redundant writes set dirty flags, the mirrors must
see them); a smaller stream ring (16 MiB matches the 4096-slot resolved ring at 3.7 KB/packet);
the uploads themselves (same bytes the old path uploads).

## Phases and file ownership (sub-agents never share a file; only the integrator builds)
Phase 0 (integrator): this plan, perf_switches.h, declarations the agents fill in.
Phase 1, in parallel:
- Agent S: src/video_core/mt/spmc_stream.h, new src/video_core/mt/selftest_lazy_wake.h,
  src/video_core/mt/selftest_standalone.sh (P1 stream side + TSan/ASan test).
- Agent T: src/video_core/texture_cache/texture_cache_mt.h, texture_cache_base.h,
  src/video_core/renderer_vulkan/vk_mt_resolve.h, vk_mt_resolve_types.h (P9).
- Integrator: vk_mt_pipeline.h, vk_rasterizer.cpp, mt/epoch.h (P1 flush points, P2-P8, the P9 call
  site).
Phase 2: integrate, build, standalone sanitizer tests, commit per piece, append the test round to
docs/mt-pending-runtime-checks.md.
