# Cycle 12 W3 hooks (sequencer capture repeat)

Lines W3 needs in functions it does not own, for the coordinator to apply. None is needed for
the build or for the default path; (1) and (2) make restarts and CAPTURE_REF exact.

## 1. `Nx2DrawPipeline::Start` (vk_mt_pipeline.h, unowned), next to the UBO_GEN store

Replace

    if (VideoCommon::MT::UboGen()) {
        VideoCommon::MT::g_ubo_gen_on.store(true, std::memory_order_seq_cst);
    }

by

    if (VideoCommon::MT::UboGen() || VideoCommon::MT::C12CaptureGensMode() != '0') {
        VideoCommon::MT::g_ubo_gen_on.store(true, std::memory_order_seq_cst);
    }
    // EDEN_NX2_C12_CAPTURE_REPEAT: forget the memos (and the generations armed while the
    // tracking was off) with the replicas reset below.
    c12_memos.reset();
    ++c12_gen_epoch;
    c12_gen_on_captures = 0;

Without it: C12Begin turns the tracking on itself at the first capture and bumps the epoch when
it finds it off, so only a Stop/Start with UBO_GEN=1 (the flag goes off and on without the
sequencer seeing it) could keep a generation armed across a gap; memos themselves are validated
byte by byte and stay exact.

## 2. `Nx2DrawPipeline::ParsePacket` (W2), compact layout, only for EDEN_NX2_C12_CAPTURE_REF

After

    decoder.ApplyInserts(wire, view.blob, inserts);

add

    decoder.C12ApplyRef(packed.c12_ref_op, packed.c12_ref_slot);

and then set `inline constexpr bool kC12CaptureRefWired = true;` in mt/c12_capture.h. Until both
are done the sequencer never sends a reference (C12CaptureRef() is false), so the packets
always carry their full overlay. REF is never sent with resolvers (Batching()), so SkipPacket
needs nothing.

## 3. Already applied by W2 (nothing to do)

ParsePacket copies `c12_repeat` / `c12_flags` from the compact header with
`VideoCommon::MT::C12CopyRepeatFlag(header, packed);` (mt/c12_commit.h). The old layout carries
DrawHeader as is.
