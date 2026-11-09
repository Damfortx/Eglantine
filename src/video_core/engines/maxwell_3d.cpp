// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/mt/gpuparam.h"
#include <fmt/ranges.h>
#include "video_core/nx2.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_profile.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/textures/texture.h"
#ifdef ARCHITECTURE_x86_64
#include "video_core/macro_turing_jit.h"
#endif

namespace Tegra::Engines {

/// First register id that is actually a Macro call.
constexpr u32 MacroRegistersStart = 0xE00;

Maxwell3D::Maxwell3D(MemoryManager& memory_manager_)
    : draw_manager()
    , memory_manager{memory_manager_}
#ifdef ARCHITECTURE_x86_64
    , macro_engine(bool(Settings::values.disable_macro_jit))
#else
    , macro_engine(true)
#endif
    , upload_state{memory_manager, regs.upload}
{
    dirty.flags.flip();
    InitializeRegisterDefaults();
    execution_mask.reset();
    for (size_t i = 0; i < execution_mask.size(); i++)
        execution_mask[i] = IsMethodExecutable(u32(i));
}

Maxwell3D::~Maxwell3D() = default;

void Maxwell3D::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
    upload_state.BindRasterizer(rasterizer_);
}

void Maxwell3D::InitializeRegisterDefaults() {
    // Initializes registers to their default values - what games expect them to be at boot. This is
    // for certain registers that may not be explicitly set by games.

    // Reset all registers to zero
    std::memset(&regs, 0, sizeof(regs));

    // Depth range near/far is not always set, but is expected to be the default 0.0f, 1.0f. This is
    // needed for ARMS.
    for (auto& viewport : regs.viewports) {
        viewport.depth_range_near = 0.0f;
        viewport.depth_range_far = 1.0f;
    }
    for (auto& viewport : regs.viewport_transform) {
        viewport.swizzle.x.Assign(Regs::ViewportSwizzle::PositiveX);
        viewport.swizzle.y.Assign(Regs::ViewportSwizzle::PositiveY);
        viewport.swizzle.z.Assign(Regs::ViewportSwizzle::PositiveZ);
        viewport.swizzle.w.Assign(Regs::ViewportSwizzle::PositiveW);
    }

    // Doom and Bomberman seems to use the uninitialized registers and just enable blend
    // so initialize blend registers with sane values
    regs.blend.color_op = Regs::Blend::Equation::Add_D3D;
    regs.blend.color_source = Regs::Blend::Factor::One_D3D;
    regs.blend.color_dest = Regs::Blend::Factor::Zero_D3D;
    regs.blend.alpha_op = Regs::Blend::Equation::Add_D3D;
    regs.blend.alpha_source = Regs::Blend::Factor::One_D3D;
    regs.blend.alpha_dest = Regs::Blend::Factor::Zero_D3D;
    for (auto& blend : regs.blend_per_target) {
        blend.color_op = Regs::Blend::Equation::Add_D3D;
        blend.color_source = Regs::Blend::Factor::One_D3D;
        blend.color_dest = Regs::Blend::Factor::Zero_D3D;
        blend.alpha_op = Regs::Blend::Equation::Add_D3D;
        blend.alpha_source = Regs::Blend::Factor::One_D3D;
        blend.alpha_dest = Regs::Blend::Factor::Zero_D3D;
    }
    regs.stencil_front_op.fail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_front_op.zfail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_front_op.zpass = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_front_op.func = Regs::ComparisonOp::Always_GL;
    regs.stencil_front_func_mask = 0xFFFFFFFF;
    regs.stencil_front_mask = 0xFFFFFFFF;
    regs.stencil_two_side_enable = 1;
    regs.stencil_back_op.fail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_back_op.zfail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_back_op.zpass = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_back_op.func = Regs::ComparisonOp::Always_GL;
    regs.stencil_back_func_mask = 0xFFFFFFFF;
    regs.stencil_back_mask = 0xFFFFFFFF;

    regs.depth_test_func = Regs::ComparisonOp::Always_GL;
    regs.gl_front_face = Regs::FrontFace::CounterClockWise;
    regs.gl_cull_face = Regs::CullFace::Back;

    // TODO(Rodrigo): Most games do not set a point size. I think this is a case of a
    // register carrying a default value. Assume it's OpenGL's default (1).
    regs.point_size = 1.0f;

    // TODO(bunnei): Some games do not initialize the color masks (e.g. Sonic Mania). Assuming a
    // default of enabled fixes rendering here.
    for (auto& color_mask : regs.color_mask) {
        color_mask.R.Assign(1);
        color_mask.G.Assign(1);
        color_mask.B.Assign(1);
        color_mask.A.Assign(1);
    }

    for (auto& format : regs.vertex_attrib_format) {
        format.constant.Assign(1);
    }

    // NVN games expect these values to be enabled at boot
    regs.rasterize_enable = 1;
    regs.color_target_mrt_enable = 1;
    regs.framebuffer_srgb = 1;
    regs.line_width_aliased = 1.0f;
    regs.line_width_smooth = 1.0f;
    regs.gl_front_face = Maxwell3D::Regs::FrontFace::ClockWise;
    regs.polygon_mode_back = Maxwell3D::Regs::PolygonMode::Fill;
    regs.polygon_mode_front = Maxwell3D::Regs::PolygonMode::Fill;

    shadow_state = regs;
}

bool Maxwell3D::IsMethodExecutable(u32 method) {
    if (method >= MacroRegistersStart) {
        return true;
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(draw.end):
    case MAXWELL3D_REG_INDEX(draw.begin):
    case MAXWELL3D_REG_INDEX(vertex_buffer.first):
    case MAXWELL3D_REG_INDEX(vertex_buffer.count):
    case MAXWELL3D_REG_INDEX(index_buffer.first):
    case MAXWELL3D_REG_INDEX(index_buffer.count):
    case MAXWELL3D_REG_INDEX(draw_inline_index):
    case MAXWELL3D_REG_INDEX(index_buffer32_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer16_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer8_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer32_first):
    case MAXWELL3D_REG_INDEX(index_buffer16_first):
    case MAXWELL3D_REG_INDEX(index_buffer8_first):
    case MAXWELL3D_REG_INDEX(inline_index_2x16.even):
    case MAXWELL3D_REG_INDEX(inline_index_4x8.index0):
    case MAXWELL3D_REG_INDEX(vertex_array_instance_first):
    case MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent):
    case MAXWELL3D_REG_INDEX(draw_texture.src_y0):
    case RootTables::SelectMethod:
    case RootTables::LoadMethod:
    case MAXWELL3D_REG_INDEX(wait_for_idle):
    case MAXWELL3D_REG_INDEX(shadow_ram_control):
    case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
    case MAXWELL3D_REG_INDEX(load_mme.instruction):
    case MAXWELL3D_REG_INDEX(load_mme.start_address):
    case MAXWELL3D_REG_INDEX(falcon[4]):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
    case MAXWELL3D_REG_INDEX(topology_override):
    case MAXWELL3D_REG_INDEX(clear_surface):
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
    case MAXWELL3D_REG_INDEX(render_enable.mode):
    case MAXWELL3D_REG_INDEX(clear_report_value):
    case MAXWELL3D_REG_INDEX(sync_info):
    case MAXWELL3D_REG_INDEX(launch_dma):
    case MAXWELL3D_REG_INDEX(inline_data):
    case MAXWELL3D_REG_INDEX(fragment_barrier):
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        return true;
    default:
        return false;
    }
}

void Maxwell3D::ProcessMacro(Core::System& system, u32 method, const u32* base_start, u32 amount, bool is_last_call) {
    if (executing_macro == 0) {
        // A macro call must begin by writing the macro method's register, not its argument.
        ASSERT((method % 2) == 0 && "Can't start macro execution by writing to the ARGS register");
        executing_macro = method;
    }

    macro_segments.emplace_back(current_dma_segment, amount);
    current_macro_dirty |= current_dirty;
    current_dirty = false;

    // Nearly every call arrives whole in one method run (title 1 makes ~1,000,000 a second):
    // copy it to the stack instead of growing the parameter vector. Still a copy: the command
    // list is often guest memory read in place, which the guest may reuse while the macro runs
    // (and the macro memo stores the parameters after it ran). Only accurate GPU mode rewrites
    // the parameters in place (RefreshParameters), so it keeps the vector.
    // EDEN_NX2_MACRO_COPY=1: always use the vector (for comparisons).
    static const bool always_copy = std::getenv("EDEN_NX2_MACRO_COPY") != nullptr;
    if (is_last_call && macro_params.empty() && amount <= 64 && !always_copy &&
        !Settings::IsGPULevelHigh()) {
        std::array<u32, 64> parameters;
        std::copy_n(base_start, amount, parameters.begin());
        ConsumeSink(system);
        CallMacroMethod(system, executing_macro, std::span<const u32>{parameters.data(), amount});
        macro_segments.clear();
        current_macro_dirty = false;
        return;
    }
    macro_params.insert(macro_params.end(), base_start, base_start + amount);

    // Call the macro when there are no more parameters in the command buffer
    if (is_last_call) {
        ConsumeSink(system);
        CallMacroMethod(system, executing_macro, macro_params);
        macro_params.clear();
        macro_segments.clear();
        current_macro_dirty = false;
    }
}

void Maxwell3D::RefreshParametersImpl() {
    if (!Settings::IsGPULevelHigh()) {
        return;
    }
    size_t current_index = 0;
    for (auto& segment : macro_segments) {
        if (segment.first == 0) {
            current_index += segment.second;
            continue;
        }
        memory_manager.ReadBlock(segment.first, &macro_params[current_index],
                                 sizeof(u32) * segment.second);
        current_index += segment.second;
    }
}

u32 Maxwell3D::GetMaxCurrentVertices() {
    u32 num_vertices = 0;
    for (size_t index = 0; index < Regs::NumVertexArrays; ++index) {
        const auto& array = regs.vertex_streams[index];
        if (array.enable == 0) {
            continue;
        }
        const auto& attribute = regs.vertex_attrib_format[index];
        if (attribute.constant) {
            num_vertices = (std::max)(num_vertices, 1U);
            continue;
        }
        const auto& limit = regs.vertex_stream_limits[index];
        const GPUVAddr gpu_addr_begin = array.Address();
        const GPUVAddr gpu_addr_end = limit.Address() + 1;
        const u64 stream_size = GetNativeMacroEngine().native_turing
                                    ? regs.NativeVertexStreamSize(index)
                                    : gpu_addr_end - gpu_addr_begin;
        const u32 address_size = static_cast<u32>((std::min<u64>)(stream_size, (std::numeric_limits<u32>::max)()));
        num_vertices = (std::max)(
            num_vertices, address_size / (std::max)(attribute.SizeInBytes(), array.stride.Value()));
        break;
    }
    return num_vertices;
}

size_t Maxwell3D::EstimateIndexBufferSize() {
    GPUVAddr start_address = regs.index_buffer.StartAddress();
    GPUVAddr end_address = regs.index_buffer.EndAddress();
    auto const byte_size = regs.index_buffer.FormatSizeInBytes();
    auto const max_size = 1ull << (byte_size * CHAR_BIT);
    auto const upper_cap = GetMaxCurrentVertices() * 4 * byte_size;
    auto const lower_cap = std::min<size_t>(size_t(end_address - start_address), upper_cap);
    return std::min<size_t>(memory_manager.GetMemoryLayoutSize(start_address, byte_size * max_size) / byte_size, lower_cap);
}

u32 Maxwell3D::ProcessShadowRam(u32 method, u32 argument) {
    // Keep track of the register value in shadow_state when requested.
    auto const c = shadow_state.shadow_ram_control;
    if (c == Regs::ShadowRamControl::Track || c == Regs::ShadowRamControl::TrackWithFilter)
        return shadow_state.reg_array[method] = argument;
    else if (c == Regs::ShadowRamControl::Replay)
        return shadow_state.reg_array[method];
    return argument;
}

void Maxwell3D::ConsumeSinkImpl(Core::System& system) {
    const auto control = shadow_state.shadow_ram_control;
    // Sunk methods are state writes only (not in execution_mask): skip unchanged values, as
    // CallMethod does.
    const bool skip_same = SkipSameValueWrites();
    const auto process = [&](u32 method, u32 value) {
        if (!skip_same || regs.reg_array[method] != value) {
            ProcessDirtyRegisters(method, value);
        }
    };
    if (control == Regs::ShadowRamControl::Track || control == Regs::ShadowRamControl::TrackWithFilter) {
        for (auto [method, value] : method_sink) {
            shadow_state.reg_array[method] = value;
            process(method, value);
        }
    } else if (control == Regs::ShadowRamControl::Replay) {
        for (auto [method, value] : method_sink)
            process(method, shadow_state.reg_array[method]);
    } else {
        for (auto [method, value] : method_sink)
            process(method, value);
    }
    method_sink.clear();
}

void Maxwell3D::ApplyState(Core::System& system, const u64* pairs, u32 count, bool track_shadow) {
    // What CallMethod does for each write of the block (none is executable or an instance loop
    // method), with the front's shadow RAM already applied; only each register's last value.
    ConsumeSink(system);
    const bool skip_same = SkipSameValueWrites();
    bool drew = false;
    for (u32 i = 0; i < count; ++i) {
        const u32 method = static_cast<u32>(pairs[i]);
        const u32 value = static_cast<u32>(pairs[i] >> 32);
        if (track_shadow) {
            shadow_state.reg_array[method] = value;
        }
        if (skip_same && regs.reg_array[method] == value) {
            continue;
        }
        if (!drew && draw_manager.HasDeferredDraw()) [[unlikely]] {
            draw_manager.DrawDeferred(*this, true);
            drew = true;
        }
        regs.reg_array[method] = value;
        mt_written.Mark(method);
        for (auto const& table : dirty.tables) {
            dirty.flags[table[method]] = true;
        }
    }
}

void Maxwell3D::ProcessDirtyRegisters(u32 method, u32 argument) {
    // The instances after the first are drawn together once their loop ends, and they must see
    // the state they were begun with: NVN's draw macros reset the base instance (and its copy in
    // c[0][0x644]) right after the loop, before the macro returns.
    if (draw_manager.HasDeferredDraw() && !DrawManager::IsInstanceLoopMethod(method)) [[unlikely]] {
        draw_manager.DrawDeferred(*this, true);
    }
    regs.reg_array[method] = argument;
    mt_written.Mark(method);
    for (auto const& table : dirty.tables)
        dirty.flags[table[method]] = true;
}

void Maxwell3D::ProcessMethodCall(u32 method, u32 argument, u32 nonshadow_argument, bool is_last_call) {
    if (GetNativeMacroEngine().native_turing && method == MAXWELL3D_REG_INDEX(clear_surface)) {
        LOG_DEBUG(HW_GPU, "Native GPU surface clear: value={:#x}", argument);
    }
    switch (method) {
    case RootTables::SelectMethod:
    case RootTables::LoadMethod:
        ApplyRootTableMethod(root_tables, method, argument);
        if (mt_record) [[unlikely]] {
            mt_root_ops.push_back(method | (u64{argument} << 32));
        }
        return;
    case MAXWELL3D_REG_INDEX(wait_for_idle):
        return rasterizer->WaitForIdle();
    case MAXWELL3D_REG_INDEX(shadow_ram_control):
        shadow_state.shadow_ram_control = static_cast<Regs::ShadowRamControl>(nonshadow_argument);
        return;
    // With macros on the split GPU thread's front (FrontWrite), the front loads and binds them.
    case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
        if (front_macros) return;
        return GetNativeMacroEngine().ClearCode(regs.load_mme.instruction_ptr);
    case MAXWELL3D_REG_INDEX(load_mme.instruction):
        if (front_macros) return;
        return GetNativeMacroEngine().AddCode(regs.load_mme.instruction_ptr, argument);
    case MAXWELL3D_REG_INDEX(load_mme.start_address):
        if (front_macros) {
            ++regs.load_mme.start_address_ptr;
            MtMarkField(regs.load_mme.start_address_ptr);
            return;
        }
        return ProcessMacroBind(argument);
    case MAXWELL3D_REG_INDEX(falcon[4]):
        return ProcessFirmwareCall4();
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
        return ProcessCBData(argument);
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
        return ProcessCBBind(0);
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
        return ProcessCBBind(1);
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
        return ProcessCBBind(2);
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
        return ProcessCBBind(3);
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
        return ProcessCBBind(4);
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
        return ProcessQueryGet();
    case MAXWELL3D_REG_INDEX(render_enable.mode):
        return ProcessQueryCondition();
    case MAXWELL3D_REG_INDEX(clear_report_value):
        return ProcessCounterReset();
    case MAXWELL3D_REG_INDEX(sync_info):
        return ProcessSyncPoint();
    case MAXWELL3D_REG_INDEX(launch_dma):
        return upload_state.ProcessExec(regs.launch_dma.memory_layout.Value() == Regs::LaunchDMA::Layout::Pitch);
    case MAXWELL3D_REG_INDEX(inline_data):
        upload_state.ProcessData(argument, is_last_call);
        return;
    case MAXWELL3D_REG_INDEX(fragment_barrier):
        return rasterizer->FragmentBarrier();
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
        rasterizer->InvalidateGPUCache();
        return rasterizer->WaitForIdle();
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        return rasterizer->TiledCacheBarrier();
    default:
        draw_manager.ProcessMethodCall(*this, method, argument);
        break;
    }
}

void Maxwell3D::CallMacroMethod(Core::System& system, u32 method, std::span<const u32> parameters) {
    // Reset the current macro.
    executing_macro = 0;

    // Lookup the macro offset
    const u32 entry =
        ((method - MacroRegistersStart) >> 1) % static_cast<u32>(GetNativeMacroPositions().size());

    // Execute the current macro.
    GetNativeMacroEngine().Execute(system, *this, GetNativeMacroPositions()[entry], parameters);
    draw_manager.DrawDeferred(*this);
}

// EDEN_NX2_3D_METHODS: log the first write to each 3D register (finds methods Eden doesn't model).
static const bool g_trace_first_method_writes = std::getenv("EDEN_NX2_3D_METHODS") != nullptr;

void Maxwell3D::TraceFirstMethodWrite(u32 method, u32 argument) {
    static std::array<bool, Regs::NUM_REGS> seen{};
    if (method < seen.size() && !seen[method]) {
        seen[method] = true;
        LOG_INFO(HW_GPU, "3D method first write: {:#x} (byte {:#x}) = {:#x}", method, method * 4, argument);
    }
}

void Maxwell3D::CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    // It is an error to write to a register other than the current macro's ARG register before
    // it has finished execution.
    if (executing_macro != 0 && method != executing_macro + 1) {
        DumpRecentWrites(method);
    }
    if (executing_macro != 0) {
        ASSERT(method == executing_macro + 1);
    }
    RecordWrite(method, method_argument, 1, is_last_call ? 1 : 2);

    // Methods after 0xE00 are special, they're actually triggers for some microcode that was
    // uploaded to the GPU during initialization.
    if (method >= MacroRegistersStart) {
        ProcessMacro(system, method, &method_argument, 1, is_last_call);
        return;
    }
    if (g_trace_first_method_writes) [[unlikely]] {
        TraceFirstMethodWrite(method, method_argument);
    }

    ASSERT(method < Regs::NUM_REGS && "Invalid Maxwell3D register, increase the size of the Regs structure");
    if (VideoCommon::Nx2Profile::Enabled()) [[unlikely]] {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::MethodWrites, 1);
        if (regs.reg_array[method] == method_argument) {
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::SameValueWrites, 1);
        }
    }
    if (method == MAXWELL3D_REG_INDEX(shadow_ram_control) && front_macros) [[unlikely]] {
        back_control_writes[back_control_count++ % back_control_writes.size()] = {
            method_argument, static_cast<u32>(shadow_state.shadow_ram_control), is_last_call ? 1U : 0U};
    }
    const u32 argument = ProcessShadowRam(method, method_argument);
    // A state register written with the value it holds changes nothing: no dirty flags (they
    // would make the next draw re-check that state), no method processing. Title 1 makes ~6
    // million 3D register writes a second, ~60% of them same-value, most from macro replays.
    // Methods with side effects (execution_mask: draws, clears, constant buffer data and
    // binds, semaphores, ...) always run. EDEN_NX2_SKIP_SAME=0 turns it off.
    if (!execution_mask[method]) {
        // ProcessMethodCall (and the draw manager behind it) acts only on executable methods.
        if (!SkipSameValueWrites() || regs.reg_array[method] != argument) {
            ProcessDirtyRegisters(method, argument);
        }
        return;
    }
    ProcessDirtyRegisters(method, argument);
    ProcessMethodCall(method, argument, method_argument, is_last_call);
}

void Maxwell3D::DumpRecentWrites(u32 method) {
    static u32 dumps{};
    if (dumps++ >= 4) {
        return;
    }
    LOG_ERROR(HW_GPU, "Write to {:#x} while macro {:#x} collects parameters ({} so far); last writes:",
              method, executing_macro, macro_params.size());
    if (!record_write_history) {
        LOG_ERROR(HW_GPU, "Register write history disabled; EDEN_NX2_3D_HISTORY=1 enables it");
        return;
    }
    for (size_t i = 0; i < recent_writes.size(); ++i) {
        const auto& w = recent_writes[(recent_write_index + i) % recent_writes.size()];
        if (w.amount != 0) {
            LOG_ERROR(HW_GPU, "  method {:#x} arg {:#x} amount {} pending {}", w.method, w.argument,
                      w.amount, w.pending);
        }
    }
}

void Maxwell3D::CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) {
    RecordWrite(method, amount ? base_start[0] : 0, amount, methods_pending);
    // Methods after 0xE00 are special, they're actually triggers for some microcode that was
    // uploaded to the GPU during initialization.
    if (method >= MacroRegistersStart) {
        ProcessMacro(system, method, base_start, amount, amount == methods_pending);
        return;
    }
    // As in ProcessDirtyRegisters: the paths below write without it.
    if (draw_manager.HasDeferredDraw() && !DrawManager::IsInstanceLoopMethod(method)) [[unlikely]] {
        draw_manager.DrawDeferred(*this, true);
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
        ProcessCBMultiData(base_start, amount);
        break;
    case MAXWELL3D_REG_INDEX(inline_data): {
        ASSERT(methods_pending == amount);
        upload_state.ProcessData(base_start, amount);
        return;
    }
    case MAXWELL3D_REG_INDEX(draw_inline_index):
    case MAXWELL3D_REG_INDEX(inline_index_2x16.even):
    case MAXWELL3D_REG_INDEX(inline_index_4x8.index0):
        if (shadow_state.shadow_ram_control != Regs::ShadowRamControl::Replay) {
            ProcessInlineIndexMultiData(method, base_start, amount);
            break;
        }
        [[fallthrough]];
    default:
        for (u32 i = 0; i < amount; i++) {
            CallMethod(system, method, base_start[i], methods_pending - i <= 1);
        }
        break;
    }
}

void Maxwell3D::ProcessMacroUpload(u32 data) {
    GetNativeMacroEngine().AddCode(regs.load_mme.instruction_ptr++, data);
    MtMarkField(regs.load_mme.instruction_ptr);
}

void Maxwell3D::ProcessMacroBind(u32 data) {
    const auto positions = GetNativeMacroPositions();
    const u32 index = regs.load_mme.start_address_ptr++;
    MtMarkField(regs.load_mme.start_address_ptr);
    if (index < positions.size()) {
        positions[index] = data;
    }
}

void Maxwell3D::ProcessFirmwareCall4() {
    LOG_DEBUG(HW_GPU, "(STUBBED) called");

    // Firmware call 4 is a blob that changes some registers depending on its parameters.
    // These registers don't affect emulation and so are stubbed by setting 0xd00 to 1.
    regs.shadow_scratch[0] = 1;
    MtMarkField(regs.shadow_scratch[0]);
}

void Maxwell3D::StampQueryResult(Core::System& system, u64 payload, bool long_query) {
    const GPUVAddr sequence_address{regs.report_semaphore.Address()};
    if (long_query) {
        memory_manager.Write<u64>(sequence_address + sizeof(u64), system.GPU().GetTicks());
        memory_manager.Write<u64>(sequence_address, payload);
    } else {
        memory_manager.Write<u32>(sequence_address, static_cast<u32>(payload));
    }
}

void Maxwell3D::ProcessQueryGet() {
    VideoCommon::QueryPropertiesFlags flags{};
    if (regs.report_semaphore.query.short_query == 0) {
        flags |= VideoCommon::QueryPropertiesFlags::HasTimeout;
    }
    const GPUVAddr sequence_address{regs.report_semaphore.Address()};
    const VideoCommon::QueryType query_type =
        static_cast<VideoCommon::QueryType>(regs.report_semaphore.query.report.Value());
    const u32 payload = regs.report_semaphore.payload;
    const u32 subreport = regs.report_semaphore.query.sub_report;
    switch (regs.report_semaphore.query.operation) {
    case Regs::ReportSemaphore::Operation::Release:
        if (regs.report_semaphore.query.short_query != 0) {
            flags |= VideoCommon::QueryPropertiesFlags::IsAFence;
        }
        rasterizer->Query(sequence_address, query_type, flags, payload, subreport);
        break;
    case Regs::ReportSemaphore::Operation::Acquire:
        // TODO(Blinkhawk): Under this operation, the GPU waits for the CPU to write a value that
        // matches the current payload.
        UNIMPLEMENTED_MSG("Unimplemented query operation ACQUIRE");
        break;
    case Regs::ReportSemaphore::Operation::ReportOnly:
        rasterizer->Query(sequence_address, query_type, flags, payload, subreport);
        break;
    case Regs::ReportSemaphore::Operation::Trap:
        UNIMPLEMENTED_MSG("Unimplemented query operation TRAP");
        break;
    default:
        UNIMPLEMENTED_MSG("Unknown query operation");
        break;
    }
}

void Maxwell3D::ProcessQueryCondition() {
    if (rasterizer->AccelerateConditionalRendering()) {
        execute_on = true;
        return;
    }
    const GPUVAddr condition_address{regs.render_enable.Address()};
    switch (regs.render_enable_override) {
    case Regs::RenderEnable::Override::AlwaysRender:
        execute_on = true;
        break;
    case Regs::RenderEnable::Override::NeverRender:
        execute_on = false;
        break;
    case Regs::RenderEnable::Override::UseRenderEnable: {
        switch (regs.render_enable.mode) {
        case Regs::RenderEnable::Mode::True: {
            execute_on = true;
            break;
        }
        case Regs::RenderEnable::Mode::False: {
            execute_on = false;
            break;
        }
        case Regs::RenderEnable::Mode::Conditional: {
            Regs::ReportSemaphore::Compare cmp;
            memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
            execute_on = cmp.initial_sequence != 0U && cmp.initial_mode != 0U;
            break;
        }
        case Regs::RenderEnable::Mode::IfEqual: {
            Regs::ReportSemaphore::Compare cmp;
            memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
            execute_on = cmp.initial_sequence == cmp.current_sequence &&
                         cmp.initial_mode == cmp.current_mode;
            break;
        }
        case Regs::RenderEnable::Mode::IfNotEqual: {
            Regs::ReportSemaphore::Compare cmp;
            memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
            execute_on = cmp.initial_sequence != cmp.current_sequence ||
                         cmp.initial_mode != cmp.current_mode;
            break;
        }
        default: {
            UNIMPLEMENTED_MSG("Uninplemented Condition Mode!");
            execute_on = true;
            break;
        }
        }
        break;
    }
    }
}

void Maxwell3D::ProcessCounterReset() {
    const auto query_type = [clear_report = regs.clear_report_value]() {
        switch (clear_report) {
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::ZPassPixelCount:
            return VideoCommon::QueryType::ZPassPixelCount64;
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::StreamingPrimitivesSucceeded:
            return VideoCommon::QueryType::StreamingPrimitivesSucceeded;
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::PrimitivesGenerated:
            return VideoCommon::QueryType::PrimitivesGenerated;
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::VtgPrimitivesOut:
            return VideoCommon::QueryType::VtgPrimitivesOut;
        default:
            LOG_DEBUG(HW_GPU, "Unimplemented counter reset={}", clear_report);
            return VideoCommon::QueryType::Payload;
        }
    }();
    rasterizer->ResetCounter(query_type);
}

void Maxwell3D::ProcessSyncPoint() {
    if (GetNativeMacroEngine().native_turing) {
        LOG_DEBUG(HW_GPU, "Native 3D syncpoint increment: id={}, raw={:#x}", regs.sync_info.sync_point.Value(), regs.reg_array[MAXWELL3D_REG_INDEX(sync_info)]);
    }
    const u32 sync_point = regs.sync_info.sync_point.Value();
    [[maybe_unused]] const u32 cache_flush = regs.sync_info.clean_l2.Value();
    rasterizer->SignalSyncPoint(sync_point);
}

void Maxwell3D::ProcessCBBind(size_t stage_index) {
    // Bind the buffer currently in CB_ADDRESS to the specified index in the desired shader
    // stage.
    const auto& bind_data = regs.bind_groups[stage_index];
    static std::array<std::array<bool, 32>, 5> seen{};
    if (!seen[stage_index][bind_data.shader_slot]) {
        seen[stage_index][bind_data.shader_slot] = true;
        LOG_INFO(HW_GPU, "Constant buffer bind: stage {} slot {} address {:#x} size {:#x}", stage_index,
                 bind_data.shader_slot.Value(), regs.const_buffer.Address(), regs.const_buffer.size);
    }
    if (bind_data.shader_slot >= Regs::MaxConstBuffers) {
        return; // slot field is 5 bits; the state array holds 18
    }
    auto& buffer = state.shader_stages[stage_index].const_buffers[bind_data.shader_slot];
    buffer.enabled = bind_data.valid.Value() != 0;
    buffer.address = regs.const_buffer.Address();
    buffer.size = regs.const_buffer.size;
    if (mt_record) [[unlikely]] {
        mt_cb_binds.push_back({
            .stage = static_cast<u8>(stage_index),
            .slot = static_cast<u8>(bind_data.shader_slot.Value()),
            .enabled = static_cast<u8>(buffer.enabled ? 1 : 0),
            .pad = 0,
            .size = buffer.size,
            .gpu_addr = buffer.address,
        });
    }

    const bool is_enabled = bind_data.valid.Value() != 0;
    if (!is_enabled) {
        rasterizer->DisableGraphicsUniformBuffer(stage_index, bind_data.shader_slot);
        return;
    }
    const GPUVAddr gpu_addr = regs.const_buffer.Address();
    const u32 size = regs.const_buffer.size;
    rasterizer->BindGraphicsUniformBuffer(stage_index, bind_data.shader_slot, gpu_addr, size);
}

void Maxwell3D::ProcessCBMultiData(const u32* start_base, u32 amount) {
    // Write the input value to the current const buffer at the current position.
    const GPUVAddr buffer_address = regs.const_buffer.Address();
    ASSERT(buffer_address != 0);

    // Don't allow writing past the end of the buffer.
    const size_t copy_size = amount * sizeof(u32);
    const size_t room = regs.const_buffer.offset < regs.const_buffer.size
                            ? regs.const_buffer.size - regs.const_buffer.offset
                            : 0;
    const size_t write_size = std::min(copy_size, room / sizeof(u32) * sizeof(u32));
    if (write_size != 0) {
        const GPUVAddr address{buffer_address + regs.const_buffer.offset};
        memory_manager.WriteBlockCached(address, start_base, write_size);
    }
    if (write_size != copy_size) {
        static std::atomic<bool> warned{};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            LOG_ERROR(HW_GPU,
                      "Dropping constant buffer words past the end: offset={:#x}, size={:#x}, "
                      "bytes={:#x}, written={:#x}",
                      regs.const_buffer.offset, regs.const_buffer.size, copy_size, write_size);
        }
    }

    // Increment the current buffer position.
    regs.const_buffer.offset += static_cast<u32>(copy_size);
    MtMarkField(regs.const_buffer.offset);
}

void Maxwell3D::ProcessCBData(u32 value) {
    ProcessCBMultiData(&value, 1);
}

void Maxwell3D::ProcessInlineIndexMultiData(u32 method, const u32* start_base, u32 amount) {
    if (amount == 0) {
        return;
    }
    const u32 argument = ProcessShadowRam(method, start_base[amount - 1]);
    ProcessDirtyRegisters(method, argument);
    draw_manager.SetInlineIndexBuffer(*this, method, start_base, amount);
}

Texture::TICEntry Maxwell3D::GetTICEntry(u32 tic_index) const {
    const GPUVAddr tic_address_gpu{regs.tex_header.Address() +
                                   tic_index * sizeof(Texture::TICEntry)};
    Texture::TICEntry tic_entry;
    memory_manager.ReadBlockUnsafe(tic_address_gpu, &tic_entry, sizeof(Texture::TICEntry));
    return tic_entry;
}

Texture::TSCEntry Maxwell3D::GetTSCEntry(u32 tsc_index) const {
    const GPUVAddr tsc_address_gpu{regs.tex_sampler.Address() +
                                   tsc_index * sizeof(Texture::TSCEntry)};
    Texture::TSCEntry tsc_entry;
    memory_manager.ReadBlockUnsafe(tsc_address_gpu, &tsc_entry, sizeof(Texture::TSCEntry));
    return tsc_entry;
}

u32 Maxwell3D::GetRegisterValue(u32 method) const {
    ASSERT(method < Regs::NUM_REGS && "Invalid Maxwell3D register");
    return regs.reg_array[method];
}

void Maxwell3D::SetHLEReplacementAttributeType(u32 bank, u32 offset,
                                               HLEReplacementAttributeType name) {
    const u64 key = (static_cast<u64>(bank) << 32) | offset;
    replace_table.emplace(key, name);
}

// Split GPU thread with macros on its front half (gpu_split.h, EDEN_NX2_SPLIT=2). The front keeps
// what macros read: the registers as the back thread will hold them when it reaches the same
// point of the method stream. That is every write the front sends, with the shadow RAM applied,
// plus the few changes the back makes on its own (constant buffer offset, shadow_scratch[0] on
// falcon[4], the macro bind pointer); the front loads and binds the macro code itself.
struct Maxwell3D::FrontState {
    std::array<ControlWrite, 16> control_writes{};
    u32 control_count{};
    std::array<u32, Regs::NUM_REGS> regs{};
    std::array<u32, Regs::NUM_REGS> shadow{};
    Regs::ShadowRamControl control{};
    u32 executing_macro{};
    std::vector<u32> parameters;
    u64 calls{};
    /// A method the draw manager handles was sent since the back last ran DrawDeferred: only
    /// then can it have a deferred instanced draw to issue at the end of a macro.
    bool draw_pending{true};
    bool skip_same{}; ///< SkipSameValueWrites() && FrontSkipSame(), read once
};

namespace {

// EDEN_NX2_SPLIT_VERIFY=1: every 4096 front macro calls, wait for the back and compare the
// front's registers with the back's (logs and resynchronizes differences).
bool FrontVerifyEnabled() {
    static const bool verify = std::getenv("EDEN_NX2_SPLIT_VERIFY") != nullptr;
    return verify;
}

// Executable methods ProcessMethodCall handles itself; the rest go to the draw manager.
bool FrontHandledOutsideDrawManager(u32 method) {
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    if (method >= cb_data && method < cb_data + 16) {
        return true;
    }
    switch (method) {
    case Maxwell3D::RootTables::SelectMethod:
    case Maxwell3D::RootTables::LoadMethod:
    case MAXWELL3D_REG_INDEX(wait_for_idle):
    case MAXWELL3D_REG_INDEX(shadow_ram_control):
    case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
    case MAXWELL3D_REG_INDEX(load_mme.instruction):
    case MAXWELL3D_REG_INDEX(load_mme.start_address):
    case MAXWELL3D_REG_INDEX(falcon[4]):
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
    case MAXWELL3D_REG_INDEX(render_enable.mode):
    case MAXWELL3D_REG_INDEX(clear_report_value):
    case MAXWELL3D_REG_INDEX(sync_info):
    case MAXWELL3D_REG_INDEX(launch_dma):
    case MAXWELL3D_REG_INDEX(inline_data):
    case MAXWELL3D_REG_INDEX(fragment_barrier):
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        return true;
    default:
        return false;
    }
}

// EDEN_NX2_SPLIT_SKIP=0: send state writes that keep the register's value too.
bool FrontSkipSame() {
    static const bool skip = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT_SKIP");
        return value == nullptr || value[0] != '0';
    }();
    return skip;
}

} // Anonymous namespace

Maxwell3D::FrontState& Maxwell3D::Front(Core::System& system, VideoCommon::GpuSplit& split) {
    if (!front) [[unlikely]] {
        // Copy the back's state and take the macros over while it is idle (state writes it
        // still holds in its method sink first).
        split.Drain(VideoCommon::GpuSplit::DrainReason::FrontState);
        ConsumeSink(system);
        front = std::make_unique<FrontState>();
        std::memcpy(front->regs.data(), regs.reg_array.data(), sizeof(front->regs));
        std::memcpy(front->shadow.data(), shadow_state.reg_array.data(), sizeof(front->shadow));
        front->control = shadow_state.shadow_ram_control;
        front->skip_same = SkipSameValueWrites() && FrontSkipSame();
        front->executing_macro = executing_macro;
        front->parameters = std::move(macro_params);
        macro_params.clear();
        executing_macro = 0;
        front_macros = true;
    }
    return *front;
}

// Guest memory the back writes when it runs a method sent to it (GpuSplit::NotePendingWrite).
void Maxwell3D::FrontNoteWrites(VideoCommon::GpuSplit& split, FrontState& fs, u32 method,
                                u32 value, bool sent) {
    const auto known = sent ? VideoCommon::GpuSplit::WriteKnown::Position
                            : VideoCommon::GpuSplit::WriteKnown::Drain;
    if (method == MAXWELL3D_REG_INDEX(launch_dma)) [[unlikely]] {
        // An inline upload (I2M): its destination is written as the back runs the data words.
        const auto& upload = reinterpret_cast<const Upload::Registers&>(
            fs.regs[MAXWELL3D_REG_INDEX(upload)]);
        const bool pitch = (value & 1) != 0;
        const u64 lines = std::max<u32>(upload.line_count, 1);
        const u64 size = pitch ? u64{upload.dest.pitch} * (lines - 1) + upload.line_length_in
                               : ~u64{0} >> 1;
        // EDEN_NX2_MT_LITE_I2MPOS: the note stays open while data words come (NoteI2mData).
        if (VideoCommon::MT::Lite6I2mPosition()) {
            split.OpenI2mWrite(pitch ? upload.dest.Address() : 0, size, known);
        } else {
            split.NotePendingWrite(pitch ? upload.dest.Address() : 0, size, known,
                                   VideoCommon::MT::TagI2m);
        }
    }
    if (method == MAXWELL3D_REG_INDEX(inline_data) && sent) [[unlikely]] {
        if (VideoCommon::MT::Lite6I2mPosition()) {
            split.NoteI2mData();
        }
    }
    if (method == MAXWELL3D_REG_INDEX(report_semaphore.query)) [[unlikely]] {
        // Every report but an acquire writes up to 16 bytes there.
        if ((fs.regs[method] & 3) != static_cast<u32>(Regs::ReportSemaphore::Operation::Acquire)) {
            // EDEN_NX2_MT_LITE_OVERLAY: a payload release (report Payload, no reduction) writes
            // the payload register's value as its first word.
            const u32 query = fs.regs[method];
            const bool payload_report =
                ((query >> 23) & 0x1f) == static_cast<u32>(Regs::ReportSemaphore::Report::Payload) &&
                ((query >> 3) & 1) == 0;
            const bool release =
                (query & 3) == static_cast<u32>(Regs::ReportSemaphore::Operation::Release);
            const bool payload_release =
                sent && VideoCommon::MT::LiteOverlayFirst() && release && payload_report;
            // EDEN_NX2_MT_LITE_REPORTSIZE: a short query writes one 32-bit word (no timestamp).
            const bool short_query = ((query >> 28) & 1) != 0;
            const bool report4 = short_query && VideoCommon::MT::Lite6ReportSize();
            // EDEN_NX2_MT_LITE_OVERLAY64 / SEQPOS: a 16-byte payload release (no short query: not
            // a fence) is written as a 64-bit payload by the sequencer (RasterizerVulkan::Query).
            const bool direct = sent && !short_query && release && payload_report &&
                                (VideoCommon::MT::Lite6Overlay64() ||
                                 VideoCommon::MT::Lite6SeqPosition()) &&
                                VideoCommon::MT::Lite6DirectPayloads();
            if (report4 && VideoCommon::MT::Lite6Stats()) [[unlikely]] {
                ++VideoCommon::MT::Lite6PositionStats::Get().report4;
            }
            split.NotePendingWrite(
                (GPUVAddr{fs.regs[MAXWELL3D_REG_INDEX(report_semaphore.address_high)]} << 32) |
                    fs.regs[MAXWELL3D_REG_INDEX(report_semaphore.address_low)],
                report4 ? 4 : 16, known,
                payload_report ? VideoCommon::MT::TagPayloadReport
                               : VideoCommon::MT::TagCounterReport,
                payload_release
                    ? std::optional<u32>{fs.regs[MAXWELL3D_REG_INDEX(report_semaphore.payload)]}
                    : std::nullopt,
                direct ? VideoCommon::GpuSplit::WriteDirect64 : u8{0});
        }
    }
}

// EDEN_NX2_MT_LITE_REGDEDUP (video_core/mt/regdedup.h): the latch-only table (built once: not
// executable, no front-side handling, not an instance loop method, not the MME DMA read
// triggers 0x157/0x158, not a puller method), and the front's registers once it exists.
void Maxwell3D::RegDedupArm() {
    namespace RD = VideoCommon::MT::RegDedup;
    static const RD::LatchTable table = [this] {
        RD::LatchTable latch;
        for (u32 m = 0x40; m < RD::LatchTable::Size; ++m) {
            if (execution_mask[m] || IsMethodExecutable(m) || FrontHandledOutsideDrawManager(m) ||
                DrawManager::IsInstanceLoopMethod(m) || m == 0x157 || m == 0x158) {
                continue;
            }
            latch.Set(m);
        }
        LOG_INFO(HW_GPU, "Register dedup: {} latch-only 3D registers", latch.Count());
        return latch;
    }();
    regdedup_latch = &table;
    if (front && regdedup_front.latch == nullptr) {
        regdedup_front.latch = &table; // Tried (it stays unarmed without skip_same).
        if (front->skip_same && RD::Armable()) {
            regdedup_front.regs = front->regs.data();
            regdedup_front.shadow = front->shadow.data();
            regdedup_front.control = &front->control;
            regdedup_front.draw_pending = &front->draw_pending;
        }
    }
}

void Maxwell3D::FrontWrite(VideoCommon::GpuSplit& split, FrontState& fs, u32 method,
                           u32 argument, bool is_last_call, bool from_macro) {
    namespace Profile = VideoCommon::Nx2Profile;
    // EDEN_NX2_MT_LITE_REGDEDUP_CHECK: a write the dedup would have dropped must take the
    // same-value return below.
    const bool dedup_check = VideoCommon::MT::RegDedup::Check() &&
                             regdedup_front.Drops(method, argument);
    // As ProcessShadowRam does on the back.
    u32 value = argument;
    bool shadow_kept = true;
    if (fs.control == Regs::ShadowRamControl::Track ||
        fs.control == Regs::ShadowRamControl::TrackWithFilter) {
        shadow_kept = fs.shadow[method] == argument;
        fs.shadow[method] = argument;
    } else if (fs.control == Regs::ShadowRamControl::Replay) {
        value = fs.shadow[method];
    }
    if (Profile::Enabled()) [[unlikely]] {
        Profile::Add(Profile::Counter::MethodWrites, 1);
    }
    if (method == MAXWELL3D_REG_INDEX(shadow_ram_control) && FrontVerifyEnabled()) [[unlikely]] {
        fs.control_writes[fs.control_count++ % fs.control_writes.size()] = {
            argument, static_cast<u32>(fs.control), from_macro ? 2U : 1U};
    }
    if (!execution_mask[method]) {
        // Nothing would change on the back: CallMethod and ConsumeSinkImpl skip same-value state
        // writes, and the shadow RAM (tracked by default) holds the value already.
        if (fs.regs[method] == value && shadow_kept && fs.skip_same) {
            if (Profile::Enabled()) [[unlikely]] {
                Profile::Add(Profile::Counter::SameValueWrites, 1);
            }
            if (dedup_check) [[unlikely]] {
                VideoCommon::MT::RegDedup::Checked(false, method, argument);
            }
            return;
        }
        if (dedup_check) [[unlikely]] {
            VideoCommon::MT::RegDedup::Checked(true, method, argument);
        }
        fs.regs[method] = value;
        // Instance loop methods keep their own entries: the back issues a deferred instanced
        // draw at the first other method, so their order against it matters.
        if (fs.skip_same && split.StateBlocks() && !DrawManager::IsInstanceLoopMethod(method)) {
            split.StateWrite(this, method, value,
                             fs.control == Regs::ShadowRamControl::Track ||
                                 fs.control == Regs::ShadowRamControl::TrackWithFilter);
            return;
        }
    } else {
        fs.regs[method] = value;
        constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
        switch (method) {
        case MAXWELL3D_REG_INDEX(shadow_ram_control):
            // The back's mode is shadow_state.shadow_ram_control: the shadow copy of this very
            // register, which a later Replay write reads back.
            fs.control = static_cast<Regs::ShadowRamControl>(argument);
            fs.shadow[method] = argument;
            break;
        case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
            GetNativeMacroEngine().ClearCode(fs.regs[MAXWELL3D_REG_INDEX(load_mme.instruction_ptr)]);
            break;
        case MAXWELL3D_REG_INDEX(load_mme.instruction):
            GetNativeMacroEngine().AddCode(fs.regs[MAXWELL3D_REG_INDEX(load_mme.instruction_ptr)], value);
            break;
        case MAXWELL3D_REG_INDEX(load_mme.start_address): {
            const auto positions = GetNativeMacroPositions();
            u32& pointer = fs.regs[MAXWELL3D_REG_INDEX(load_mme.start_address_ptr)];
            if (pointer < positions.size()) {
                positions[pointer] = value;
            }
            ++pointer;
            break;
        }
        case MAXWELL3D_REG_INDEX(falcon[4]):
            fs.regs[MAXWELL3D_REG_INDEX(shadow_scratch)] = 1;
            break;
        default:
            if (method >= cb_data && method < cb_data + 16) {
                fs.regs[MAXWELL3D_REG_INDEX(const_buffer.offset)] += sizeof(u32);
            } else if (!FrontHandledOutsideDrawManager(method)) {
                fs.draw_pending = true;
            }
            break;
        }
    }
    if (from_macro) {
        // EDEN_NX2_CB_BATCH=0 sends each word on its own (=a alternates). Not with the shadow
        // RAM replaying: the back takes the word from it then.
        static const char cb_batch = Tegra::Nx2EnvMode("EDEN_NX2_CB_BATCH");
        constexpr u32 cb_first = MAXWELL3D_REG_INDEX(const_buffer.buffer);
        if (method >= cb_first && method < cb_first + 16 &&
            fs.control != Regs::ShadowRamControl::Replay && Tegra::Nx2Switch(cb_batch, true)) {
            split.MacroWrite(this, method, argument, true);
        } else {
            split.MacroWrite(this, method, argument);
        }
    } else {
        split.Method(this, method, argument, is_last_call, nullptr);
    }
    FrontNoteWrites(split, fs, method, value, true);
    if (method == MAXWELL3D_REG_INDEX(report_semaphore.query)) [[unlikely]] {
        // A release that writes its payload (no counter, no reduction): acquires on the front can
        // count on it before the back runs it (GpuSplit::PredictedRelease).
        const u32 query = fs.regs[MAXWELL3D_REG_INDEX(report_semaphore.query)];
        const bool release = (query & 3) == static_cast<u32>(Regs::ReportSemaphore::Operation::Release);
        const bool payload_report = ((query >> 23) & 0x1f) == 0;
        const bool reduction = ((query >> 3) & 1) != 0;
        if (release && payload_report && !reduction) {
            const GPUVAddr address =
                (GPUVAddr{fs.regs[MAXWELL3D_REG_INDEX(report_semaphore.address_high)]} << 32) |
                fs.regs[MAXWELL3D_REG_INDEX(report_semaphore.address_low)];
            split.NoteRelease(address, fs.regs[MAXWELL3D_REG_INDEX(report_semaphore.payload)]);
        }
    }
}

void Maxwell3D::FrontCallMethod(Core::System& system, VideoCommon::GpuSplit& split, u32 method,
                                u32 argument, bool is_last_call) {
    auto& fs = Front(system, split);
    if (regdedup_front.latch == nullptr) [[unlikely]] {
        RegDedupArm(); // EDEN_NX2_MT_LITE_REGDEDUP: DmaPusher filters before this call.
    }
    if (method >= MacroRegistersStart) {
        if (fs.executing_macro == 0) {
            fs.executing_macro = method;
        }
        fs.parameters.push_back(argument);
        if (is_last_call) {
            FrontRunMacro(system, split, fs);
        }
        return;
    }
    FrontWrite(split, fs, method, argument, is_last_call, false);
}

void Maxwell3D::FrontWriteRun(Core::System& system, VideoCommon::GpuSplit& split, u32 method,
                              const u32* words, u32 count, bool ends_method) {
    auto& fs = Front(system, split);
    namespace RD = VideoCommon::MT::RegDedup;
    if (regdedup_front.latch == nullptr) [[unlikely]] {
        RegDedupArm();
    }
    // EDEN_NX2_MT_LITE_REGDEDUP: same-value writes to latch-only registers go first, before any
    // bookkeeping (FrontWrite would skip them with no effect).
    const bool dedup = regdedup_front.Armed() && RD::Active();
    u32 dropped = 0;
    if (!VideoCommon::MT::LiteFrontBatch()) {
        for (u32 i = 0; i < count; ++i) {
            if (dedup && regdedup_front.Drops(method + i, words[i])) {
                ++dropped;
                continue;
            }
            FrontWrite(split, fs, method + i, words[i], ends_method && i + 1 == count, false);
        }
        if (dropped != 0) {
            RD::Dropped(RD::Site::Dma, dropped, true);
        }
        return;
    }
    // EDEN_NX2_MT_LITE_FBATCH: a plain state write is FrontWrite's non-executable branch inline
    // (shadow RAM, same-value skip, state block). Anything else takes FrontWrite, which may change
    // fs.control, so the mode is read again for every word.
    namespace Profile = VideoCommon::Nx2Profile;
    const bool general = !fs.skip_same || FrontVerifyEnabled(); // Fixed for the FrontState.
    const bool check = RD::Check() && regdedup_front.Armed();
    for (u32 i = 0; i < count; ++i) {
        const u32 m = method + i;
        const u32 arg = words[i];
        if (dedup && regdedup_front.Drops(m, arg)) {
            ++dropped;
            continue;
        }
        if (general || execution_mask[m] || !split.StateBlocks() ||
            fs.control == Regs::ShadowRamControl::Replay || DrawManager::IsInstanceLoopMethod(m)) {
            FrontWrite(split, fs, m, arg, ends_method && i + 1 == count, false);
            continue;
        }
        // EDEN_NX2_MT_LITE_REGDEDUP_CHECK: predicted before the shadow word changes.
        const bool predicted = check && regdedup_front.Drops(m, arg);
        const bool tracked = fs.control == Regs::ShadowRamControl::Track ||
                             fs.control == Regs::ShadowRamControl::TrackWithFilter;
        bool kept = true;
        if (tracked) {
            kept = fs.shadow[m] == arg;
            fs.shadow[m] = arg;
        }
        if (Profile::Enabled()) [[unlikely]] {
            Profile::Add(Profile::Counter::MethodWrites, 1);
        }
        if (fs.regs[m] == arg && kept) {
            if (Profile::Enabled()) [[unlikely]] {
                Profile::Add(Profile::Counter::SameValueWrites, 1);
            }
            if (predicted) [[unlikely]] {
                RD::Checked(false, m, arg);
            }
            continue;
        }
        if (predicted) [[unlikely]] {
            RD::Checked(true, m, arg);
        }
        fs.regs[m] = arg;
        split.StateWrite(this, m, arg, tracked);
    }
    if (dropped != 0) {
        RD::Dropped(RD::Site::Dma, dropped, true);
    }
}

void Maxwell3D::FrontCallMultiMethod(Core::System& system, VideoCommon::GpuSplit& split,
                                     u32 method, const u32* base_start, u32 amount,
                                     u32 methods_pending) {
    auto& fs = Front(system, split);
    if (method >= MacroRegistersStart) {
        if (fs.executing_macro == 0) {
            fs.executing_macro = method;
        }
        fs.parameters.insert(fs.parameters.end(), base_start, base_start + amount);
        if (amount == methods_pending) {
            FrontRunMacro(system, split, fs);
        }
        return;
    }
    // EDEN_NX2_MT_LITE_REGDEDUP: the leading words of a non-incrementing run to a latch-only
    // register that keep its value change nothing (the back's CallMethod skips each). The back's
    // CallMultiMethod also issues a pending deferred draw first: the whole run goes only when no
    // draw manager method was sent since the back last ran DrawDeferred (else one word stays).
    if (regdedup_front.latch == nullptr) [[unlikely]] {
        RegDedupArm();
    }
    if (amount != 0 && regdedup_front.Armed() && VideoCommon::MT::RegDedup::Active() &&
        regdedup_front.Drops(method, base_start[0])) {
        u32 leading = 1;
        while (leading < amount && regdedup_front.Drops(method, base_start[leading])) {
            ++leading;
        }
        if (leading == amount && *regdedup_front.draw_pending) {
            --leading;
        }
        if (leading != 0) {
            VideoCommon::MT::RegDedup::Dropped(VideoCommon::MT::RegDedup::Site::Multi, leading,
                                               false);
            if (leading == amount) {
                return;
            }
            base_start += leading;
            amount -= leading;
            methods_pending -= leading;
        }
    }
    // What CallMultiMethod does to the registers on the back, then one entry for all the words.
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    const auto tracked = [&fs] {
        return fs.control == Regs::ShadowRamControl::Track ||
               fs.control == Regs::ShadowRamControl::TrackWithFilter;
    };
    if (method >= cb_data && method < cb_data + 16) {
        fs.regs[MAXWELL3D_REG_INDEX(const_buffer.offset)] += amount * sizeof(u32);
    } else if (method == MAXWELL3D_REG_INDEX(inline_data)) {
        // Upload data only.
    } else if ((method == MAXWELL3D_REG_INDEX(draw_inline_index) ||
                method == MAXWELL3D_REG_INDEX(inline_index_2x16.even) ||
                method == MAXWELL3D_REG_INDEX(inline_index_4x8.index0)) &&
               fs.control != Regs::ShadowRamControl::Replay) {
        if (amount != 0) {
            const u32 last = base_start[amount - 1];
            if (tracked()) {
                fs.shadow[method] = last;
            }
            fs.regs[method] = last;
        }
        fs.draw_pending = true;
    } else {
        // CallMethod for each word, macro uploads among them (load_mme.instruction). A word may
        // change the shadow RAM mode for the next (a run of SET_SHADOW_RAM_CONTROL writes).
        for (u32 i = 0; i < amount; ++i) {
            const u32 argument = base_start[i];
            u32 value = argument;
            if (method == MAXWELL3D_REG_INDEX(shadow_ram_control) && FrontVerifyEnabled()) [[unlikely]] {
                fs.control_writes[fs.control_count++ % fs.control_writes.size()] = {
                    argument, static_cast<u32>(fs.control), 3U};
            }
            if (tracked()) {
                fs.shadow[method] = argument;
            } else if (fs.control == Regs::ShadowRamControl::Replay) {
                value = fs.shadow[method];
            }
            fs.regs[method] = value;
            if (!execution_mask[method]) {
                continue;
            }
            FrontNoteWrites(split, fs, method, value, false);
            switch (method) {
            case MAXWELL3D_REG_INDEX(shadow_ram_control):
                fs.control = static_cast<Regs::ShadowRamControl>(argument);
                fs.shadow[method] = argument; // The same word on the back (see FrontWrite)
                break;
            case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
                GetNativeMacroEngine().ClearCode(value);
                break;
            case MAXWELL3D_REG_INDEX(load_mme.instruction):
                GetNativeMacroEngine().AddCode(fs.regs[MAXWELL3D_REG_INDEX(load_mme.instruction_ptr)], value);
                break;
            case MAXWELL3D_REG_INDEX(load_mme.start_address): {
                const auto positions = GetNativeMacroPositions();
                u32& pointer = fs.regs[MAXWELL3D_REG_INDEX(load_mme.start_address_ptr)];
                if (pointer < positions.size()) {
                    positions[pointer] = value;
                }
                ++pointer;
                break;
            }
            case MAXWELL3D_REG_INDEX(falcon[4]):
                fs.regs[MAXWELL3D_REG_INDEX(shadow_scratch)] = 1;
                break;
            default:
                if (!FrontHandledOutsideDrawManager(method)) {
                    fs.draw_pending = true;
                }
                break;
            }
        }
    }
    split.MultiMethod(this, method, base_start, amount, methods_pending, nullptr);
    if (method == MAXWELL3D_REG_INDEX(inline_data) && VideoCommon::MT::Lite6I2mPosition())
        [[unlikely]] {
        split.NoteI2mData(); // EDEN_NX2_MT_LITE_I2MPOS: upload data words just sent
    }
}

void Maxwell3D::FrontRunMacro(Core::System& system, VideoCommon::GpuSplit& split,
                              FrontState& fs) {
#ifdef ARCHITECTURE_x86_64
    const u32 method = fs.executing_macro;
    fs.executing_macro = 0;
    auto& engine = GetNativeMacroEngine();
    const auto positions = GetNativeMacroPositions();
    const u32 start = positions[((method - MacroRegistersStart) >> 1) % positions.size()];
    {
        VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Macro};
        engine.native_interpreter.active_dram = &native_data_ram;
        if (!engine.native_jit) {
            engine.native_jit = std::make_shared<TuringMacroJit>();
        }
        engine.native_jit->Prepare(engine.native_code, engine.native_code_generation);
        const size_t parameter_count = fs.parameters.size();
        // EDEN_NX2_MT_LITE_FCALL: a same-value state write FrontWrite would drop returns in the
        // emit callback (no profile counters or verify records to keep then).
        const bool fast_same = VideoCommon::MT::LiteFrontCalls() && fs.skip_same &&
                               !VideoCommon::Nx2Profile::Enabled() && !FrontVerifyEnabled();
        // EDEN_NX2_MT_LITE_REGDEDUP: same-value writes to latch-only registers return first
        // (counted for the profile as FrontWrite would).
        if (regdedup_front.latch == nullptr) [[unlikely]] {
            RegDedupArm();
        }
        const bool dedup = regdedup_front.Armed() && VideoCommon::MT::RegDedup::Active();
        u32 dedup_dropped = 0;
        const auto result = engine.native_jit->Execute(
            start, fs.parameters, std::span<u32>{native_data_ram},
            [&fs](u32 address) { return address < Regs::NUM_REGS ? fs.regs[address] : 0U; },
            [&](u32 address, u32 value) {
                if (address >= MacroRegistersStart) {
                    return false;
                }
                if (dedup && regdedup_front.Drops(address, value)) {
                    ++dedup_dropped;
                    return true;
                }
                // FrontWrite's same-value return, by shadow RAM mode (Replay writes the shadow
                // word). Executable methods never skip, nor the MME DMA reads below (0x157 and
                // 0x158 are not in execution_mask: a read repeating the last count must run).
                if (fast_same && !execution_mask[address] && address != 0x157 &&
                    address != 0x158) {
                    bool same;
                    if (fs.control == Regs::ShadowRamControl::Track ||
                        fs.control == Regs::ShadowRamControl::TrackWithFilter) {
                        same = fs.shadow[address] == value && fs.regs[address] == value;
                    } else if (fs.control == Regs::ShadowRamControl::Replay) {
                        same = fs.regs[address] == fs.shadow[address];
                    } else {
                        same = fs.regs[address] == value;
                    }
                    if (same) {
                        return true;
                    }
                }
                FrontWrite(split, fs, address, value, true, true);
                // MME_DMA_READ (into data RAM) and MME_DMA_READ_FIFOED (into the load FIFO).
                if (address == 0x157 || address == 0x158) {
                    FrontDmaRead(split, fs, address, value);
                }
                return true;
            });
        if (dedup_dropped != 0) {
            VideoCommon::MT::RegDedup::Dropped(VideoCommon::MT::RegDedup::Site::Macro,
                                               dedup_dropped, true);
        }
        if (!result.success) {
            engine.native_execution_failed = true;
            LOG_ERROR(HW_GPU, "Native MME (JIT, split front) failed: start={:#x}, consumed={}/{}, error={}",
                      start, result.consumed, parameter_count, result.error);
        }
    }
    fs.parameters.clear();
    if (fs.draw_pending) {
        fs.draw_pending = false;
        split.Defer([this, system_ptr = &system] { FinishFrontMacro(*system_ptr); });
    }
    if (FrontVerifyEnabled() && (++fs.calls & 4095) == 0) [[unlikely]] {
        FrontVerify(split, fs);
    }
#else
    fs.executing_macro = 0;
    fs.parameters.clear();
#endif
}

void Maxwell3D::FrontDmaRead(VideoCommon::GpuSplit& split, FrontState& fs, u32 method,
                             u32 count) {
    VideoCommon::MT::Lite6LogOnce();
    const GPUVAddr source = (GPUVAddr{fs.regs[0x154] & 0x1ffffff} << 32) | fs.regs[0x155];
    const u32 ram = fs.regs[0x156];
    const u32 words = std::min<u32>(count, 0x1000);
    const size_t read_bytes = size_t{words} * sizeof(u32);
    // EDEN_NX2_DMA_READ_CHECK=1: compare the words before and after the drain (how often the
    // queued work changes them), with the source pages' GPU-written state.
    static const bool check = std::getenv("EDEN_NX2_DMA_READ_CHECK") != nullptr;
    std::array<u32, 64> before{};
    const u32 checked = std::min<u32>(words, 64);
    bool was_gpu_written = false;
    if (check) [[unlikely]] {
        memory_manager.ReadBlockUnsafe(source, before.data(), checked * sizeof(u32));
        if (const auto device = memory_manager.GpuToCpuAddress(source)) {
            was_gpu_written = Tegra::Nx2IsGpuWritten(*device);
        }
    }
    // The words may be written by methods still queued for the back (indirect draw arguments
    // from a culling dispatch), and the caches that may hold newer ones belong to it. Title 1
    // read ~1,000 times a second, each waiting ~0.3 ms for the back to finish everything sent;
    // the drain changed the words ~1 time in 10,000, always on pages the GPU hadn't written
    // (inline uploads and reports). Those are noted (NotePendingWrite), copy and compute work as
    // writers: reads they can't touch only wait for the writers. EDEN_NX2_DMA_READ_DRAIN=1
    // always drains.
    static const bool always_drain = [] {
        const char* value = std::getenv("EDEN_NX2_DMA_READ_DRAIN");
        return value != nullptr && value[0] == '1';
    }();
    bool on_gpu_written = false;
    for (GPUVAddr page = source & ~GPUVAddr{0xfff}; page < source + read_bytes; page += 0x1000) {
        if (const auto device = memory_manager.GpuToCpuAddress(page);
            !device || Tegra::Nx2IsGpuWritten(*device)) {
            on_gpu_written = true;
        }
    }
    // A read under writes the front noted waits only until the back has run them (their
    // positions), not for everything sent: title 1's ~235 such reads a second each waited ~1 ms
    // for a full drain. Off by default: no stale read in ~60,000 checked (EDEN_NX2_DMA_READ_CHECK),
    // 2,200 more draws a second, but 1.2 fewer frames (median, 36 windows).
    // EDEN_NX2_DMA_READ_PARTIAL=1 turns it on (=a alternates); on by default under
    // EDEN_NX2_MT_LITE (EDEN_NX2_MT_LITE_DMAPARTIAL): there a full drain also idles the sequencer
    // until the front sends again, and position waits only go up to the committer's guest writes.
    static const char partial_mode = Tegra::Nx2EnvMode("EDEN_NX2_DMA_READ_PARTIAL");
    const auto pending = split.PendingWritesFor(source, read_bytes);
    // EDEN_NX2_MT_LITE_OVERLAY: a read whose queued writes all have known values takes the
    // overlay below (no wait) before the partial wait: with DMAPARTIAL on, every one of them
    // waited for its newest release (~67 a second, ~1.2 ms each in title 1).
    static const char overlay_mode = Tegra::Nx2EnvMode("EDEN_NX2_DMA_READ_OVERLAY");
    static std::vector<VideoCommon::GpuSplit::KnownWord> known_words;
    known_words.clear();
    u8 overlay_refusal = 0; // EDEN_NX2_MT_LITE stats: why KnownPendingWords said no
    const bool overlay_first =
        VideoCommon::MT::LiteOverlayFirst() && !always_drain && !on_gpu_written &&
        !pending.drain && !pending.writers && pending.position != 0 &&
        Tegra::Nx2Switch(overlay_mode, true) &&
        split.KnownPendingWords(source, read_bytes, known_words, &overlay_refusal);
    if (!overlay_first) {
        known_words.clear();
    }
    const bool partial = !overlay_first && !always_drain && !on_gpu_written && !pending.drain &&
                         (pending.writers || pending.position != 0) &&
                         Tegra::Nx2Switch(partial_mode, VideoCommon::MT::LiteDmaPartial());
    const bool plain_skip = !always_drain && !on_gpu_written && !pending.drain &&
                            !pending.writers && pending.position == 0;
    // Title 1's ~230 waiting reads a second (EDEN_NX2_DMA_READ_WHY) cover fence releases still
    // queued, whose payloads the front knows: the read gets them laid over memory instead of
    // draining the back. Only releases the back hasn't run count; memory has the rest.
    // EDEN_NX2_DMA_READ_OVERLAY=0 drains as before (=a alternates).
    const bool overlay = overlay_first ||
                         (!always_drain && !on_gpu_written && !pending.drain && !pending.writers &&
                          pending.position != 0 && !partial &&
                          Tegra::Nx2Switch(overlay_mode, true) &&
                          split.KnownPendingWords(source, read_bytes, known_words,
                                                  &overlay_refusal));
    if (!overlay) {
        known_words.clear();
    }
    const bool skipped = plain_skip || overlay;
    const auto apply_known = [&](u32* data, u32 count) {
        for (const auto& known : known_words) {
            if (known.address >= source && known.address + 4 <= source + u64{count} * 4 &&
                ((known.address - source) & 3) == 0) {
                data[(known.address - source) / 4] = known.value;
            } else if (known.address < source + u64{count} * 4 && source < known.address + 4) {
                // Unaligned against the read: patch byte by byte.
                for (u64 b = 0; b < 4; ++b) {
                    const GPUVAddr at = known.address + b;
                    if (at >= source && at < source + u64{count} * 4) {
                        reinterpret_cast<u8*>(data)[at - source] =
                            static_cast<u8>(known.value >> (8 * b));
                    }
                }
            }
        }
    };
    // EDEN_NX2_DMA_READ_WHY: why MME DMA reads wait (logged every 10 s).
    static const bool why = std::getenv("EDEN_NX2_DMA_READ_WHY") != nullptr;
    if (why) [[unlikely]] {
        static u64 n_total{}, n_gpu{}, n_overflow{}, n_drain{}, n_writers{}, n_position{};
        static auto why_window = std::chrono::steady_clock::now();
        ++n_total;
        n_gpu += on_gpu_written ? 1 : 0;
        n_overflow += split.PendingWriteOverflow() ? 1 : 0;
        n_drain += pending.drain && !split.PendingWriteOverflow() ? 1 : 0;
        n_writers += pending.writers ? 1 : 0;
        n_position += pending.position != 0 ? 1 : 0;
        // Tags: VideoCommon::MT::Lite6WriteTag (reports: counter 2, payload 3).
        static std::array<u64, VideoCommon::MT::TagCount> n_tag{};
        const u32 tags = split.PendingWriteTags(source, read_bytes);
        for (u32 t = 0; t < VideoCommon::MT::TagCount; ++t) {
            n_tag[t] += (tags >> t) & 1;
        }
        if (std::chrono::steady_clock::now() - why_window > std::chrono::seconds{10}) {
            LOG_INFO(HW_GPU, "MME DMA read waits in 10 s: {} reads, {} GPU-written, {} table "
                     "overflow, {} drain-known writes, {} writers, {} positions (I2M {}, "
                     "report {} (counter {}, payload {}), puller release {}, qmd {}, other {}); "
                     "last {:#x} {} bytes",
                     n_total, n_gpu, n_overflow, n_drain, n_writers, n_position,
                     n_tag[VideoCommon::MT::TagI2m],
                     n_tag[VideoCommon::MT::TagCounterReport] +
                         n_tag[VideoCommon::MT::TagPayloadReport],
                     n_tag[VideoCommon::MT::TagCounterReport],
                     n_tag[VideoCommon::MT::TagPayloadReport],
                     n_tag[VideoCommon::MT::TagPullerRelease], n_tag[VideoCommon::MT::TagQmd],
                     n_tag[VideoCommon::MT::TagOther], source, read_bytes);
            n_tag = {};
            n_total = n_gpu = n_overflow = n_drain = n_writers = n_position = 0;
            why_window = std::chrono::steady_clock::now();
        }
    }
    // A read that only waits for the copy and compute work queued before it (skipped) did so
    // for title 1's ~1,000 reads a second: each one left the back without work while the front
    // waited, so the two took turns. As DmaPusher does for command lists (IsKnownCpuCommandList),
    // pages a read found unwritten by the GPU after such a wait skip it next time; a page the
    // back has since marked GPU-written drains above (marks are never cleared). The risk is a
    // dispatch still queued writing such a page for the first time. EDEN_NX2_DMA_READ_KNOWN=1
    // skips (=a alternates); on by default under EDEN_NX2_MT_LITE (EDEN_NX2_MT_LITE_DMAKNOWN):
    // under the MT back each writer wait waits for the sequencer to run everything up to the
    // last writer (~900/s, ~45 ms/s in title 1).
    static const char known_mode = Tegra::Nx2EnvMode("EDEN_NX2_DMA_READ_KNOWN");
    static std::vector<u64> known_source_pages; // Front thread only; one bit a device page.
    const auto known_source = [&](bool note) {
        if (known_source_pages.empty()) {
            known_source_pages.resize((u64{1} << (36 - 12)) / 64);
        }
        for (GPUVAddr page = source & ~GPUVAddr{0xfff}; page < source + read_bytes;
             page += 0x1000) {
            const auto device = memory_manager.GpuToCpuAddress(page);
            if (!device || (*device >> 12) / 64 >= known_source_pages.size()) {
                return false;
            }
            const u64 index = *device >> 12;
            u64& word = known_source_pages[index / 64];
            if (note) {
                word |= u64{1} << (index % 64);
            } else if (((word >> (index % 64)) & 1) == 0) {
                return false;
            }
        }
        return true;
    };
    // EDEN_NX2_GPU_PROFILE / EDEN_NX2_PULLER_STATS: position waits by kind (lite 6 line).
    const bool lite6_stats = VideoCommon::MT::Lite6Stats();
    if (lite6_stats) [[unlikely]] {
        auto& stats = VideoCommon::MT::Lite6PositionStats::Get();
        stats.refused_no_value += overlay_refusal == 1 ? 1 : 0;
        stats.refused_past_known += overlay_refusal == 2 ? 1 : 0;
        stats.open_i2m += pending.open_i2m ? 1 : 0;
        stats.overlay_reads += overlay ? 1 : 0;
        stats.writers_only += partial && pending.position == 0 ? 1 : 0;
        stats.MaybeLog();
    }
    if (partial) {
        if (pending.position != 0) {
            // EDEN_NX2_MT_LITE_SEQPOS: the committer only for the writes it makes; with writers
            // over the read (QMD release words, written by fences) as far as before.
            const u64 settle = pending.writers ? pending.position : pending.settle_position;
            const u64 begin = lite6_stats ? VideoCommon::Nx2Profile::Now() : 0;
            const bool waited = split.WaitForPosition(
                pending.position, settle, VideoCommon::GpuSplit::DrainReason::Position);
            if (lite6_stats) [[unlikely]] {
                auto& stats = VideoCommon::MT::Lite6PositionStats::Get();
                stats.Note(VideoCommon::MT::Lite6PositionStats::KindOf(pending.newest_tag,
                                                                       pending.newest_direct),
                           settle == 0, waited,
                           waited ? VideoCommon::Nx2Profile::Now() - begin : 0);
            }
        } else {
            split.WaitForPosition(0);
        }
        split.WaitForWriters(VideoCommon::GpuSplit::DrainReason::WritersDmaRead);
        if (check) [[unlikely]] {
            memory_manager.ReadBlockUnsafe(source, before.data(), checked * sizeof(u32));
            split.Drain(VideoCommon::GpuSplit::DrainReason::DmaRead);
        }
    } else if (skipped) {
        if (!(Tegra::Nx2Switch(known_mode, VideoCommon::MT::LiteDmaKnown()) &&
              known_source(false))) {
            split.WaitForWriters(VideoCommon::GpuSplit::DrainReason::WritersDmaRead);
            // Still not GPU-written with the writers run: a CPU-written page (so far).
            bool written = false;
            for (GPUVAddr page = source & ~GPUVAddr{0xfff}; page < source + read_bytes;
                 page += 0x1000) {
                if (const auto device = memory_manager.GpuToCpuAddress(page);
                    !device || Tegra::Nx2IsGpuWritten(*device)) {
                    written = true;
                }
            }
            if (!written) {
                known_source(true);
            }
        }
        if (check) [[unlikely]] {
            // What the read sees without the drain, compared below with what it sees after one.
            memory_manager.ReadBlockUnsafe(source, before.data(), checked * sizeof(u32));
            apply_known(before.data(), checked);
            split.Drain(VideoCommon::GpuSplit::DrainReason::DmaRead);
        }
    } else if (!on_gpu_written) {
        // EDEN_NX2_MT_DMA_CONSUMED: no GPU data to download here; under the multithreaded back
        // the committer's queued guest writes are all the read needs past the sequencer.
        split.DrainForRead(VideoCommon::GpuSplit::DrainReason::DmaRead);
    } else {
        split.Drain(VideoCommon::GpuSplit::DrainReason::DmaRead);
    }
    if (check) [[unlikely]] {
        if (memory_manager.IsMemoryDirty(source, read_bytes, VideoCommon::CacheType::BufferCache)) {
            split.RunFlush([memory = &memory_manager, source, read_bytes] {
                memory->FlushRegion(source, read_bytes, VideoCommon::CacheType::BufferCache);
            }, VideoCommon::GpuSplit::DrainReason::DmaRead);
        }
        std::array<u32, 64> after{};
        memory_manager.ReadBlockUnsafe(source, after.data(), checked * sizeof(u32));
        static u64 reads{}, changed{}, gpu_pages{}, changed_cpu{}, skips{}, stale_skips{};
        static auto window = std::chrono::steady_clock::now();
        const bool differs = std::memcmp(before.data(), after.data(), checked * sizeof(u32)) != 0;
        ++reads;
        changed += differs ? 1 : 0;
        gpu_pages += was_gpu_written ? 1 : 0;
        changed_cpu += differs && !was_gpu_written ? 1 : 0;
        skips += skipped || partial ? 1 : 0;
        stale_skips += (skipped || partial) && differs ? 1 : 0;
        if (std::chrono::steady_clock::now() - window > std::chrono::seconds{10}) {
            LOG_INFO(HW_GPU, "MME DMA reads: {} in 10 s, {} changed by the drain, {} on GPU-written "
                     "pages, {} changed on other pages, {} without a drain ({} of them stale); "
                     "last {:#x} ({} words)",
                     reads, changed, gpu_pages, changed_cpu, skips, stale_skips, source, words);
            reads = changed = gpu_pages = changed_cpu = skips = stale_skips = 0;
            window = std::chrono::steady_clock::now();
        }
    }
    if (VideoCommon::Nx2Profile::Enabled()) {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::DmaReadBytes, read_bytes);
    }
    if (memory_manager.IsMemoryDirty(source, read_bytes, VideoCommon::CacheType::BufferCache)) {
        // The buffer cache holds GPU data there (indirect arguments a dispatch wrote): the flush
        // downloads it, which waits for the host GPU to finish everything (runtime.Finish), ms
        // each. EDEN_NX2_DMA_READ_WHY also logs these (count, time, why the read drained).
        const u64 begin = why ? VideoCommon::Nx2Profile::Now() : 0;
        split.RunFlush([memory = &memory_manager, source, read_bytes] {
            memory->FlushRegion(source, read_bytes, VideoCommon::CacheType::BufferCache);
        }, VideoCommon::GpuSplit::DrainReason::DmaRead);
        if (why) [[unlikely]] {
            static u64 n_flush{}, flush_ns{}, n_flush_gpu{}, n_flush_skipped{}, n_flush_ram{};
            static auto flush_window = std::chrono::steady_clock::now();
            ++n_flush;
            flush_ns += VideoCommon::Nx2Profile::Now() - begin;
            n_flush_gpu += on_gpu_written ? 1 : 0;
            n_flush_skipped += skipped || partial ? 1 : 0;
            n_flush_ram += method == 0x158 ? 0 : 1;
            if (std::chrono::steady_clock::now() - flush_window > std::chrono::seconds{10}) {
                LOG_INFO(HW_GPU, "MME DMA read downloads in 10 s: {} ({:.1f} ms), {} on "
                         "GPU-written pages, {} after a skipped drain, {} into data RAM (rest "
                         "macro parameters); last {:#x} {} bytes",
                         n_flush, static_cast<double>(flush_ns) / 1e6, n_flush_gpu,
                         n_flush_skipped, n_flush_ram, source, read_bytes);
                n_flush = flush_ns = n_flush_gpu = n_flush_skipped = n_flush_ram = 0;
                flush_window = std::chrono::steady_clock::now();
            }
        }
    }
    static std::vector<u32> overlaid;
    if (!known_words.empty()) {
        overlaid.resize(words);
        memory_manager.ReadBlockUnsafe(source, overlaid.data(), read_bytes);
        apply_known(overlaid.data(), words);
    }
    for (u32 i = 0; i < words; ++i) {
        const u32 word = known_words.empty()
                             ? memory_manager.Read<u32>(source + u64{i} * sizeof(u32))
                             : overlaid[i];
        if (method == 0x158) {
            fs.parameters.push_back(word);
        } else if (ram + i < native_data_ram.size()) {
            native_data_ram[ram + i] = word;
        }
    }
}

void Maxwell3D::FrontVerify(VideoCommon::GpuSplit& split, FrontState& fs) {
    split.Drain(VideoCommon::GpuSplit::DrainReason::Verify);
    static u64 checks{};
    static u64 differences{};
    ++checks;
    // State writes the back still holds in its method sink count as done.
    std::array<u32, Regs::NUM_REGS> expected;
    std::memcpy(expected.data(), regs.reg_array.data(), sizeof(expected));
    for (const auto& [method, value] : method_sink) {
        expected[method] = shadow_state.shadow_ram_control == Regs::ShadowRamControl::Replay
                               ? shadow_state.reg_array[method]
                               : value;
    }
    static u64 control_differences{};
    if ((fs.control != shadow_state.shadow_ram_control ||
         fs.shadow[MAXWELL3D_REG_INDEX(shadow_ram_control)] != shadow_state.reg_array[MAXWELL3D_REG_INDEX(shadow_ram_control)] ||
         fs.regs[MAXWELL3D_REG_INDEX(shadow_ram_control)] != expected[MAXWELL3D_REG_INDEX(shadow_ram_control)]) &&
        control_differences < 4) {
        LOG_WARNING(HW_GPU, "Split shadow RAM control writes, front ({} total) / back ({} total):",
                    fs.control_count, back_control_count);
        for (u32 i = 0; i < 16; ++i) {
            const auto& f = fs.control_writes[(fs.control_count + i) % 16];
            const auto& b = back_control_writes[(back_control_count + i) % 16];
            LOG_WARNING(HW_GPU, "  front {:#x} (mode {}, path {}) | back {:#x} (mode {}, last {})",
                        f.argument, f.before, f.path, b.argument, b.before, b.path);
        }
    }
    for (u32 i = 0; i < Regs::NUM_REGS; ++i) {
        if (fs.regs[i] != expected[i]) {
            if (differences++ < 64) {
                LOG_WARNING(HW_GPU, "Split front register {:#x}: front {:#x}, back {:#x}", i,
                            fs.regs[i], expected[i]);
            }
            fs.regs[i] = expected[i];
        }
    }
    if (fs.control != shadow_state.shadow_ram_control) {
        if (control_differences++ < 16) {
            LOG_WARNING(HW_GPU, "Split front shadow RAM control {} vs back {}",
                        static_cast<u32>(fs.control),
                        static_cast<u32>(shadow_state.shadow_ram_control));
        }
        fs.control = shadow_state.shadow_ram_control;
    }
    for (u32 i = 0; i < Regs::NUM_REGS; ++i) {
        if (fs.shadow[i] != shadow_state.reg_array[i]) {
            if (control_differences++ < 16) {
                LOG_WARNING(HW_GPU, "Split front shadow register {:#x}: front {:#x}, back {:#x}", i,
                            fs.shadow[i], shadow_state.reg_array[i]);
            }
            fs.shadow[i] = shadow_state.reg_array[i];
        }
    }
    if ((checks & 63) == 1) {
        LOG_INFO(HW_GPU, "Split front register checks: {} done, {} differences", checks, differences);
    }
}

void Maxwell3D::FinishFrontMacro(Core::System& system) {
    // What ProcessMacro and CallMacroMethod do around the macro.
    ConsumeSink(system);
    draw_manager.DrawDeferred(*this);
}

// ---- GPU-written macro parameters (video_core/mt/gpuparam.h, docs/gpuparam-plan.md) ----

namespace GP = VideoCommon::MT::GpuParam;
static_assert(MAXWELL3D_REG_INDEX(draw.end) == GP::RegDrawEnd);
static_assert(MAXWELL3D_REG_INDEX(draw.begin) == GP::RegDrawBegin);
static_assert(MAXWELL3D_REG_INDEX(index_buffer.format) == GP::RegIndexFormat);
static_assert(MAXWELL3D_REG_INDEX(index_buffer.first) == GP::RegIndexFirst);
static_assert(MAXWELL3D_REG_INDEX(index_buffer.count) == GP::RegIndexCount);
static_assert(MAXWELL3D_REG_INDEX(vertex_id_base) == GP::RegVertexIdBase);
static_assert(MAXWELL3D_REG_INDEX(global_base_vertex_index) == GP::RegBaseVertex);
static_assert(MAXWELL3D_REG_INDEX(global_base_instance_index) == GP::RegBaseInstance);
static_assert(Maxwell3D::Regs::MeshControl == GP::RegMeshControl);
static_assert(MacroRegistersStart == GP::MacroRegistersStart);
static_assert(MAXWELL3D_REG_INDEX(const_buffer.size) == GP::RegCbSize);
static_assert(MAXWELL3D_REG_INDEX(const_buffer.address_low) == GP::RegCbAddressLow);
static_assert(MAXWELL3D_REG_INDEX(const_buffer.offset) == GP::RegCbOffset);
static_assert(MAXWELL3D_REG_INDEX(const_buffer.buffer) == GP::RegCbData);
static_assert(static_cast<u32>(Maxwell3D::Regs::IndexFormat::UnsignedByte) == 0);

struct Maxwell3D::GpuParamState {
    GP::Classifier classifier;
    GP::Stats stats;
    std::vector<u32> words;  ///< EDEN_NX2_GPUPARAM_CHECK: the words read as before
    std::vector<u32> params; ///< the zero-parameter call without the split front
    std::vector<GP::DrawRecord> actual;
    std::vector<GP::DrawRecord> expected;
    u32 mismatch_logs{};
    // GPU params v2 (mt/gpuparam2.h).
    GP::V2Stats v2;
    Maxwell3D* self{};
    Core::System* system{};
    /// The pending group (valid while the split's front hold context is this state).
    GP::Call group{};
    u32 group_macro{};
    /// A joining call's zero-word call runs with the hold installed: its constant buffer
    /// writes (and const_buffer.offset) pass, anything else sends the group first.
    bool tolerant{};

    ~GpuParamState() {
        auto& split = VideoCommon::GpuSplit::Get();
        if (split.FrontHoldContext() == this) {
            split.TakeFrontHold();
        }
    }
    void MaybeLog() {
        stats.MaybeLog([this] {
            std::string text = v2.Format();
            v2 = GP::V2Stats{};
            return text;
        });
    }
};

bool Maxwell3D::GpuParamCall(Core::System& system, u32 method, const GP::Segment* segments,
                             size_t segment_count, const GpuParamReader* check_reader) {
    auto& split = VideoCommon::GpuSplit::Get();
    const bool front_path = split.Active() && split.FrontMacros();
#ifndef ARCHITECTURE_x86_64
    if (front_path) {
        return false; // FrontRunMacro needs the macro JIT
    }
#endif
    if (segment_count == 0 || !GP::On(front_path && split.MtActive())) {
        return false;
    }
    auto& engine = GetNativeMacroEngine();
    if (!gpuparam) {
        gpuparam = std::make_unique<GpuParamState>();
    }
    GpuParamState& gs = *gpuparam;
    const auto keep_waiting = [&gs](GP::Reason reason) {
        gs.stats.Note(reason);
        gs.MaybeLog();
        return false;
    };
    if (!engine.native_turing) {
        return keep_waiting(GP::Reason::NoCode);
    }
    FrontState* const fs = front_path ? &Front(system, split) : nullptr;
    const u32 current = fs != nullptr ? fs->executing_macro : executing_macro;
    const u32 macro_method = current != 0 ? current : (method & ~1U);
    const std::vector<u32>& cpu = fs != nullptr ? fs->parameters : macro_params;
    const u32* const reg_file = fs != nullptr ? fs->regs.data() : regs.reg_array.data();
    const auto control = fs != nullptr ? fs->control : shadow_state.shadow_ram_control;

    // The layout: p0 from the CPU, then 5 words, or a draw count and N records of 5 words (in
    // one entry, or a 1-word entry and an entry of records).
    if (cpu.size() != 1) {
        return keep_waiting(GP::Reason::CpuParams);
    }
    u64 words = 0;
    for (size_t i = 0; i < segment_count; ++i) {
        words += segments[i].words;
    }
    GP::Call call{};
    call.begin = cpu[0];
    if (words == 5 && segment_count == 1) {
        call.kind = GP::Kind::IndexedSingle;
        call.records_addr = segments[0].gpu_addr;
        call.max_draws = 1;
    } else if (words >= 6 && (words - 1) % 5 == 0 && words <= 1 + 5 * 4096) {
        call.kind = GP::Kind::IndexedMulti;
        call.max_draws = static_cast<u32>((words - 1) / 5);
        if (segment_count == 1) {
            call.count_addr = segments[0].gpu_addr;
            call.records_addr = segments[0].gpu_addr + sizeof(u32);
        } else if (segment_count == 2 && segments[0].words == 1) {
            call.count_addr = segments[0].gpu_addr;
            call.records_addr = segments[1].gpu_addr;
        } else {
            return keep_waiting(GP::Reason::Layout);
        }
    } else {
        return keep_waiting(GP::Reason::Layout);
    }
    if (((call.begin >> 26) & 3) != 0) {
        return keep_waiting(GP::Reason::InstanceBits);
    }
    if (!GP::IsTopologySafe(call.begin & 0xffff)) {
        return keep_waiting(GP::Reason::Topology);
    }
    if (control == Regs::ShadowRamControl::Replay) {
        return keep_waiting(GP::Reason::ShadowReplay);
    }
    if ((reg_file[GP::RegMeshControl] & 1) != 0) {
        return keep_waiting(GP::Reason::Mesh);
    }
    if (reg_file[GP::RegIndexFormat] == 0) {
        return keep_waiting(GP::Reason::IndexU8);
    }

    // The macro's code against the reference model (cached unless it reads state).
    const auto positions = GetNativeMacroPositions();
    const u32 index = (macro_method - MacroRegistersStart) >> 1;
    const std::span<const u32> code_ram{engine.native_code};
    const std::span<const u32> reg_span{reg_file, Regs::NUM_REGS};
    const auto executable = [this](u32 m) {
        return m < execution_mask.size() && execution_mask[m];
    };
    const auto verdict = gs.classifier.Verify(code_ram, positions, index,
                                              engine.native_code_generation, call.kind, call.begin,
                                              call.max_draws, reg_span, native_data_ram, executable);
    gs.stats.verified += verdict.cached ? 0 : 1;
    if (!verdict.ok) {
        ++gs.stats.rejects[static_cast<size_t>(verdict.reject)];
        return keep_waiting(GP::Reason::Rejected);
    }
    call.cb = verdict.cb;
    // GPU params v2 (mt/gpuparam2.h): single calls whose constant buffer words are exactly the
    // base vertex/instance draw with the built-ins, without copies; the rest as v1.
    const bool mt = front_path && split.MtActive();
    if (GP::V2On(mt)) {
        ++gs.v2.calls;
        if (call.kind != GP::Kind::IndexedSingle) {
            ++gs.v2.fallbacks[static_cast<size_t>(GP::V2Fallback::Kind)];
        } else if (!GP::BuiltinPlan(call.cb)) {
            ++gs.v2.fallbacks[static_cast<size_t>(GP::V2Fallback::Plan)];
        } else {
            call.builtins = true;
            ++gs.v2.copy_free;
        }
    }

    // EDEN_NX2_GPUPARAM_CHECK: the words as the old path reads them (after their writers), and
    // the macro's draws for them against the indirect draw's.
    if (check_reader != nullptr && *check_reader) {
        gs.words.resize(words);
        bool read = true;
        size_t offset = 0;
        for (size_t i = 0; i < segment_count && read; ++i) {
            read = (*check_reader)(segments[i].gpu_addr, segments[i].words,
                                   gs.words.data() + offset);
            offset += segments[i].words;
        }
        if (!read) {
            ++gs.stats.check_unreadable;
        } else {
            std::string_view error;
            bool cb_same = true;
            const u32 start = positions[index % positions.size()];
            const bool same = gs.classifier.Compare(code_ram, start, call.kind, call.begin,
                                                    call.max_draws, call.cb, gs.words, reg_span,
                                                    native_data_ram, executable, gs.actual,
                                                    gs.expected, error, cb_same);
            ++gs.stats.check_compared;
            gs.stats.check_draws += gs.expected.size();
            if (call.builtins) {
                // v2: what the built-ins give (record words 3/4) against the constant buffer
                // words the macro writes before its draws for the real words.
                ++gs.v2.check_compared;
                if (!gs.classifier.BuiltinsMatch(gs.words)) {
                    ++gs.v2.check_mismatch;
                    if (gs.mismatch_logs++ < 16) {
                        LOG_WARNING(HW_GPU,
                                    "GPU params check (v2): macro {} at {:#x}: constant buffer "
                                    "words differ from record words 3/4 ({:#x}, {:#x})",
                                    index, call.records_addr, gs.words.size() > 3 ? gs.words[3] : 0U,
                                    gs.words.size() > 4 ? gs.words[4] : 0U);
                    }
                }
            }
            if (!same) {
                ++gs.stats.check_mismatch;
                if (gs.mismatch_logs++ < 16) {
                    size_t first_difference = 0;
                    while (first_difference < gs.actual.size() &&
                           first_difference < gs.expected.size() &&
                           gs.actual[first_difference] == gs.expected[first_difference]) {
                        ++first_difference;
                    }
                    const auto describe = [](const std::vector<GP::DrawRecord>& draws, size_t i) {
                        if (i >= draws.size()) {
                            return std::string{"none"};
                        }
                        const auto& d = draws[i];
                        return fmt::format("begin {:#x} count {} first {} base vertex {:#x} "
                                           "base instance {} indexed {}",
                                           d.begin, d.count, d.first, d.base_vertex,
                                           d.base_instance, d.indexed);
                    };
                    LOG_WARNING(HW_GPU,
                                "GPU params check: macro {} at {:#x} ({} words): macro draws {}, "
                                "indirect draws {}{}{}{}; first difference #{}: macro {} / "
                                "indirect {}; words {:x}",
                                index, call.records_addr, words, gs.actual.size(),
                                gs.expected.size(), cb_same ? "" : ", constant buffer words differ",
                                error.empty() ? "" : ", error ", error,
                                first_difference, describe(gs.actual, first_difference),
                                describe(gs.expected, first_difference),
                                fmt::join(std::span<const u32>{gs.words}.first(
                                              std::min<size_t>(gs.words.size(), 16)),
                                          ","));
                }
            }
        }
    }

    // The call with zero GPU words: the registers it leaves, no draw. Then the indirect draw.
    if (fs != nullptr) {
#ifdef ARCHITECTURE_x86_64
        // v2 groups (MT back only): the draw is held (GpuSplit front hold) and the next call
        // joins it when its records follow and nothing was sent in between; a joining call's
        // zero-word call runs with the hold installed (GpuParamHoldRelease lets its constant
        // buffer writes pass: with the built-ins the held draws don't read those words, and
        // the call writes the same values each time).
        const bool group = mt && call.builtins && GP::GroupCap() > 1;
        bool join = false;
        if (void* const held = split.FrontHoldContext(); held != nullptr) {
            GP::NoJoin no_join = GP::NoJoin::Other;
            if (held != &gs || !group) {
                no_join = GP::NoJoin::Other;
            } else if (!verdict.group_safe) {
                no_join = GP::NoJoin::Unsafe;
            } else if (gs.group_macro != macro_method) {
                no_join = GP::NoJoin::Macro;
            } else if (gs.group.begin != call.begin) {
                no_join = GP::NoJoin::Begin;
            } else if (!GP::SamePlan(gs.group.cb, call.cb)) {
                no_join = GP::NoJoin::Plan;
            } else if (call.records_addr !=
                       gs.group.records_addr + u64{gs.group.max_draws} * GP::RecordBytes) {
                no_join = GP::NoJoin::Address;
            } else {
                join = true;
            }
            if (!join) {
                ++gs.v2.no_join[static_cast<size_t>(no_join)];
                split.ReleaseFrontHold(VideoCommon::GpuSplit::HoldNonJoinable);
            }
        }
        fs->executing_macro = macro_method;
        fs->parameters.resize(1 + words, 0U);
        gs.tolerant = join;
        FrontRunMacro(system, split, *fs);
        gs.tolerant = false;
        if (join && split.FrontHoldContext() == &gs) {
            ++gs.group.max_draws;
            if (gs.group.max_draws >= GP::GroupCap()) {
                split.ReleaseFrontHold(VideoCommon::GpuSplit::HoldFull);
            }
        } else if (group && verdict.group_safe) {
            // A new group (the zero call's other emissions, if any, sent the old one first).
            gs.self = this;
            gs.system = &system;
            gs.group = call;
            gs.group_macro = macro_method;
            split.SetFrontHold(&Maxwell3D::GpuParamHoldRelease, &gs);
        } else {
            struct BackDraw {
                Maxwell3D* self;
                Core::System* system;
                GP::Call call;
                void operator()() const {
                    self->GpuParamDraw(*system, call);
                }
            };
            split.Defer(BackDraw{this, &system, call});
        }
#endif
    } else {
        ConsumeSink(system);
        gs.params.assign(macro_params.begin(), macro_params.end());
        gs.params.resize(1 + words, 0U);
        CallMacroMethod(system, macro_method, gs.params);
        macro_params.clear();
        macro_segments.clear();
        current_macro_dirty = false;
        GpuParamDraw(system, call);
    }
    gs.stats.Note(GP::Reason::Deferred);
    ++(call.kind == GP::Kind::IndexedSingle ? gs.stats.single : gs.stats.multi);
    gs.stats.entries += segment_count;
    gs.MaybeLog();
    return true;
}

void Maxwell3D::GpuParamDraw(Core::System& system, const GP::Call& call) {
    // As CallMacroMethod/FinishFrontMacro before the macro's draw: pending state, then a pending
    // instanced draw. Then HLE_DrawIndexedIndirect's draw with the words in GPU memory.
    ConsumeSink(system);
    draw_manager.DrawDeferred(*this);
    // The constant buffer words the draws read (driver constants: base vertex/instance at
    // 0x640/0x644 for macro 40). The zero-word call wrote them all already, with its final
    // values: put the draw-time values there first (GPU words as GPU copies from the parameter
    // buffer, after its writer in queue order), restore the final ones after the draw.
    // v2: the vertex stage reads the words as built-ins (only from bank 0); else v1's copies,
    // one record at a time for a group.
    bool builtins = false;
    if (call.builtins) {
        const int bank = GpuParamVertexBank();
        if (bank == 0) {
            builtins = true;
            GP::g_v2_builtin_draws.fetch_add(1, std::memory_order_relaxed);
            GP::g_v2_builtin_records.fetch_add(call.max_draws, std::memory_order_relaxed);
        } else {
            (bank < 0 ? GP::g_v2_fallback_no_bank : GP::g_v2_fallback_bank)
                .fetch_add(1, std::memory_order_relaxed);
            GP::g_v2_fallback_records.fetch_add(call.max_draws, std::memory_order_relaxed);
            for (u32 i = 0; i < call.max_draws; ++i) {
                GP::Call single = call;
                single.builtins = false;
                single.max_draws = 1;
                single.records_addr = call.records_addr + u64{i} * GP::RecordBytes;
                GpuParamDraw(system, single);
            }
            return;
        }
    }
    const bool has_cb = !builtins && (call.cb.pre_count != 0 || call.cb.post_count != 0);
    if (has_cb) {
        // The zero-word call's cached constant buffer writes reach the buffer cache first, so
        // the copy's destination sync uploads them before the copy, never after it.
        memory_manager.FlushCaching();
        GpuParamConstantWords(std::span{call.cb.pre.data(), call.cb.pre_count}, call);
    }
    auto& params = draw_manager.indirect_state;
    params.is_byte_count = false;
    params.is_indexed = true;
    params.include_count = call.count_addr != 0;
    params.count_start_address = call.count_addr;
    params.indirect_start_address = call.records_addr;
    params.buffer_size = size_t{call.max_draws} * 5 * sizeof(u32);
    params.max_draw_counts = call.max_draws;
    // A group (max_draws > 1, no count word): vkCmdDrawIndexedIndirect drawCount N, stride 20
    // (multiDrawIndirect is a required device feature).
    params.stride = call.kind == GP::Kind::IndexedSingle && call.max_draws <= 1 ? 0
                                                                                 : 5 * sizeof(u32);
    const u32 estimate = static_cast<u32>(EstimateIndexBufferSize());
    dirty.flags[VideoCommon::Dirty::IndexBuffer] = true;
    if (builtins) {
        engine_state = EngineHint::OnGpuParamDraw;
        gpuparam_cb_bank = 0;
    }
    draw_manager.DrawIndexedIndirect(*this, static_cast<Regs::PrimitiveTopology>(call.begin & 0xffff),
                                     0, estimate);
    if (builtins) {
        engine_state = EngineHint::None;
        gpuparam_cb_bank = 0;
    }
    dirty.flags[VideoCommon::Dirty::IndexBuffer] = true;
    // A drawing call leaves no indexed draw pending (DrawEnd clears it).
    draw_manager.draw_state.draw_indexed = false;
    if (has_cb) {
        GpuParamConstantWords(std::span{call.cb.post.data(), call.cb.post_count}, call);
    }
}

void Maxwell3D::GpuParamConstantWords(std::span<const GP::CbWord> cb_words, const GP::Call& call) {
    // As ProcessCBMultiData: at const_buffer address + offset, nothing past const_buffer.size.
    const GPUVAddr base = regs.const_buffer.Address();
    const u32 size = regs.const_buffer.size;
    if (base == 0) {
        return;
    }
    for (size_t i = 0; i < cb_words.size();) {
        const GP::CbWord& word = cb_words[i];
        if (word.gpu_word == GP::NoGpuWord) {
            if (u64{word.offset} + sizeof(u32) <= size) {
                memory_manager.WriteBlockCached(base + word.offset, &word.value, sizeof(u32));
            }
            ++i;
            continue;
        }
        // A run of GPU words consecutive in both the parameters and the constant buffer: one
        // buffer copy (BufferCopy: syncs the destination, copies on the GPU, marks it
        // GPU-modified so the draw binds the buffer instead of streaming guest memory).
        size_t run = 1;
        while (i + run < cb_words.size() && cb_words[i + run].gpu_word != GP::NoGpuWord &&
               cb_words[i + run].gpu_word == word.gpu_word + run &&
               cb_words[i + run].offset == word.offset + run * sizeof(u32)) {
            ++run;
        }
        const u64 bytes = std::min<u64>(run * sizeof(u32),
                                        word.offset < size ? (size - word.offset) & ~3U : 0);
        if (bytes != 0) {
            const GPUVAddr source = call.records_addr + u64{word.gpu_word} * sizeof(u32);
            const GPUVAddr dest = base + word.offset;
            if (!rasterizer->AccessAccelerateDMA().BufferCopy(source, dest, bytes)) {
                // Neither range in the buffer cache: guest memory (MaxwellDMA's fallback).
                GP::g_cb_copy_fallbacks.fetch_add(1, std::memory_order_relaxed);
                std::array<u32, GP::MaxCbWords> copied{};
                memory_manager.ReadBlock(source, copied.data(), bytes);
                memory_manager.WriteBlockCached(dest, copied.data(), bytes);
            }
        }
        i += run;
    }
}

void Maxwell3D::GpuParamHoldRelease(void* context, u32 why) {
    auto& gs = *static_cast<GpuParamState*>(context);
    if (gs.tolerant) {
        // A joining call's zero-word call: its constant buffer data, const_buffer.offset and
        // index_buffer.first writes (the same values every call) pass; the hold stays.
        const u32 kind = why & ~0xffffU;
        const u32 method = why & 0xffffU;
        if ((kind == VideoCommon::GpuSplit::HoldStateWrite ||
             kind == VideoCommon::GpuSplit::HoldMacroWrite) &&
            (GP::IsCbData(method) || method == GP::RegCbOffset || method == GP::RegIndexFirst)) {
            VideoCommon::GpuSplit::Get().SetFrontHold(&Maxwell3D::GpuParamHoldRelease, context);
            return;
        }
        ++gs.v2.zero_call_breaks;
    }
    gs.self->GpuParamEmitGroup(why);
}

void Maxwell3D::GpuParamEmitGroup(u32 why) {
    GpuParamState& gs = *gpuparam;
    gs.v2.NoteGroup(gs.group.max_draws, why);
    struct BackDraw {
        Maxwell3D* self;
        Core::System* system;
        GP::Call call;
        void operator()() const {
            self->GpuParamDraw(*system, call);
        }
    };
    // The hold is already cleared: Defer does not come back here.
    VideoCommon::GpuSplit::Get().Defer(BackDraw{this, gs.system, gs.group});
}

int Maxwell3D::GpuParamVertexBank() const {
    // The selected constant buffer (where the macro writes the driver constants) as bound to
    // the vertex stage (shader_stages[0]: VertexA/VertexB).
    const GPUVAddr address = regs.const_buffer.Address();
    const auto& banks = state.shader_stages[0].const_buffers;
    for (size_t i = 0; i < banks.size(); ++i) {
        if (banks[i].enabled && banks[i].address == address) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace Tegra::Engines
