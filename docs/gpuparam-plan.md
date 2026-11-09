# GPU-written macro parameters: indirect draws instead of front waits (branch mkw-mt-lite)

Iteration 5 left "writers (commands)" as the front's biggest wait in Mario Kart World: ~420
GPU-written command list fetches a second, all written by compute (culling), ~108 ms/s of
waiting (median 400 us, p90 1.6 ms). EDEN_NX2_LIST_DUMP (13.6k records, attract demo) says what
they are. This change stops reading them.

## What the lists are

- 97.7% are 5-word entries, 2% are a 1-word entry followed by a 380-word one, 0.3% are 55-70-word
  lists of state + macro calls (macros 1, 14, 27: const buffer updates; not handled here).
- **Every one starts inside a method the previous, CPU-written entry opened**: `carry=0O:e51+5`
  (13294) or `carry=0O:e59+381` (143, then the 380-word entry continues it, 126). 0xE51 is the
  parameter register of macro **40** (0xE00 + 2*40 + 1), 0xE59 that of macro **44**. The CPU entry
  ends with an IncreaseOnce header for the macro's start register (0xE50 / 0xE58) and its first
  word; the GPU entry supplies the rest of the call's parameters, and the macro runs when the last
  one arrives (Maxwell3D::ProcessMacro / FrontCallMethod).
- So the "GPU-written command lists" are NVN's indirect draws: the GPFIFO entry points straight at
  the indirect buffer the culling compute wrote. Value statistics over the 13294 macro-40 entries:
  word 0 119 distinct values (an index count), word 1 0 in 76% and 1..8 otherwise (an instance
  count; 0 = culled), word 2 always 0 (first index), word 3 0 or varying (vertex offset), word 4
  small (first instance). That is VkDrawIndexedIndirectCommand exactly. The macro-44 calls: a
  draw count (0x4c = 76) and 76 such records (380 = 76 * 5); the records' first index
  accumulates the previous counts (0, 0x12, 0x24, 0x36, 0x39, ...): a multi-draw.
- The "mme[41].start 45.5% / mme[45].start 40.8%" histogram is a decoder artifact: the dump marked
  the carried method 'O' (dma_increment_once) although its first word had already been consumed
  (non_incrementing set), so the decoder incremented 0xE51 -> 0xE52 (macro 41 start) and 0xE59 ->
  0xE5A (macro 45 start) and dumped those macros' code. Fixed: the carry is now 'N' once the
  method stopped incrementing (dma_pusher.cpp RecordListDump). Macros 40 and 44 never appear in
  the dump's M lines (their start registers are in the CPU entries), so **their code is not
  known yet**: the implementation logs it (below).

## Decoded macros (tools/mmedis.py, TU104 MME semantics of src/video_core/macro_turing.h)

Register names: 0x5f7 index_buffer.first, 0x5f8 index_buffer.count, 0x446 vertex_id_base,
0x50d global_base_vertex_index, 0x50e global_base_instance_index, 0x585 draw.end, 0x586 draw.begin.

Macro 41 (hash d11d56facfb0c568), parameters (begin, count, instances, first, base vertex, base
instance) - **DrawElementsInstanced** (the shape macro 40 has to have):
```
1f5: r2=p0 (begin) | r1=p1 (count)
1f6: r0=p2 (instances) | index_buffer.first = p3
1f7: vertex_id_base = p4 | global_base_vertex_index = p4
1f8: if r0 == 0 goto 1fd | global_base_instance_index = p5
1f9: draw.begin = r2 | index_buffer.count = r1          <- loop: one draw per instance
1fa: draw.end = 0
1fb: r2.instance_id (bits 26-27) = 1 (Subsequent) | r0 -= 1
1fc: if r0 != 0 goto 1f9
1fd: END | vertex_id_base = 0 | global_base_vertex_index = 0
1fe: global_base_instance_index = 0
```
The draw manager turns the First + Subsequent draws into one vkCmdDrawIndexed(count, instances,
first, base vertex, base instance) - the same as vkCmdDrawIndexedIndirect on the 5 words.

Macro 45 (hash f807be34d132c5e5), 1 parameter: reads data RAM 0x4b8/0x4b9, merges bits 0-7 of
the parameter (if bit 8 set) into the high byte of a state word, clamps against 0x4b9, stores the
parameter at data RAM 0x4ba and writes method 0xd8 (byte 0x360). State only, no draw.
Macros 14 / 27 (1 parameter): data RAM lookup -> const_buffer data (0x8e0+); macro 1: a const
buffer update guarded by an index range. None of them takes GPU-written parameters in the dump.

## Design

Front (DmaPusher::Step, before choosing how to read an entry): an entry on GPU-written pages,
at a word offset of 0, inside a 3D macro call's parameters (method >= 0xE00, non-incrementing,
method_count >= entry size), whose call ends in it or in the next entries of the same submission
(all GPU-written, parameters only, no sync flag) goes to `Maxwell3D::GpuParamCall` with the
entries' (address, words). If it takes the call the entries are not read at all (no writer wait,
no copy/flush); `dma_state.method_count` = 0 and the extra entries are skipped.

`GpuParamCall` (maxwell_3d.cpp; generic parts in mt/gpuparam.h):
1. Layout: exactly one CPU parameter p0 (the draw.begin word) and 5 GPU words in one entry
   (IndexedSingle), or 1 + 5N in one entry or a 1-word entry + a 5N-word entry (IndexedMulti:
   count word, N records).
2. Per-call conditions on the front's registers: p0 instance id First, topology not
   quads/quad strip/polygon (as HLE_DrawIndexedIndirect), shadow RAM not in Replay, mesh control
   off (RecordDrawIndirect skips task/mesh draws), index format not u8.
3. **Classifier** (no fixed hash: macros 40/44's code is unknown): the macro's native code runs in
   a scratch TuringMacroInterpreter on 7 test vectors of GPU words (first: all zero), with the
   front's registers for MME state reads and a copy of the data RAM. Every run must finish, emit
   no macro/MME-DMA-read method and no executable method besides the draw registers, produce
   exactly the reference model's draws (each draw.end recorded with draw.begin, index count/first,
   base vertex/instance, indexed flag; reference = per record `instances` draws, first with p0,
   the rest with instance id Subsequent - what the indirect draw draws), write the other
   registers and the data RAM identically in every run and none of them after a draw. The verdict
   is cached per (start, code hash, kind, N, p0) unless a run read registers or data RAM (then
   every call is verified, against the current state). Each macro's first verdict is logged with
   its code: `GPU params: macro <i> start=<s> hash=<h> single|multi N=<n> begin=<p0>:
   deferrable|rejected (<why>) code=...` - this is how we learn macros 40/44.
4. The call runs **with zero GPU words** (FrontRunMacro on the split front, CallMacroMethod
   without the split): it draws nothing (checked: the zero vector must draw nothing), leaves every
   non-draw register and the data RAM exactly as the real call would (checked: independent of the
   words) and the draw registers at their zero-call values on front and back alike (shadow and
   back stay consistent).
5. Then the indirect draw: `split.Defer` (in order after the call's writes) ->
   `Maxwell3D::GpuParamDraw` on the back: ConsumeSink, DrawDeferred (pending instanced draw),
   DrawManager::indirect_state = {indexed, include_count = multi, count at the count word,
   records at the GPU address, max N, stride 20 (0 for single as HLE)}, DrawIndexedIndirect with
   the topology of p0 and the index-buffer estimate (HLE_DrawIndexedIndirect's path), then
   draw_indexed cleared. On the sequencer this is SeqDrawIndirect -> a draw packet with the
   indirect parameters (EDEN_NX2_MT_INDIRECT); the committer's buffer cache binds the buffer that
   holds the compute output (BindHostDrawIndirectBuffers, SeqTakeUploads at capture) and records
   vkCmdDrawIndexedIndirect / vkCmdDrawIndexedIndirectCount.

Ordering: the compute dispatch that writes the words is earlier in the same stream, so earlier on
the sequencer, the committer and the Vulkan queue; the dispatch ends the render pass and the next
render pass begins after the COMPUTE_SHADER -> ALL_GRAPHICS (MEMORY_READ, covers
DRAW_INDIRECT) barrier the scheduler adds after compute writes (NoteComputeWrites, EGLANTINE_SYNC_FIX,
default on). DMA/I2M writers go through the buffer cache's upload/copy barriers as for any
indirect draw. CPU-written words on once-GPU-written pages are guest memory the buffer cache
uploads (CPU-modified tracking) before the draw.

Register equivalence: the indirect draw reads none of the draw registers (vertex offset, first
instance, first index, count from the words; index buffer address/format, topology from state the
macro doesn't touch or from p0). After the call, index_buffer.first/count, draw.begin and the base
registers hold the zero-call values instead of the GPU values on both front and back (consistent,
so same-value skips stay right); hardware would hold the GPU values. Only a later draw that
relies on them without writing them, or a macro reading them with MME state reads, could differ
(not seen for NVN draw macros: they write all of them).

Without the MT back (EDEN_NX2_GPUPARAM=1, default off) the same: the single GPU thread runs the
zero call through CallMacroMethod and the indirect draw inline; with the split but no MT, the
split front path and a back Defer.

## Switches

| switch | default | effect |
|---|---|---|
| EDEN_NX2_MT_LITE_GPUPARAM=0/1/a | on under EDEN_NX2_MT_LITE=1 (needs EDEN_NX2_MT) | the above on the split front |
| EDEN_NX2_GPUPARAM=0/1/a | off | the same without the MT back |
| EDEN_NX2_GPUPARAM_CHECK=1 | off | every deferred call also waits and reads the words as before (ReadGpuWrittenCommands / flush), runs the macro on them and compares its draws with the reference (= the indirect draw's); first 16 mismatches logged `GPU params check:` |
| EDEN_NX2_GPUPARAM_STATS=1 | off (also GPU_PROFILE, CHECK) | line every 10 s: `GPU params in 10 s: N deferred (single, multi; entries not read), kept waiting: <reason counts>, rejects: <why>, verified, check compared/mismatched/unreadable/draws` |

## Coverage (estimate)

If macro 40 is the DrawElementsInstanced shape its parameters say it is: 97.7% of the GPU-written
fetches (all the 5-word ones) stop waiting; macro 44 if it is the per-record multi-draw: +2%
(two fetches per call). The 0.3% state lists keep the old path. With ~420 fetches/s and ~108 ms/s
of writer waits, up to ~100 ms/s of front time back; the zero call and the classifier's cached
lookup cost ~1-2 us per call. If the classifier rejects macro 40 the log line gives its code and
reason, and nothing changes (the old wait).

## Checks to run (centralized testing)

1. Learn the macros: one MT lite run (`EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1 EDEN_NX2_GPUPARAM_STATS=1`):
   the `GPU params: macro 40 ...` / `macro 44 ...` lines (deferrable or why rejected, code) and the
   10 s line (deferred/s should be ~420 minus the 0.3%; "kept waiting" reasons).
2. Correctness: `EDEN_NX2_GPUPARAM_CHECK=1`: "mismatched" 0, unreadable 0; picture check (missing
   or exploded geometry = wrong indirect parameters), VERIFY run, watchdog, no device loss.
   `EDEN_NX2_SPLIT_VERIFY=1` shows front/back register differences (expected none: both hold the
   zero-call values).
3. Profile: GPU_PROFILE + PULLER_STATS: "writers (commands)" ~108 ms/s -> ~0-10; list stats
   fetches/s ~420 -> ~10.
4. fps A/B: `EDEN_NX2_MT_LITE_GPUPARAM=0` vs default (or `=a` in one run), >= 3 each.
5. Without MT: `EDEN_NX2_GPUPARAM=1` (MT unset), same checks.

## Risks

- Register divergence (above): draw registers keep zero-call values, not GPU values.
- The classifier tests 7 vectors with instance counts <= 3: a macro that behaves differently for
  other values (e.g. masks the instance count, splits large draws) would pass and be wrong for
  those values; the check mode compares every real call.
- Index-buffer range: like HLE, the indirect path binds the index buffer for its estimated size
  (EstimateIndexBufferSize) instead of first+count.
- Task/mesh pipelines are refused by the front's mesh-control register; a pipeline the front
  thinks is not mesh but is would lose the draw (RecordDrawIndirect skips mesh).
- Pages already marked command-list pages keep being copied to host after their writers
  (Nx2MarkCommandListPage) although nobody reads them now: a cost left for later.
- Conditional rendering, transform feedback, topology override: as any indirect draw
  (ProcessDrawIndirect).
