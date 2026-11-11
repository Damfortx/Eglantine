# Cycle 11 W1 hooks (lines for files W1 does not own)

Both hooks only feed F2 (EDEN_NX2_C11_LISTRANGE, off by default). Without them F2 stays correct
but gains nothing: every dispatch counts as "unknown program" (hook 1 missing) and every 3D
constant buffer multi-method run as "unranged" (hook 2 missing).

## Hook 1 (W2/W4 file): src/video_core/renderer_vulkan/vk_mt_pipeline.h, Nx2DrawPipeline::CaptureCompute

Right after `const bool mapping_sized = real.IsDlssDispatch();` (gpu_split.h, already included,
brings in mt/c11_front.h):

```cpp
    // EDEN_NX2_C11_LISTRANGE (W1 F2, mt/c11_front.h): the program's written SSBO descriptors,
    // for the front's command list writer ranges (a no-op with the switch off).
    VideoCommon::MT::C11PublishComputeWrites(memory.GetID(), real.regs.code_loc.Address(), info,
                                             mapping_sized, native_key == CollisionMapProgram);
```

(`native_key` is CaptureCompute's new parameter in the W2 tree; if the integrated signature has
no `native_key`, pass `false` as the last argument. The key must be the QMD's code address,
qmd[48] | (qmd[49] & 0x1ffff) << 32, which DispatchNativeCompute copies to regs.code_loc.)

## Hook 2 (unowned function): src/video_core/engines/maxwell_3d.cpp, Maxwell3D::FrontCallMultiMethod

Right after the line `split.MultiMethod(this, method, base_start, amount, methods_pending, nullptr);`
at the end of the function:

```cpp
    // EDEN_NX2_C11_LISTRANGE (W1 F2): a constant buffer data run writes the 3D constant buffer
    // when the back runs it (C11NoteCbData; `true` tells DmaPusher this path is hooked).
    if (method - MAXWELL3D_REG_INDEX(const_buffer.buffer) < 16 &&
        VideoCommon::MT::C11ListRange()) [[unlikely]] {
        split.C11NoteCbData(
            memory_manager.GetID(),
            (GPUVAddr{fs.regs[MAXWELL3D_REG_INDEX(const_buffer.address_high)]} << 32) |
                fs.regs[MAXWELL3D_REG_INDEX(const_buffer.address_low)],
            std::max<u64>({4, fs.regs[MAXWELL3D_REG_INDEX(const_buffer.size)],
                           fs.regs[MAXWELL3D_REG_INDEX(const_buffer.offset)]}),
            true);
    }
```
