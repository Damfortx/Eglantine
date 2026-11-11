# Cycle 12 W1 hooks (lines for files W1 does not own)

None. Every W1 change is in W1 files (plan section 3): gpu_split.h/.cpp, dma_pusher.cpp/.h,
maxwell_3d.cpp FrontDmaRead, kepler_compute.cpp front QMD mirror (dispatch callback),
mt/c11_front.h, new mt/c12_front.h. The cycle 11 hooks (CaptureCompute
C11PublishComputeWrites, FrontCallMultiMethod C11NoteCbData) are already in the tree and are
what P2 (LISTRANGE gate) relies on.

Integration notes for the coordinator:
- gpu_split.h now includes mt/c12_front.h (after c11_front.h); dma_pusher.h includes it too.
- `GpuSplit::PendingWrite` gained a trailing member `s32 channel{-1}` (set in PushPendingWrite);
  the two brace initialisers keep 9 positional values, so nothing else changes.
