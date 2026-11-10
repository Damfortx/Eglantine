// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Upstream d1195b7d49 (stage_cache.h + stage_cache.cpp), ported header-only: this branch adds no
// .cpp files. Included by vk_pipeline_cache.cpp only.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/nx2.h"

namespace VideoCommon {

/// Reuses translated and emitted native shader stages across pipelines.
///
/// Pipelines that differ only in fixed state (blending, depth, render targets) translate the same
/// programs again: in Ocarina of Time 64% of the stages a cold boot translated gave SPIR-V the
/// same program had already given, half of the translation time. A stage's IR depends only on its
/// program, the environment's constant properties and the answers to the reads translation makes
/// (constant buffer values, texture types and formats, storage pointer checks, ...); its SPIR-V
/// also on the runtime info and the bindings the earlier stages took. A translation is reused when
/// those reads, replayed in the order translation made them, give the same answers: translation
/// is deterministic, so every read replayed up to a difference is one it would make too. Emitted
/// stages are reused when the runtime info and bindings are also byte-for-byte equal.
///
/// This branch: the GPU params v2 state (draw bank, two-word replacement) is part of the
/// signature, and the GPU params counters translation notes on the environment are recorded and
/// given back on reuse (ReportGpuParamPipeline reads them). There is no indirect storage pointer
/// query here.
///
/// Thread safe: pipelines are built on the GPU thread / MT committer, translation workers, boot
/// workers and background prebuild threads.
class StageCache {
public:
    /// What a reused stage stands in for: the program's metadata, without its IR.
    struct Emitted {
        std::vector<u8> runtime_key; ///< Serialized runtime info and incoming bindings
        std::vector<u32> spirv;
        Shader::IR::Program program; ///< After emission, without IR
        Shader::Backend::Bindings bindings_out;
    };

    struct Translation {
        std::vector<u8> signature;
        struct Read {
            u8 kind;
            u32 a, b, c;
            u64 value;
        };
        std::vector<Read> reads;
        Shader::IR::Program program; ///< After translation, without IR
        /// GPU params v2 counters the translation noted on its environment.
        u32 gpuparam_memory_reads{};
        u32 gpuparam_dynamic_reads{};
        u32 gpuparam_two_word_replaced{};

        mutable std::mutex emitted_mutex;
        mutable std::vector<std::shared_ptr<const Emitted>> emitted;
    };

    enum ReadKind : u8 {
        CbufValue,
        StoragePointer,
        TextureType,
        TexturePixelFormat,
        TextureInteger,
        ViewportTransform,
        Replacement,
    };

    /// Records the reads translation makes through it, forwarding everything to `inner`.
    class Recorder final : public Shader::Environment {
    public:
        explicit Recorder(Shader::Environment& inner_) : inner{inner_} {
            sph = inner.SPH();
            gp_passthrough_mask = inner.GpPassthroughMask();
            mesh_registers = inner.MeshRegisters();
            mesh_primitive_header = inner.MeshPrimitiveHeader();
            stage = inner.ShaderStage();
            start_address = inner.StartAddress();
            is_proprietary_driver = inner.IsProprietaryDriver();
            gpuparam_draw_bank = inner.GpuParamDrawBank();
            gpuparam_two_word = inner.GpuParamTwoWordReplace();
        }

        u64 ReadInstruction(u32 address) override {
            return inner.ReadInstruction(address);
        }
        u32 TranslateProgramAddress(u32 address) const override {
            return inner.TranslateProgramAddress(address);
        }
        bool IsNativeProgram() const override {
            return inner.IsNativeProgram();
        }
        bool IsCbufStorageBufferPointer(u32 index, u32 offset) override {
            return Log(StoragePointer, index, offset, 0,
                       inner.IsCbufStorageBufferPointer(index, offset)) != 0;
        }
        u32 ReadCbufValue(u32 index, u32 offset) override {
            return static_cast<u32>(
                Log(CbufValue, index, offset, 0, inner.ReadCbufValue(index, offset)));
        }
        Shader::TextureType ReadTextureType(u32 handle) override {
            return static_cast<Shader::TextureType>(
                Log(TextureType, handle, 0, 0, static_cast<u64>(inner.ReadTextureType(handle))));
        }
        Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
            return static_cast<Shader::TexturePixelFormat>(Log(
                TexturePixelFormat, handle, 0, 0, static_cast<u64>(inner.ReadTexturePixelFormat(handle))));
        }
        bool IsTexturePixelFormatInteger(u32 handle) override {
            return Log(TextureInteger, handle, 0, 0, inner.IsTexturePixelFormatInteger(handle)) != 0;
        }
        u32 ReadViewportTransformState() override {
            return static_cast<u32>(Log(ViewportTransform, 0, 0, 0, inner.ReadViewportTransformState()));
        }
        u32 TextureBoundBuffer() const override {
            return inner.TextureBoundBuffer();
        }
        u32 LocalMemorySize() const override {
            return inner.LocalMemorySize();
        }
        u32 SharedMemorySize() const override {
            return inner.SharedMemorySize();
        }
        std::array<u32, 3> WorkgroupSize() const override {
            return inner.WorkgroupSize();
        }
        bool HasHLEMacroState() const override {
            return inner.HasHLEMacroState();
        }
        std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override {
            const auto value = inner.GetReplaceConstBuffer(bank, offset);
            Log(Replacement, bank, offset, 0, EncodeReplacement(value));
            return value;
        }
        void Dump(u64 pipeline_hash, u64 shader_hash) override {
            inner.Dump(pipeline_hash, shader_hash);
        }

        std::vector<Translation::Read> TakeReads() {
            return std::move(reads);
        }

        /// Gives the GPU params counters noted during translation to `inner` and `translation`.
        void ForwardGpuParamCounts(Translation& translation) {
            translation.gpuparam_memory_reads = GpuParamMemoryReads();
            translation.gpuparam_dynamic_reads = GpuParamDynamicReads();
            translation.gpuparam_two_word_replaced = GpuParamTwoWordReplaced();
            GiveGpuParamCounts(translation, inner);
        }

    private:
        u64 Log(u8 kind, u32 a, u32 b, u32 c, u64 value) {
            // Only first reads: within one translation a repeated read gives the same answer.
            const std::array<u32, 4> id_words{kind, a, b, c};
            const u64 id = Common::CityHash64(reinterpret_cast<const char*>(id_words.data()),
                                              sizeof(id_words));
            if (logged.insert(id).second) {
                reads.push_back({kind, a, b, c, value});
            }
            return value;
        }

        Shader::Environment& inner;
        std::vector<Translation::Read> reads;
        std::unordered_set<u64> logged;
    };

    StageCache() {
        // EDEN_NX2_STAGE_CACHE=0 translates every stage again; =verify also builds every reused
        // stage and compares the SPIR-V.
        // 'a' alternates (Tegra::Nx2Switch) per pipeline.
        const char* const value = std::getenv("EDEN_NX2_STAGE_CACHE");
        mode = Tegra::Nx2EnvMode("EDEN_NX2_STAGE_CACHE");
        enabled = value == nullptr || value[0] != '0';
        verify = enabled && value != nullptr && std::string_view{value} == "verify";
        if (verify) {
            LOG_INFO(Render_Vulkan,
                     "EDEN_NX2_STAGE_CACHE=verify: reused stages are rebuilt and compared");
        }
    }

    [[nodiscard]] bool Enabled() const {
        return enabled && (mode != 'a' || Tegra::Nx2AltOn());
    }

    /// Whether EDEN_NX2_STAGE_CACHE=verify: every reused stage is also built and compared.
    [[nodiscard]] bool Verify() const noexcept {
        return verify;
    }

    /// Adds a reused translation's GPU params counters to the environment it stands in for.
    static void GiveGpuParamCounts(const Translation& translation, Shader::Environment& env) {
        for (u32 i = 0; i < translation.gpuparam_memory_reads; ++i) {
            env.NoteGpuParamRead(false);
        }
        for (u32 i = 0; i < translation.gpuparam_dynamic_reads; ++i) {
            env.NoteGpuParamRead(true);
        }
        for (u32 i = 0; i < translation.gpuparam_two_word_replaced; ++i) {
            env.NoteGpuParamTwoWordReplaced();
        }
    }

    /// The bytes translation depends on besides the reads: program, slot, environment constants
    /// and the settings the recompiler reads.
    [[nodiscard]] static std::vector<u8> Signature(Shader::Environment& env, u64 unique_hash,
                                                   size_t slot) {
        Writer writer;
        writer.Put(unique_hash);
        writer.Put(static_cast<u32>(slot));
        writer.Put(env.ShaderStage());
        writer.Put(env.StartAddress());
        writer.Put(env.IsProprietaryDriver());
        writer.Put(env.IsNativeProgram());
        writer.Put(env.SPH());
        writer.Put(env.GpPassthroughMask());
        writer.Put(env.MeshRegisters());
        writer.Put(env.MeshPrimitiveHeader());
        writer.Put(env.TextureBoundBuffer());
        writer.Put(env.LocalMemorySize());
        writer.Put(env.SharedMemorySize());
        writer.Put(env.WorkgroupSize());
        writer.Put(env.HasHLEMacroState());
        writer.Put(env.GpuParamDrawBank());
        writer.Put(env.GpuParamTwoWordReplace());
        // What the recompiler reads from the settings, which can change during a session.
        const auto& resolution = Settings::values.resolution_info;
        writer.Put(resolution.up_scale);
        writer.Put(resolution.down_shift);
        writer.Put(resolution.up_factor);
        writer.Put(resolution.down_factor);
        writer.Put(resolution.active);
        writer.Put(Settings::values.renderer_debug.GetValue());
        writer.Put(Settings::values.disable_shader_loop_safety_checks.GetValue());
        writer.Put(Settings::values.fix_bloom_effects.GetValue());
        return std::move(writer.bytes);
    }

    /// A translation of `signature` whose reads `env` answers the same way, or null.
    [[nodiscard]] std::shared_ptr<const Translation> Find(const std::vector<u8>& signature,
                                                          Shader::Environment& env) {
        const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(signature.data()),
                                            signature.size());
        std::vector<std::shared_ptr<const Translation>> candidates;
        {
            std::scoped_lock lock{mutex};
            const auto it = translations.find(hash);
            if (it == translations.end()) {
                return nullptr;
            }
            candidates = it->second;
        }
        for (const auto& candidate : candidates) {
            if (candidate->signature != signature) {
                continue;
            }
            bool same = true;
            try {
                // In the order translation read them: up to the first difference, every read is
                // one translating here would make too.
                for (const Translation::Read& read : candidate->reads) {
                    u64 value{};
                    switch (read.kind) {
                    case CbufValue:
                        value = env.ReadCbufValue(read.a, read.b);
                        break;
                    case StoragePointer:
                        value = env.IsCbufStorageBufferPointer(read.a, read.b);
                        break;
                    case TextureType:
                        value = static_cast<u64>(env.ReadTextureType(read.a));
                        break;
                    case TexturePixelFormat:
                        value = static_cast<u64>(env.ReadTexturePixelFormat(read.a));
                        break;
                    case TextureInteger:
                        value = env.IsTexturePixelFormatInteger(read.a);
                        break;
                    case ViewportTransform:
                        value = env.ReadViewportTransformState();
                        break;
                    case Replacement:
                        value = EncodeReplacement(env.GetReplaceConstBuffer(read.a, read.b));
                        break;
                    default:
                        value = ~read.value;
                        break;
                    }
                    if (value != read.value) {
                        same = false;
                        break;
                    }
                }
            } catch (const Shader::Exception&) {
                // A disk cache environment that never recorded this read.
                same = false;
            }
            if (same) {
                return candidate;
            }
        }
        return nullptr;
    }

    /// The emitted stage of `translation` for `runtime_key`, or null.
    [[nodiscard]] std::shared_ptr<const Emitted> FindEmitted(const Translation& translation,
                                                             const std::vector<u8>& runtime_key) {
        std::scoped_lock lock{translation.emitted_mutex};
        for (const auto& emitted : translation.emitted) {
            if (emitted->runtime_key == runtime_key) {
                return emitted;
            }
        }
        return nullptr;
    }

    /// Makes a translation (from a Recorder) findable, with its first emitted stage.
    void Insert(std::shared_ptr<Translation> translation, std::shared_ptr<const Emitted> emitted) {
        const u64 hash = Common::CityHash64(
            reinterpret_cast<const char*>(translation->signature.data()),
            translation->signature.size());
        const size_t size = translation->reads.size() * sizeof(Translation::Read) +
                            translation->signature.size() + sizeof(Translation) +
                            emitted->spirv.size() * sizeof(u32) + sizeof(Emitted);
        translation->emitted.push_back(std::move(emitted));
        {
            std::scoped_lock lock{mutex};
            translations[hash].push_back(std::move(translation));
        }
        Account(size);
        CountTranslated();
    }

    /// Adds an emitted stage to a translation found earlier.
    void AddEmitted(const Translation& translation, std::shared_ptr<const Emitted> emitted) {
        const size_t size = emitted->spirv.size() * sizeof(u32) + sizeof(Emitted);
        {
            std::scoped_lock lock{translation.emitted_mutex};
            translation.emitted.push_back(std::move(emitted));
        }
        Account(size);
    }

    [[nodiscard]] static std::vector<u8> RuntimeKey(const Shader::RuntimeInfo& info,
                                                    const Shader::Backend::Bindings& bindings) {
        // Every field of RuntimeInfo: one left out would let different SPIR-V share a key.
        Writer writer;
        writer.Put(bindings);
        writer.Put(info.generic_input_types);
        for (size_t index = 0; index < info.previous_stage_stores.mask.size(); index += 64) {
            u64 word{};
            for (size_t bit = 0; bit < 64; ++bit) {
                word |= u64{info.previous_stage_stores.mask[index + bit]} << bit;
            }
            writer.Put(word);
        }
        writer.Put(static_cast<u32>(info.per_primitive_generics.to_ulong()));
        writer.Put(static_cast<u32>(info.previous_stage_legacy_stores_mapping.size()));
        for (const auto& [from, to] : info.previous_stage_legacy_stores_mapping) {
            writer.Put(from);
            writer.Put(to);
        }
        writer.Put(info.convert_depth_mode);
        writer.Put(info.force_early_z);
        writer.Put(info.tess_primitive);
        writer.Put(info.tess_spacing);
        writer.Put(info.tess_clockwise);
        writer.Put(info.input_topology);
        writer.Put(info.fixed_state_point_size.has_value());
        writer.Put(info.fixed_state_point_size.value_or(0.0f));
        writer.Put(info.alpha_test_func.has_value());
        writer.Put(info.alpha_test_func.value_or(Shader::CompareFunction{}));
        writer.Put(info.alpha_test_reference);
        writer.Put(info.y_negate);
        writer.Put(info.glasm_use_storage_buffers);
        writer.Put(info.xfb_count);
        for (u32 index = 0; index < info.xfb_count && index < info.xfb_varyings.size(); ++index) {
            const auto& varying = info.xfb_varyings[index];
            writer.Put(varying.buffer);
            writer.Put(varying.stream);
            writer.Put(varying.stride);
            writer.Put(varying.offset);
            writer.Put(varying.components);
        }
        writer.Put(info.color_output_types);
        writer.Put(info.dual_source_blend);
        return std::move(writer.bytes);
    }

    /// Program metadata without its IR.
    [[nodiscard]] static Shader::IR::Program Summary(const Shader::IR::Program& program) {
        Shader::IR::Program summary;
        summary.info = program.info;
        summary.stage = program.stage;
        summary.workgroup_size = program.workgroup_size;
        summary.output_topology = program.output_topology;
        summary.output_vertices = program.output_vertices;
        summary.invocations = program.invocations;
        summary.local_memory_size = program.local_memory_size;
        summary.shared_memory_size = program.shared_memory_size;
        summary.is_geometry_passthrough = program.is_geometry_passthrough;
        summary.mesh = program.mesh;
        return summary;
    }

    /// Counts for the periodic log line.
    void CountReuse(bool was_emitted) {
        (was_emitted ? reused_emitted : reused_translation).fetch_add(1, std::memory_order_relaxed);
        Log();
    }
    void CountTranslated() {
        translated.fetch_add(1, std::memory_order_relaxed);
        Log();
    }
    void CountVerifyMismatch(u64 unique_hash) {
        verify_mismatches.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR(Render_Vulkan, "Stage cache: reused stage {:016x} differs from a fresh build",
                  unique_hash);
    }

private:
    // Past this, the cache starts over (translations hold program metadata and SPIR-V).
    static constexpr size_t MaxBytes = 256ULL << 20;

    class Writer {
    public:
        template <typename T>
            requires std::is_trivially_copyable_v<T>
        void Put(const T& value) {
            const auto* const begin = reinterpret_cast<const u8*>(&value);
            bytes.insert(bytes.end(), begin, begin + sizeof(T));
        }

        std::vector<u8> bytes;
    };

    static u64 EncodeReplacement(const std::optional<Shader::ReplaceConstant>& value) {
        return value ? (u64{1} << 32) | static_cast<u32>(*value) : 0;
    }

    void Account(size_t added) {
        std::scoped_lock lock{mutex};
        bytes += added;
        if (bytes > MaxBytes) {
            // Translations in use elsewhere stay alive through their shared pointers.
            translations.clear();
            bytes = 0;
        }
    }

    void Log() {
        const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        s64 last = last_log.load(std::memory_order_relaxed);
        if (now - last < 10 || !last_log.compare_exchange_strong(last, now)) {
            return;
        }
        if (last == 0) {
            return;
        }
        LOG_INFO(Render_Vulkan,
                 "Stage cache: {} stages reused whole, {} translations reused, {} translated{}",
                 reused_emitted.load(), reused_translation.load(), translated.load(),
                 verify ? fmt::format(", {} verify mismatches", verify_mismatches.load())
                        : std::string{});
    }

    char mode{'d'};
    bool enabled{};
    bool verify{};

    std::mutex mutex;
    std::unordered_map<u64, std::vector<std::shared_ptr<const Translation>>> translations;
    size_t bytes{};

    std::atomic<u64> reused_emitted{};
    std::atomic<u64> reused_translation{};
    std::atomic<u64> translated{};
    std::atomic<u64> verify_mismatches{};
    std::atomic<s64> last_log{};
};

} // namespace VideoCommon
