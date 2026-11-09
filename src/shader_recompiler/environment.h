// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include "common/container/unordered_map.h"

#include "common/common_types.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/shader_info.h"
#include "shader_recompiler/stage.h"
#include "shader_recompiler/frontend/ir/value.h"

namespace Shader::IR {
class Inst;
}

namespace Shader {

struct CbufWordKey {
    u32 index;
    u32 offset;
    constexpr bool operator==(const CbufWordKey& o) const noexcept {
        return index == o.index && offset == o.offset;
    }
};

struct CbufWordKeyHash {
    constexpr size_t operator()(const CbufWordKey& k) const noexcept {
        return (size_t(k.index) << 32) ^ k.offset;
    }
};

struct HandleKey {
    u32 index, offset, shift_left;
    u32 sec_index, sec_offset, sec_shift_left;
    u32 count;
    bool has_secondary;
    constexpr bool operator==(const HandleKey& o) const noexcept {
        return std::tie(index, offset, shift_left, sec_index, sec_offset, sec_shift_left, count, has_secondary)
            == std::tie(o.index, o.offset, o.shift_left, o.sec_index, o.sec_offset, o.sec_shift_left, o.count, o.has_secondary);
    }
};
struct HandleKeyHash {
    constexpr size_t operator()(const HandleKey& k) const noexcept {
        size_t h = (size_t(k.index) << 32) ^ k.offset;
        h ^= (size_t(k.shift_left) << 1);
        h ^= (size_t(k.sec_index) << 33) ^ (size_t(k.sec_offset) << 2);
        h ^= (size_t(k.sec_shift_left) << 3);
        h ^= (size_t(k.count) << 7);
        return h ^ (k.has_secondary ? 0x9e3779b97f4a7c15ULL : 0ULL);
    }
};

struct ConstBufferAddr {
    u32 index;
    u32 offset;
    u32 shift_left;
    u32 secondary_index;
    u32 secondary_offset;
    u32 secondary_shift_left;
    IR::U32 dynamic_offset;
    u32 count;
    bool has_secondary;
    u32 handle_program{~0U}; ///< index in Info::handle_programs for a handle computed per draw
};

class Environment {
public:
    virtual ~Environment() = default;

    [[nodiscard]] virtual u64 ReadInstruction(u32 address) = 0;

    [[nodiscard]] virtual u32 TranslateProgramAddress(u32 address) const { return address; }

    /// A Switch 2 native program lowered to Maxwell instructions. Its storage buffer pointers are
    /// raw 8-byte arguments in constant buffers, without the NVN descriptor layout.
    [[nodiscard]] virtual bool IsNativeProgram() const { return false; }

    /// Native programs: whether the 8-byte constant at (index, offset) is a mapped GPU address.
    /// Native argument tables mix pointers with scalars (sizes, strides).
    [[nodiscard]] virtual bool IsCbufStorageBufferPointer(u32, u32) { return true; }

    [[nodiscard]] virtual u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) = 0;

    [[nodiscard]] virtual TextureType ReadTextureType(u32 raw_handle) = 0;

    [[nodiscard]] virtual TexturePixelFormat ReadTexturePixelFormat(u32 raw_handle) = 0;

    [[nodiscard]] virtual bool IsTexturePixelFormatInteger(u32 raw_handle) = 0;

    [[nodiscard]] virtual u32 ReadViewportTransformState() = 0;

    [[nodiscard]] virtual u32 TextureBoundBuffer() const = 0;

    [[nodiscard]] virtual u32 LocalMemorySize() const = 0;

    [[nodiscard]] virtual u32 SharedMemorySize() const = 0;

    [[nodiscard]] virtual std::array<u32, 3> WorkgroupSize() const = 0;

    [[nodiscard]] virtual bool HasHLEMacroState() const = 0;

    [[nodiscard]] virtual std::optional<ReplaceConstant> GetReplaceConstBuffer(u32 bank,
                                                                               u32 offset) = 0;

    virtual void Dump(u64 pipeline_hash, u64 shader_hash) = 0;

    /// GPU params v2 (docs/mt-lite-plan6.md, W1): the pipeline is drawn with the hint
    /// Maxwell3D::EngineHint::OnGpuParamDraw, whose fixed table reads cbuf[bank][0x640] as the
    /// BaseVertex built-in and cbuf[bank][0x644] as BaseInstance (vertex stages only). ~0U: no
    /// such hint. Set from the pipeline key / the engine when the environment is made.
    [[nodiscard]] u32 GpuParamDrawBank() const noexcept {
        return gpuparam_draw_bank;
    }
    void SetGpuParamDrawBank(u32 bank) noexcept {
        gpuparam_draw_bank = bank;
    }
    /// Translation under that hint: a read of those words left as a memory read (not a vertex
    /// stage, or a width other than one word), or a read of that bank at a dynamic offset.
    void NoteGpuParamRead(bool dynamic) noexcept {
        ++(dynamic ? gpuparam_dynamic_reads : gpuparam_memory_reads);
    }
    [[nodiscard]] u32 GpuParamMemoryReads() const noexcept {
        return gpuparam_memory_reads;
    }
    [[nodiscard]] u32 GpuParamDynamicReads() const noexcept {
        return gpuparam_dynamic_reads;
    }
    /// GPU params R1 (b) (video_core/mt/gpuparam3.h, EDEN_NX2_GPUPARAM2_LDC64): under that hint,
    /// a two-word read cb[bank][0x640] becomes (BaseVertex, BaseInstance) when both words have
    /// a replacement. Set with the bank by whoever makes the environment.
    [[nodiscard]] bool GpuParamTwoWordReplace() const noexcept {
        return gpuparam_two_word;
    }
    void SetGpuParamTwoWordReplace(bool on) noexcept {
        gpuparam_two_word = on;
    }
    void NoteGpuParamTwoWordReplaced() noexcept {
        ++gpuparam_two_word_replaced;
    }
    [[nodiscard]] u32 GpuParamTwoWordReplaced() const noexcept {
        return gpuparam_two_word_replaced;
    }

    [[nodiscard]] const ProgramHeader& SPH() const noexcept {
        return sph;
    }

    [[nodiscard]] const std::array<u32, 8>& GpPassthroughMask() const noexcept {
        return gp_passthrough_mask;
    }

    [[nodiscard]] Stage ShaderStage() const noexcept {
        return stage;
    }

    [[nodiscard]] u32 StartAddress() const noexcept {
        return start_address;
    }

    [[nodiscard]] bool IsProprietaryDriver() const noexcept {
        return is_proprietary_driver;
    }

    /// Task and mesh programs: SET_MESH_SHADER_A, SET_MESH_SHADER_B and SET_MESH_INIT_SHADER.
    [[nodiscard]] const std::array<u32, 3>& MeshRegisters() const noexcept {
        return mesh_registers;
    }

    /// Mesh programs: the header of the pass-through geometry program bound with them, whose
    /// output map lists the per-primitive outputs.
    [[nodiscard]] const ProgramHeader& MeshPrimitiveHeader() const noexcept {
        return mesh_primitive_header;
    }

protected:
    ProgramHeader sph{};
    std::array<u32, 8> gp_passthrough_mask{};
    std::array<u32, 3> mesh_registers{};
    ProgramHeader mesh_primitive_header{};
    Stage stage{};
    u32 start_address{};
    bool is_proprietary_driver{};
    u32 gpuparam_draw_bank = ~0U;
    u32 gpuparam_memory_reads{};
    u32 gpuparam_dynamic_reads{};
    bool gpuparam_two_word{};
    u32 gpuparam_two_word_replaced{};
public:
    ::Common::unordered_map<CbufWordKey, u32, CbufWordKeyHash> cbuf_word_cache;
    ::Common::unordered_map<HandleKey,  u32, HandleKeyHash> handle_cache;
    ::Common::unordered_map<const IR::Inst*, ConstBufferAddr> track_cache;
};

} // namespace Shader
