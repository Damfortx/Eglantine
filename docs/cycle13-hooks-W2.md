# Cycle 13 W2 hooks (bank re-upload under MT / VERIFY): lines for D's file

All in `src/video_core/renderer_vulkan/vk_mt_pipeline.h` (owner D / coordinator). The callee
functions live in W2's files (`buffer_cache_mt.h`, declared in `buffer_cache_base.h`); with
`EDEN_NX2_C13_BANK=0` / `EDEN_NX2_C13_CLAIM` unset they return at once.

## H1 (B1, required): sequencer uniform capture marks the bank before taking its uploads

`Nx2DrawPipeline::CaptureDraw`, the graphics uniform loop (`for (size_t stage = 0; stage <
infos.size(); ++stage)` ... `const auto& sizes = pipeline->UniformSizes();`), the last branch of
the staged/cached decision. Replace

```cpp
            } else {
                SeqTakeUploads(device_addr, size);
            }
            ubos.push_back(entry);
```

with

```cpp
            } else {
                // C13 W2 (EDEN_NX2_C13_BANK=1): the native bank re-upload, once per frame, as a
                // guest write taken into this packet by the SeqTakeUploads below.
                rasterizer.buffer_cache.MtC13CaptureBank(device_addr, size, sizes[stage][index],
                                                         rasterizer.gpu_memory != nullptr &&
                                                             rasterizer.gpu_memory->UsesNativeTiling());
                SeqTakeUploads(device_addr, size);
            }
            ubos.push_back(entry);
```

Notes: only the graphics loop (not the compute one with `.stage = 5`: compute binds never had the
re-upload). The call must stay before `SeqTakeUploads` and inside the tracker hold (it takes the
recursive tracker lock itself). `SeqTakeUploads` dedups by `taken_ranges` per capture: the mark
must precede the first take of that range in the capture, which it does since it is in the same
branch. If D's M1/M2 memo work adds a path that skips this loop for a whole draw, that draw does
not re-upload the bank (it is still marked by the next draw that runs the loop, at most one
frame later).

## H2 (B2, contingent, off unless EDEN_NX2_C13_CLAIM=1): claim CPU writes in SeqInvalidateBuffers

`Nx2DrawPipeline::SeqInvalidateBuffers`:

(a) TrapLock branch, mutex ranges: replace

```cpp
            std::scoped_lock lock{r.buffer_cache.mutex};
            for (const auto& [addr, size] : mutex_ranges) {
                r.buffer_cache.WriteMemory(addr, size);
            }
```

with

```cpp
            std::scoped_lock lock{r.buffer_cache.mutex};
            r.buffer_cache.MtC13ClaimCpuWrites(std::span<const std::pair<DAddr, std::size_t>>(
                mutex_ranges.data(), mutex_ranges.size())); // C13 W2 B2
            for (const auto& [addr, size] : mutex_ranges) {
                r.buffer_cache.WriteMemory(addr, size);
            }
```

(b) non-TrapLock branch: replace

```cpp
        std::scoped_lock lock{r.buffer_cache.mutex};
        for (const auto& [addr, size] : regions) {
            if (r.buffer_cache.MtPageHoldsGpuData(addr, size)) {
```

with

```cpp
        std::scoped_lock lock{r.buffer_cache.mutex};
        if (VideoCommon::MT::C13Bank::ClaimOn()) {
            // C13 W2 B2: claim the ranges written here now; the owner ranges are claimed with
            // all the regions in the Serial below.
            boost::container::small_vector<std::pair<DAddr, std::size_t>, 8> claim_now;
            for (const auto& [addr, size] : regions) {
                if (!r.buffer_cache.MtPageHoldsGpuData(addr, size)) {
                    claim_now.emplace_back(addr, size);
                }
            }
            r.buffer_cache.MtC13ClaimCpuWrites(
                std::span<const std::pair<DAddr, std::size_t>>(claim_now.data(), claim_now.size()));
        }
        for (const auto& [addr, size] : regions) {
            if (r.buffer_cache.MtPageHoldsGpuData(addr, size)) {
```

(c) owner ranges Serial (the upstream fe2cd8be2b case: one range's page download must not write
GPU data over another range's newer CPU write; all regions of the batch are claimed first, on the
committer, under the mutex): replace

```cpp
    if (!owner_ranges.empty()) {
        counters.inval_owner_serials.fetch_add(1, std::memory_order_relaxed);
        Serial(MT::Kind::InvalidateRegion, [&] {
            std::scoped_lock lock{r.buffer_cache.mutex};
            for (const auto& [addr, size] : owner_ranges) {
```

with

```cpp
    if (!owner_ranges.empty()) {
        counters.inval_owner_serials.fetch_add(1, std::memory_order_relaxed);
        Serial(MT::Kind::InvalidateRegion, [&] {
            std::scoped_lock lock{r.buffer_cache.mutex};
            r.buffer_cache.MtC13ClaimCpuWrites(regions); // C13 W2 B2 (Serial is synchronous)
            for (const auto& [addr, size] : owner_ranges) {
```

`MtC13ClaimCpuWrites` takes `std::span<const std::pair<DAddr, std::size_t>>`; the small_vectors
are passed as explicit `(data(), size())` spans (boost iterators are not guaranteed
`contiguous_iterator`s for std::span's range constructor); `regions` is already that span.
