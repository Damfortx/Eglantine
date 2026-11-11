# Cycle 13 W3 (front list writer waits): hook lines for files W3 does not own

Plan section 3 calls this file docs/cycle13-hooks-C.md (agent C = W3). All three hooks are in
D's files (vk_mt_pipeline.h, vk_mt_seq.h). Everything they call is in `video_core/gpu_split.h`
(already included there) and `video_core/mt/c13_front.h` (included by gpu_split.h). Without
the hooks W3 still builds and runs: the census keys fall back to `@program-address`, the kick
still publishes and sets the back's wait target (the sequencer's existing Mark at that target
settles it), only "kick marks", "run ends" and "seq memo skips" stay 0.

## H1 (L1 census key): shader hash per program, sequencer

`src/video_core/renderer_vulkan/vk_mt_pipeline.h`, `Nx2DrawPipeline::CaptureCompute`, right
after the existing `VideoCommon::MT::C11PublishComputeWrites(...)` call:

```cpp
    // Cycle 13 W3 L1 (mt/c13_front.h): the program's shader hash, the front census key.
    VideoCommon::MT::C13PublishProgramHash(memory.GetID(), real.regs.code_loc.Address(),
                                           pipeline->ShaderHash());
```

(`pipeline` is the `ComputePipeline*` parameter; `ComputePipeline::ShaderHash()` exists in
vk_compute_pipeline.h. Same key as C11's table: the front's QMD program address. One relaxed
compare on the per-dispatch fast path; a no-op unless the C13 front census runs.)

## H2 (L2): the committer ends its lock run at the kick's Mark

`vk_mt_pipeline.h`, the committer loop's `if (lockrun) switch (...)` (~line 1586). Take
`case MT::StreamEntry::Mark:` out of the shared Deltas/ComputeDeltas/CbBind/RootTable/Mark
group and give it its own case:

```cpp
            case MT::StreamEntry::Mark:
                // Cycle 13 W3 L2 (mt/c13_front.h): the kick's Mark ends the lock run, so the
                // front's list read after it does not meet the cache mutexes held.
                if (VideoCommon::MT::C13KickRun() &&
                    VideoCommon::GpuSplit::Get().C13KickMark(entry[1])) [[unlikely]] {
                    if (RunHeld()) {
                        EndRun(&VideoCommon::MT::g_c11_commit.end_entry);
                        VideoCommon::MT::g_c13_kick_run_ends.fetch_add(1,
                                                                       std::memory_order_relaxed);
                    }
                } else if (RunHeld() && RunContended()) {
                    EndRun(&VideoCommon::MT::g_c11_commit.end_contended);
                }
                break;
```

(The other four kinds keep the existing contended check. `entry[1]` is the Mark's position, as
in `case MT::StreamEntry::Mark: last_mark = entry[1];` further down. No flush is added: the
writer's command list pages are already copied and submitted right after the dispatch by the
buffer cache.)

## H3 (L2): the sequencer skips C12 memo record work before a pending kick

`src/video_core/renderer_vulkan/vk_mt_seq.h`, `Nx2DrawPipeline::C12AfterPass1`, the final
miss branch (`} else { ++memo.miss_streak; C12Record(...); return; }`), and the same in D's
new way-aware version if it replaces it:

```cpp
    } else {
        if (memo.miss_streak < 0xffff) {
            ++memo.miss_streak;
        }
        // Cycle 13 W3 L2 (mt/c13_front.h): a draw before a pending kick: no record work (the
        // memo keeps its old entry; c12_memo cleared so C12Finish does not touch it).
        if (VideoCommon::GpuSplit::Get().C13SeqBeforeKick()) [[unlikely]] {
            VideoCommon::MT::g_c13_seq_skips.fetch_add(1, std::memory_order_relaxed);
            c12_memo = nullptr;
            return;
        }
        C12Record(memo, pipeline, ssbos);
        return;
    }
```

Only the plain miss branch, never the CAPTURE_REPEAT_CHECK branch. `c12_memo = nullptr` is
required: C12Finish otherwise writes this draw's binding state into a memo whose reads are
another draw's. D: please confirm nothing after C12AfterPass1 other than C12Finish uses
`c12_memo` (it does not in the c12 code). `EDEN_NX2_C13_KICK_SEQ=0` turns it off.
