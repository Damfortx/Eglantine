# Cycle 13 hooks from W4 (2P memos): M3, the uniform bind memo with ways

W4 owns `buffer_cache.h` **C12UpdateGraphicsBuffers only**; the uniform bind memo lives in
`BindGraphicsUniformBuffer` and `UpdateUniformBuffers` (buffer_cache.h, owned by nobody this
cycle) and its member in `buffer_cache_base.h` (W2/B's file). These three hunks make it 2-way
(EDEN_NX2_C13_MEMO_WAYS, same switch as M1/M2). The helpers they call (`MT::C13MemoWaysNow`,
`MT::C13PromoteWay`, `MT::C13DemoteFirst`, `MT::C13FindWay`, `MT::C13Add`,
`MT::g_c13_memo`, `MT::kC13MaxWays`) are in `src/video_core/mt/c13_memo.h`, already included
through `mt/c12_commit.h` (which `buffer_cache_base.h` includes). With `EDEN_NX2_C13_MEMO_WAYS=1`
(or `=0`, or the 1-way half of `a`) every hunk takes exactly the cycle 12 path.

Without these hunks everything else of W4 still builds and works (M1, M2, the texture ways, the
commit profile); only the "ubind" counters of the C13 memo line stay 0.

## H1: buffer_cache_base.h, next to `c12_ubind_memo`

After

```cpp
    std::array<std::array<C12UniformBindMemo, NUM_GRAPHICS_UNIFORM_BUFFERS>, NUM_STAGES>
        c12_ubind_memo{};
```

add

```cpp
    /// EDEN_NX2_C13_MEMO_WAYS (cycle 13 W4 M3, mt/c13_memo.h): the uniform bind memo's other
    /// ways per slot (c12_ubind_memo is way 0, the most recent), most recent first.
    std::array<std::array<std::array<C12UniformBindMemo, MT::kC13MaxWays - 1>,
                          NUM_GRAPHICS_UNIFORM_BUFFERS>,
               NUM_STAGES>
        c13_ubind_ways{};
```

(~5 x 18 x 3 x 88 B = 24 KiB per buffer cache.)

## H2: buffer_cache.h, BindGraphicsUniformBuffer

Replace the translation lookup

```cpp
        const C12UniformBindMemo& memo = c12_ubind_memo[stage][index];
        if ((c12_generation & 1) != 0 && memo.generation == c12_generation &&
            memo.gpu_memory == gpu_memory && memo.channel == channel_state &&
            memo.gpu_addr == gpu_addr) {
            MT::C12Add(MT::g_c12_commit.ubind_hits);
```

with

```cpp
        C12UniformBindMemo& memo = c12_ubind_memo[stage][index];
        const auto translation_matches = [&](const C12UniformBindMemo& candidate) {
            return (c12_generation & 1) != 0 && candidate.generation == c12_generation &&
                   candidate.gpu_memory == gpu_memory && candidate.channel == channel_state &&
                   candidate.gpu_addr == gpu_addr;
        };
        // EDEN_NX2_C13_MEMO_WAYS (cycle 13 W4 M3): another way holding this address (the other
        // view's camera buffer) becomes way 0, its resolution with it.
        bool c12_hit = translation_matches(memo);
        if (!c12_hit) {
            const u32 c13_ways = MT::C13MemoWaysNow();
            if (c13_ways > 1 && MT::C13PromoteWay(memo, c13_ubind_ways[stage][index], c13_ways,
                                                  translation_matches)) {
                c12_hit = true;
                MT::C13Add(MT::g_c13_memo.ubind_promote);
            }
        }
        if (c12_hit) {
            MT::C12Add(MT::g_c12_commit.ubind_hits);
```

(the rest of the hit block, CHECK included, is unchanged: it reads `memo`, now way 0.)

And at the end of the function replace

```cpp
    if (c12_memo) {
        C12UniformBindMemo& memo = c12_ubind_memo[stage][index];
        memo.channel = channel_state;
```

with

```cpp
    if (c12_memo) {
        C12UniformBindMemo& memo = c12_ubind_memo[stage][index];
        const u32 c13_ways = MT::C13MemoWaysNow();
        if (c13_ways > 1 && memo.generation != 0 && memo.gpu_addr != gpu_addr) {
            // EDEN_NX2_C13_MEMO_WAYS: the previous address stays as a way (with its resolution);
            // way 0 starts this one without a resolution.
            MT::C13DemoteFirst(memo, c13_ubind_ways[stage][index], c13_ways);
            memo.resolved_buffer_id = BufferId{};
            MT::C13Add(MT::g_c13_memo.ubind_demote);
        }
        memo.channel = channel_state;
```

## H3: buffer_cache.h, UpdateUniformBuffers (graphics, the EDEN_NX2_C12_UBIND_MEMO block)

Replace

```cpp
        C12UniformBindMemo& memo = c12_ubind_memo[stage][index];
        if (c12_memo && memo.resolved_buffer_id && memo.resolved_channel == channel_state &&
            memo.resolved_deletion_epoch == buffer_deletion_epoch &&
            memo.resolved_c12_epoch == c12_buffer_epoch &&
            memo.resolved_device_addr == binding.device_addr &&
            memo.resolved_size == binding.size) {
```

with

```cpp
        C12UniformBindMemo& memo = c12_ubind_memo[stage][index];
        const auto resolution_matches = [&](const C12UniformBindMemo& candidate) {
            return static_cast<bool>(candidate.resolved_buffer_id) &&
                   candidate.resolved_channel == channel_state &&
                   candidate.resolved_deletion_epoch == buffer_deletion_epoch &&
                   candidate.resolved_c12_epoch == c12_buffer_epoch &&
                   candidate.resolved_device_addr == binding.device_addr &&
                   candidate.resolved_size == binding.size;
        };
        if (c12_memo && !resolution_matches(memo)) {
            // EDEN_NX2_C13_MEMO_WAYS (cycle 13 W4 M3): a resolution another way kept (the
            // translation memo missed, e.g. a layout change, but the buffer is the same).
            const u32 c13_ways = MT::C13MemoWaysNow();
            if (c13_ways > 1) {
                const auto& c13_kept = c13_ubind_ways[stage][index];
                const size_t found = MT::C13FindWay(c13_kept, c13_ways, resolution_matches);
                if (found < c13_kept.size()) {
                    const C12UniformBindMemo& kept = c13_kept[found];
                    memo.resolved_channel = kept.resolved_channel;
                    memo.resolved_deletion_epoch = kept.resolved_deletion_epoch;
                    memo.resolved_c12_epoch = kept.resolved_c12_epoch;
                    memo.resolved_device_addr = kept.resolved_device_addr;
                    memo.resolved_size = kept.resolved_size;
                    memo.resolved_buffer_id = kept.resolved_buffer_id;
                    MT::C13Add(MT::g_c13_memo.ubind_resolve_other);
                }
            }
        }
        if (c12_memo && resolution_matches(memo)) {
```

(the hit body, CHECK included, and the fill after FindBuffer are unchanged.)

Checks: EDEN_NX2_C13_MEMO_CHECK=1 turns on EDEN_NX2_C12_COMMIT_REPEAT_CHECK, whose ubind checks
("uniform bind device address", "uniform buffer id") run on every hit, promoted ways included.
Counters: "ubind other-way translations / demoted / other-way resolutions" in "C13 memo (10 s)".

## Nothing else

M1 (vk_mt_seq.h / vk_mt_pipeline.h), M2 (buffer_cache.h C12UpdateGraphicsBuffers), the texture
memo ways (vk_graphics_pipeline.cpp/.h) and the W4c commit profile (vk_mt_pipeline.h CommitDraw,
vk_graphics_pipeline.cpp ConfigureImpl) are in W4's own files/functions. W4 applies no B/C hook.
