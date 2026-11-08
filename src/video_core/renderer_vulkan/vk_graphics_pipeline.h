// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <type_traits>
#include <vector>

#include "common/thread_worker.h"
#include "video_core/mt/engine_ptr.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

struct GraphicsPipelineCacheKey {
    std::array<u64, 6> unique_hashes;
    FixedPipelineState state;

    size_t Hash() const noexcept;

    bool operator==(const GraphicsPipelineCacheKey& rhs) const noexcept;

    bool operator!=(const GraphicsPipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }

    size_t Size() const noexcept {
        return sizeof(unique_hashes) + state.Size();
    }
};
static_assert(std::has_unique_object_representations_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<GraphicsPipelineCacheKey>);

} // namespace Vulkan

namespace std {
template <>
struct hash<Vulkan::GraphicsPipelineCacheKey> {
    size_t operator()(const Vulkan::GraphicsPipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};
} // namespace std

namespace Vulkan {

class Device;
class PipelineStatistics;
class RenderPassCache;
class RescalingPushConstant;
class RenderAreaPushConstant;
class Scheduler;

class GraphicsPipeline {
    static constexpr size_t NUM_STAGES = Tegra::Engines::Maxwell3D::Regs::MaxShaderStage;

public:
    explicit GraphicsPipeline(
        Scheduler& scheduler, BufferCache& buffer_cache, TextureCache& texture_cache,
        vk::PipelineCache& pipeline_cache, VideoCore::ShaderNotify* shader_notify,
        const Device& device, DescriptorPool& descriptor_pool,
        GuestDescriptorQueue& guest_descriptor_queue,
        DescriptorBufferRing& descriptor_buffer_ring, Common::ThreadWorker* worker_thread,
        PipelineStatistics* pipeline_statistics, RenderPassCache& render_pass_cache,
        const GraphicsPipelineCacheKey& key, std::array<vk::ShaderModule, NUM_STAGES> stages,
        const std::array<const Shader::Info*, NUM_STAGES>& infos);

    bool HasDynamicVertexInput() const noexcept { return key.state.dynamic_vertex_input; }
    bool SupportsAlphaToCoverage() const noexcept {
        return fragment_has_color0_output;
    }

    bool SupportsAlphaToOne() const noexcept {
        return fragment_has_color0_output;
    }

    bool UsesExtendedDynamicState() const noexcept {
        return key.state.extended_dynamic_state != 0;
    }

    /// Switch 2 task/mesh pipeline: drawn with vkCmdDrawMeshTasksEXT.
    /// Immutable after construction (read by the multithreaded back's sequencer at capture).
    [[nodiscard]] const std::array<Shader::Info, NUM_STAGES>& StageInfos() const noexcept {
        return stage_infos;
    }
    [[nodiscard]] const std::array<u32, 5>& EnabledUniformBufferMasks() const noexcept {
        return enabled_uniform_buffer_masks;
    }
    [[nodiscard]] const VideoCommon::UniformBufferSizes& UniformSizes() const noexcept {
        return uniform_buffer_sizes;
    }

    [[nodiscard]] bool IsMeshPipeline() const noexcept {
        return stage_infos[0].is_task || stage_infos[0].is_mesh;
    }
    GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept = delete;
    GraphicsPipeline(GraphicsPipeline&&) noexcept = delete;

    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(const GraphicsPipeline&) = delete;

    void AddTransition(GraphicsPipeline* transition);

    bool Configure(bool is_indexed) {
        return configure_func(this, is_indexed);
    }

    [[nodiscard]] GraphicsPipeline* Next(const GraphicsPipelineCacheKey& current_key) noexcept {
        if (key == current_key) {
            return this;
        }
        const auto it{std::find(transition_keys.begin(), transition_keys.end(), current_key)};
        return it != transition_keys.end() ? transitions[std::distance(transition_keys.begin(), it)]
                                           : nullptr;
    }

    /// The frame the pipeline cache last handed this pipeline out (evicts long unused ones).
    void MarkUsed(u64 frame) noexcept {
        last_use_frame.store(frame, std::memory_order_relaxed);
    }

    [[nodiscard]] u64 LastUse() const noexcept {
        return last_use_frame.load(std::memory_order_relaxed);
    }

    /// Forgets the transitions, which may point to pipelines the cache evicted.
    void ClearTransitions() noexcept {
        transition_keys.clear();
        transitions.clear();
    }

    [[nodiscard]] bool IsBuilt() const noexcept {
        return is_built.load(std::memory_order::relaxed);
    }

    template <typename Spec>
    static auto MakeConfigureSpecFunc() {
        return [](GraphicsPipeline* pl, bool is_indexed) { return pl->ConfigureImpl<Spec>(is_indexed); };
    }

    void SetEngine(Tegra::Engines::Maxwell3D* maxwell3d_, Tegra::MemoryManager* gpu_memory_) {
        maxwell3d = maxwell3d_;
        gpu_memory = gpu_memory_;
    }

    /// The Spec ConfigureImpl was instantiated with (which stages and descriptor kinds it
    /// visits).
    struct SpecFlags {
        std::array<bool, 5> enabled_stages;
        bool has_storage_buffers;
        bool has_texture_buffers;
        bool has_image_buffers;
        bool has_images;
    };

    /// What a resolver thread of the multithreaded back reads from the pipeline (immutable
    /// after construction; docs/mt-phase5-spec.md §7.3).
    struct ResolveInfo {
        const std::array<Shader::Info, NUM_STAGES>* stage_infos;
        std::array<u32, 5> enabled_uniform_buffer_masks;
        const VideoCommon::UniformBufferSizes* uniform_buffer_sizes;
        u32 num_descriptor_entries;
        u32 num_image_elements;
        u32 num_textures;
        bool uses_descriptor_buffer;
        bool uses_push_descriptor;
        bool has_descriptor_set_layout;
        SpecFlags spec;
        const DescriptorBufferLayout* descriptor_buffer_layout;
        bool is_mesh;
        bool has_texture_buffers{}; ///< any texture or image buffer descriptor
        bool uses_render_area{};
    };

    [[nodiscard]] const ResolveInfo& GetResolveInfo() const noexcept {
        return resolve_info;
    }

    /// The real engine the pipeline was last configured against (MT_RCHECK).
    [[nodiscard]] Tegra::Engines::Maxwell3D* StoredEngine() const noexcept {
        return maxwell3d.Real();
    }

    /// ConfigureDraw from a payload built elsewhere (a resolver's arena): the descriptor-buffer
    /// write and reuse memo, then the shared tail.
    bool ConfigureDrawPayload(const DescriptorUpdateEntry* payload,
                              const RescalingPushConstant& rescaling,
                              const RenderAreaPushConstant& render_area);

    /// ConfigureDraw's tail: render pass, build wait, pipeline/descriptor binds and the record.
    /// `descriptor_buffer_offset/chunk` were allocated and written by the caller.
    bool ConfigureDrawCommon(
        const DescriptorUpdateEntry* payload, VkDeviceSize descriptor_buffer_offset,
        u32 descriptor_buffer_chunk,
        const std::array<u32, Shader::Backend::SPIRV::NUM_TEXTURE_AND_IMAGE_SCALING_WORDS>&
            rescaling_words,
        const RenderAreaPushConstant& render_area);

    [[nodiscard]] bool HasPipeline() const noexcept {
        return static_cast<bool>(pipeline);
    }

private:
    /// Builds the pipeline once, on whichever thread gets here first.
    void RunBuild();

    template <typename Spec>
    bool ConfigureImpl(bool is_indexed);

    bool ConfigureDraw(const RescalingPushConstant& rescaling,
                       const RenderAreaPushConstant& render_are);

    void MakePipeline(VkRenderPass render_pass);

    void Validate();

    const GraphicsPipelineCacheKey key;
    VideoCommon::MT::ThreadEnginePtr<Tegra::Engines::Maxwell3D> maxwell3d;
    Tegra::MemoryManager* gpu_memory;
    const Device& device;
    TextureCache& texture_cache;
    BufferCache& buffer_cache;
    vk::PipelineCache& pipeline_cache;
    Scheduler& scheduler;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;

    bool (*configure_func)(GraphicsPipeline*, bool){};

    std::vector<GraphicsPipelineCacheKey> transition_keys;
    std::vector<GraphicsPipeline*> transitions;
    std::atomic<u64> last_use_frame{};

    std::array<vk::ShaderModule, NUM_STAGES> spv_modules;

    std::array<Shader::Info, NUM_STAGES> stage_infos;
    std::array<u32, 5> enabled_uniform_buffer_masks{};
    VideoCommon::UniformBufferSizes uniform_buffer_sizes{};
    u32 num_descriptor_entries{};
    size_t num_image_elements{};
    u64 memo_last_handles{}; ///< EDEN_NX2_MEMO_STATS
    ResolveInfo resolve_info{};

    /// EDEN_NX2_TEX_MEMO: the texture bindings of this pipeline's last draw that took the full
    /// path. A draw reuses them when the channel, the texture cache's view epoch and bindings
    /// serial match, it read the same handles, and TryReuseGraphicsBindings confirms every
    /// TIC/TSC entry is unchanged in guest memory and each image needs no preparation. The
    /// descriptor entries and rescaling bits are a pure function of the view and sampler ids
    /// (views are immutable until erased, which advances the epoch; rescaling erases them).
    struct TextureMemo {
        bool valid{};
        const void* channel{};
        u64 view_epoch{};
        u64 bindings_serial{};
        u64 touch_tick{~0ULL}; ///< Frame its images were last touched in the LRU
        std::vector<u32> view_handles;    ///< TIC handle per image element, in views order
        std::vector<u32> sampler_handles; ///< TSC handle per texture element
        std::vector<VideoCommon::ImageViewId> view_ids;
        std::vector<VideoCommon::SamplerId> sampler_ids;
        std::vector<VideoCommon::ImageId> written_images; ///< PushImageDescriptors marks these
        std::vector<DescriptorUpdateEntry> entries; ///< Image entries of all stages, in order
        std::array<u32, NUM_STAGES> stage_begin{}; ///< First of the stage's entries
        std::array<u32, NUM_STAGES> stage_count{}; ///< How many entries the stage has
        std::array<u32, Shader::Backend::SPIRV::NUM_TEXTURE_AND_IMAGE_SCALING_WORDS> rescaling{};
    };
    TextureMemo tex_memo;
    u32 num_textures{};
    bool fragment_has_color0_output{};

    vk::DescriptorSetLayout descriptor_set_layout;
    DescriptorAllocator descriptor_allocator;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorUpdateTemplate descriptor_update_template;
    vk::Pipeline pipeline;

    DescriptorBufferLayout descriptor_buffer_layout;
    std::vector<DescriptorUpdateEntry> last_descriptor_payload;
    VkDeviceSize last_descriptor_buffer_offset{};
    u32 last_descriptor_buffer_chunk{};
    u64 last_descriptor_buffer_generation{};

    std::function<void()> build_task;
    std::atomic_bool build_claimed{false};
    std::condition_variable build_condvar;
    std::mutex build_mutex;
    std::atomic_bool is_built{false};
    bool uses_push_descriptor{false};
    bool uses_descriptor_buffer{false};
};

} // namespace Vulkan
