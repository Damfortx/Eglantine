B (mkw-mt-lite ef10090), 2x attract demo, idle PC:
- MT unset 63.4 / 62.2 fps; MT=1 MT_LITE=1: 52.3 / 55.5 fps (0.82-0.89 of baseline; best MT so far). VERIFY: 0 mismatches except one 14-byte "upload changed" (concurrent guest write), watchdog 0.
- Approach C (front assist, no threads) measured neutral: FA_UBO +0.36+-4.06 fps (proves 2.1M binds unchanged, 0 wrong), FA_XLATE -2.41+-5.49. Discarded.
- Profile of B (perf flat 12 s each, tools/runs/perf/Bp-*.data):
  GPUBack (sequencer): BackLoop self 13.65%, CaptureDraw 7.0+1.75%, CallMethod 5.3%, SpinRecursiveMutex::lock 5.0%, FinishOverlayCompact 4.6%, Execute 4.3%, TrackerScope 2.6%, GpuToCpuAddress 2.0%, SerialRaw 1.8%, WriteBlockCached 1.7%, EmitDraw 1.7%.
  GPU (front): WaitConsumed 18.7%, sched_yield 16.0%, FrontCallMethod 9.8%, WaitForWriters 7.0%, DmaPusher CallMethod 6.8%, ProcessCommands 5.8%, FrontWrite 4.7%, macro JIT 4.3%, MacroWrite 4.1%, DrainToSettleTarget 2.0%.
  GPUCommit: Idle 16.5%, TrackerScope 8.7%, DoUpdateGraphicsBuffers 3.8%, BindHostComputeStorageBuffers lambda 3.2%, ConfigureImpl 2.9%, PrepareImage 2.6%, BindHostGraphicsUniformBuffer 2.5%, PushImageDescriptors 2.45%, GpuToCpuAddress 2.1%, PacketDecoder::Decode 1.5%.
