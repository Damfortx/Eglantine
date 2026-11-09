# Round 3 (merged mkw-mt 8501d90) — results and profile

Attract demo, 2x, NOPROF=1 EDEN_NX2_VSYNC_HZ=240, idle PC.
- MT unset: 66.5 / 60.8 fps. MT=1: 45.5 / 38.4 fps (47.6 with STATS). VERIFY 0 mismatches, SHADOW 0.
- MT_STATS: 47k draw packets/s, 4.4k compute packets/s, packets 3831 B avg (regressed from ~1.1-1.5 KB),
  delta words 922k/s, marks 10.5k/s, batches 4.7 draws, resolvers busy 3% / wait 94%,
  slow path 9.2k/s all "sparse", sequencer draws 480 ms/s (~10 us/draw), committer idle 256 ms/s.
- top: GPU 85%, GPUBack 80%, GPUCommit 67%.
- perf flat (12 s each, cpu-clock:u; data in tools/runs/perf/t3-*.data):
  GPUBack: BackLoop self 20%, SpinRecursiveMutex::lock 10.9%, CaptureDraw 4.2+2.4%, CallMethod 3.3%,
    FinishOverlayCompact 2.7%, Execute 2.6%, TrackerScope ctor 2.2%, GpuToCpuAddress 2.0%, SerialRaw 1.7%.
  GPU (front): sched_yield 32.7%, WaitConsumed 17.8%, Drain 13.6%, WaitForWriters 2.6% (~67% waiting).
  GPUCommit: BatchCursor::Wait in WaitResolved 31.4%, TrackerScope ctor 11.8%, Idle 9.5%,
    BindHostComputeStorageBuffers lambda 6.8%.
  VulkanWorker: light.
Diagnosis: waits and contention dominate, not work: (1) committer waits for resolver batches while resolvers
idle (batch latency); (2) tracker_mutex contention sequencer<->committer; (3) front blocked in drains on
the MT pipeline; (4) sequencer/front ping-pong; plus packet size regression and 922k delta words/s.
