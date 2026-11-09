B (mkw-mt-lite ef10090), 2x attract demo, idle PC:
- MT unset 63.4 / 62.2 fps; MT=1 MT_LITE=1: 52.3 / 55.5 fps (0.82-0.89 of baseline; best MT so far). VERIFY: 0 mismatches except one 14-byte "upload changed" (concurrent guest write), watchdog 0.
- Approach C (front assist, no threads) measured neutral: FA_UBO +0.36+-4.06 fps (proves 2.1M binds unchanged, 0 wrong), FA_XLATE -2.41+-5.49. Discarded.
- Profile of B (perf flat 12 s each, tools/runs/perf/Bp-*.data):
  GPUBack (sequencer): BackLoop self 13.65%, CaptureDraw 7.0+1.75%, CallMethod 5.3%, SpinRecursiveMutex::lock 5.0%, FinishOverlayCompact 4.6%, Execute 4.3%, TrackerScope 2.6%, GpuToCpuAddress 2.0%, SerialRaw 1.8%, WriteBlockCached 1.7%, EmitDraw 1.7%.
  GPU (front): WaitConsumed 18.7%, sched_yield 16.0%, FrontCallMethod 9.8%, WaitForWriters 7.0%, DmaPusher CallMethod 6.8%, ProcessCommands 5.8%, FrontWrite 4.7%, macro JIT 4.3%, MacroWrite 4.1%, DrainToSettleTarget 2.0%.
  GPUCommit: Idle 16.5%, TrackerScope 8.7%, DoUpdateGraphicsBuffers 3.8%, BindHostComputeStorageBuffers lambda 3.2%, ConfigureImpl 2.9%, PrepareImage 2.6%, BindHostGraphicsUniformBuffer 2.5%, PushImageDescriptors 2.45%, GpuToCpuAddress 2.1%, PacketDecoder::Decode 1.5%.

# Iteration 2 (ae4966c) measured
- 2x attract demo: B2 lite 59.8 / 64.6 / 50.8 / 55.9 fps vs MT unset 60.6 / 58.9 / 53.0 (+ one anomalous 42.4): parity (~57.8 vs ~57.5). VERIFY 0 mismatches, watchdog 0.
- MT_STATS: up to 97.6k draw packets/s, 7.1k compute/s, 1086 B avg packets, marks 69.7k/s (WMARK), sequencer draws 432 ms/s (~4.4 us/draw), committer idle 310 ms/s; commands/s: query 12.5k, fence 9.4k, barrier 7.2k, invalidate 6.9k.
- top: GPU (front) 81%, GPUCommit 79%, GPUBack 71%.
- perf flat: GPUBack BackLoop 12.5%, CaptureDraw 7.1%, SpinRecursiveMutex::lock 6.1%, CallMethod 4.9%, Execute 4.6%, FinishOverlayCompact 3.0%, SerialRaw 2.5%, SeqReadDescriptor 2.1%, WriteBlockCached 2.1%. GPU front: WaitConsumed 19.7%, sched_yield 16.1%, FrontCallMethod 10.1%, DmaPusher CallMethod 7.6%, ProcessCommands 6.5%, FrontWrite 5.4%, TimedWaitSettled 5.1%, MacroWrite 4.4%, macro JIT 4.1%. GPUCommit: Idle 17%, TrackerScope 6.7%, DoUpdateGraphicsBuffers 4.2%.
- EDEN_NX2_GPU_PROFILE front per second: push 368 ms, front method 219 ms, macro 153 ms, split drain 188 ms, idle 54 ms => FRONT IS NOW THE LIMIT (~93% busy). 3D register writes 9.7M/s of which 5.4M same value. Drains/s: puller ~2.0-2.4k (463-475 waited, 68-87 ms, committer share 5-6 ms), writers(commands) 12-14k (450-464 waited, 43 ms, committer 16-18 ms), writers(dma read) ~900 (228-238 waited, 45-46 ms, committer 0), position ~250 (63-69 waited, 11-13 ms), writers(cpu lists) 70-97k (2 waited).

# Iteration 3 (88b4fd8) measured
- fps 2x: MT lite 62.6 / 63.4 / 72.3 (mean 66.1) vs MT unset 54.5 / 67.4 / 55.8 (mean 59.2): +12%. VERIFY 0, watchdog 0, DMA_READ_CHECK 0 stale. SPLIT_VERIFY differences ~70% also on the stable build (artifact).
- EDEN_NX2_GPU_PROFILE front per second: push 304 ms, macro 162, front method 130 (was 219), split drain 324 (was 188), idle 58. 10.2M 3D register writes/s (5.5M same value).
- Drains/s: puller 2015 (366 waited, 98 ms, committer 11 ms); writers(commands) 18.8k (371 waited, 83.5 ms, committer 25 ms); position 202 (67 waited, 83.5 ms => ~1.2 ms each, committer 13.8 ms); writers(dma read) 202 (70 waited, 46 ms); writers(cpu lists) 6/s; predicted acquires 3470/s.
- Puller waits in 10 s: acquire drain 1641, acquire fences 1039, resume poll 16404 (1004 behind).
- Acquire drains in 10 s: site1 op41 pending unmet 315, site1 op41 pending met 85, site2 op40 none unmet 693, site2 op41 none unmet 30, site2 op43 none met 518; top addresses 0x2105037010 (496), 0x21f6297000 (355), 0x21f6297100 (338), 0x21f6297040 (236), 0x21f6297140 (194).
=> The front now mostly waits on the SEQUENCER (committer share small).
