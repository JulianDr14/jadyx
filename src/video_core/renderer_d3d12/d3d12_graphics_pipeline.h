// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "common/memory_ledger.h"
#include "common/thread_worker.h"
#include "shader_recompiler/ir_opt/demote_return_pass.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_dxil_disk_cache.h"
#include "video_core/renderer_d3d12/d3d12_pso_cache.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_transition_order.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_stage_bindings.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Tegra {
class MemoryManager;
}

namespace D3D12 {

class GuestDescriptorQueue;
class SamplerHeap;
class Scheduler;

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

/// Vulkan's pipeline key (the fixed state only depends on Maxwell, and with every dynamic feature
/// off it holds everything a PSO needs) plus the state D3D12 bakes into the PSO but Vulkan sets
/// dynamically. The extra fields are zero when their feature is disabled, so they do not split
/// pipelines that would be identical.
struct GraphicsPipelineCacheKey {
    std::array<u64, 6> unique_hashes;
    Vulkan::FixedPipelineState state;
    u32 depth_bias;              ///< f32 bits, Maxwell units (twice D3D12's)
    u32 depth_bias_clamp;        ///< f32 bits
    u32 slope_scaled_depth_bias; ///< f32 bits
    u8 stencil_read_mask;        ///< D3D12 has one pair for both faces: the front one
    u8 stencil_write_mask;
    u8 strip_cut; ///< D3D12_INDEX_BUFFER_STRIP_CUT_VALUE, from the index format
    /// Bit 0 (SHADER_REDUCTION): the float samples apply the sampler table's MIN/MAX reduction
    /// (PipelineCache sets it for the shaders seen with a filtered MIN/MAX sampler the device
    /// cannot create). Bits 1-2 (SHADER_A2C_SHIFT): the fragment shader computes
    /// alpha-to-coverage with this footprint (Shader::RuntimeInfo::alpha_to_coverage_footprint)
    /// and the PSO does not. Bit 3 (SHADER_DEPTH_BIAS): the fragment shader adds the depth bias
    /// (Shader::RuntimeInfo::depth_bias_in_shader) and the PSO has none.
    /// Bits 4-7 (SAMPLE_MASK_SHIFT): samples 0-3 the guest's sample mask turns off at the first
    /// position of its 2x2 footprint, the rest in state.sample_mask_rest. Zero keeps every sample,
    /// so the keys of pipelines already on disk stay valid.
    u8 shader_flags;
    static constexpr u8 SHADER_REDUCTION = 1;
    static constexpr u32 SHADER_A2C_SHIFT = 1;
    static constexpr u8 SHADER_DEPTH_BIAS = 8;
    static constexpr u32 SAMPLE_MASK_SHIFT = 4;

    [[nodiscard]] u8 ShaderAlphaToCoverage() const noexcept {
        return static_cast<u8>((shader_flags >> SHADER_A2C_SHIFT) & 3);
    }

    /// A mask that changes with the position needs the fragment shader: D3D12's has one value.
    [[nodiscard]] bool SampleMaskInShader() const noexcept {
        return state.sample_mask_rest != 0 && unique_hashes[5] != 0;
    }

    [[nodiscard]] UINT SampleMask() const noexcept {
        return SampleMaskInShader() ? UINT_MAX : ~static_cast<UINT>(shader_flags >> SAMPLE_MASK_SHIFT);
    }

    /// Shader::RuntimeInfo::sample_mask_footprint.
    [[nodiscard]] u16 SampleMaskFootprint() const noexcept {
        if (!SampleMaskInShader()) {
            return 0;
        }
        const u32 first = shader_flags >> SAMPLE_MASK_SHIFT;
        const u32 rest = state.sample_mask_rest;
        return static_cast<u16>(first | (first ^ (rest & 0xF)) << 4 |
                                (first ^ ((rest >> 4) & 0xF)) << 8 |
                                (first ^ ((rest >> 8) & 0xF)) << 12);
    }

    size_t Hash() const noexcept;

    bool operator==(const GraphicsPipelineCacheKey& rhs) const noexcept;

    bool operator!=(const GraphicsPipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }

    /// Refreshes the D3D12-only fields from the registers (state must be refreshed first).
    void RefreshStatic(const Maxwell& regs) noexcept;
};
static_assert(std::has_unique_object_representations_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<GraphicsPipelineCacheKey>);

} // namespace D3D12

namespace std {
template <>
struct hash<D3D12::GraphicsPipelineCacheKey> {
    size_t operator()(const D3D12::GraphicsPipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};
} // namespace std

namespace D3D12 {

/// What a pipeline binds its guest resources with (the rasterizer's caches and heaps).
struct PipelineBindContext {
    Tegra::Engines::Maxwell3D& maxwell3d;
    Tegra::MemoryManager& gpu_memory;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    GuestDescriptorQueue& descriptor_queue;
    SamplerHeap& sampler_heap;
    Scheduler& scheduler;
};

/// Root arguments of one draw, filled by GraphicsPipeline::Configure.
struct PipelineBindings {
    struct CapturedImageView {
        VideoCommon::ImageViewId id;
        bool storage{};
        Shader::TextureType declared{};
        bool depth_feedback{};
    };
    D3D12_GPU_DESCRIPTOR_HANDLE resource_table{};
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table{};
    /// Root CBVs (PipelineLayout::RootCbvIndex on), by root slot.
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, PipelineLayout::MAX_ROOT_CBVS> root_cbvs{};
    std::array<u32, PUSH_CONSTANT_WORDS> push_constants{};
    /// Integer sampler states (PipelineLayout::IntegerSamplerIndex), 0 when the layout has none.
    D3D12_GPU_VIRTUAL_ADDRESS integer_samplers{};
    /// When set (draw trace), Configure appends the views of the sampled textures here.
    std::vector<VideoCommon::ImageViewId>* trace_views{};
    /// With trace_views: each texture's sampler (Sampler::Describe), in the same order.
    std::vector<std::string>* trace_filters{};
    /// With trace_views: the Shader::TextureType each texture is declared as.
    std::vector<u32>* trace_types{};
    /// Full forensic capture: every sampled/storage image, in descriptor order.
    std::vector<CapturedImageView>* trace_all_views{};
    /// A texture of the draw is the bound depth buffer: bind it read-only (DEPTH_SAMPLED_STATE).
    bool depth_sampled{};
    /// Stages (bit per Configure stage, VS first) that sampled a float texture with a sampler whose
    /// MIN/MAX reduction is emulated while the pipeline does not emulate it. Draws filter normally.
    u8 unemulated_reduction_stages{};
    /// In: the handle of the textures whose shader takes it per primitive (varying_handle).
    u32 vertex_texture_handle{};
    /// Per axis (u, v), the fragment stage's texture descriptors that MirrorBorderPass mirrors.
    std::array<u32, 2> mirror_border_masks{};
    /// Some are, while the pipeline was built without the pass.
    bool unemulated_mirror_border{};
};

/// Textures whose samples EDEN_SPIRV_TO_DXIL_EMULATE_REDUCTION reduces: float, not cube.
[[nodiscard]] inline bool ReducibleTexture(const Shader::TextureDescriptor& desc) noexcept {
    return !desc.is_integer && desc.type != Shader::TextureType::ColorCube &&
           desc.type != Shader::TextureType::ColorArrayCube &&
           desc.type != Shader::TextureType::Buffer;
}

/// Sampler states of one draw or dispatch, indexed by texture binding: the integer textures', and
/// every texture's when the pipeline emulates MIN/MAX reduction. Rows not added read as zero.
class IntegerSamplerTable {
public:
    void Add(u32 binding, const Sampler& sampler, const ImageView& view) {
        auto state = sampler.IntegerState(static_cast<u32>(std::max(view.range.extent.levels, 1) - 1));
        // Shaders substitute the border for the texels they load through the view themselves.
        std::array<u32, 4> border;
        std::copy(std::begin(state.border_color), std::end(state.border_color), border.begin());
        border = view.SampledBorder(border);
        std::copy(border.begin(), border.end(), state.border_color);
        rows.push_back({binding, state});
        num_bindings = std::max(num_bindings, binding + 1);
    }
    /// Copies the rows to the staging stream; returns their address, or 0 if there are none.
    D3D12_GPU_VIRTUAL_ADDRESS Upload(BufferCache& buffer_cache) const;

    [[nodiscard]] bool Empty() const noexcept {
        return rows.empty();
    }
    [[nodiscard]] bool SameRows(const IntegerSamplerTable& other) const noexcept {
        return num_bindings == other.num_bindings && rows.size() == other.rows.size() &&
               std::equal(rows.begin(), rows.end(), other.rows.begin(),
                          [](const auto& a, const auto& b) {
                              return a.first == b.first &&
                                     std::memcmp(&a.second, &b.second, sizeof(a.second)) == 0;
                          });
    }

private:
    boost::container::small_vector<std::pair<u32, eden_integer_sampler_state>, 8> rows;
    u32 num_bindings{};
};

/// A guest graphics pipeline: signed DXIL of each stage, its root signature and the PSO, built on
/// a worker when one is given. Mirrors Vulkan::GraphicsPipeline (transitions, IsBuilt).
///
/// The PSO, the driver's compiled pipeline and by far its largest part, is resident only while it
/// is drawn (PipelineResidencyPolicy): a dormant pipeline keeps its DXIL, and Wake builds the PSO
/// again from it.
/// Draws reuse the views and samplers a stage resolved in the pipeline's last draw while it reads
/// the same handles from unchanged descriptor table entries (boot.cfg "stage_view_cache=1").
void SetStageViewCache(bool enabled);

/// Draws that bind the same views and samplers as the pipeline's last draw, under the same
/// binding stamp and depth target, take its texture descriptors, prepare flags, reduction bits
/// and integer sampler rows instead of resolving each texture again (boot.cfg
/// "texture_replay=1"; "=check" binds in full and logs every replay that would have differed).
enum class TextureReplayMode : u8 { Off, On, Check };
void SetTextureReplay(TextureReplayMode mode);

/// How a pipeline's color equations share D3D12's single blend constant. Mixing constant color
/// and constant alpha leaves it to the destination factors; the fragment shader applies the source
/// factors that read the other one.
struct BlendConstantPlan {
    bool hardware_alpha{}; ///< The constant holds the guest's alpha in all four channels.
    std::array<Shader::BlendConstantFactor, 8> shader_factors{};

    [[nodiscard]] bool InShader() const noexcept {
        return std::ranges::any_of(shader_factors, [](Shader::BlendConstantFactor factor) {
            return factor != Shader::BlendConstantFactor::None;
        });
    }
};

[[nodiscard]] BlendConstantPlan PlanBlendConstants(const Vulkan::FixedPipelineState& state);

/// What the draws of a pipeline that emulates tessellation (Shader::TessellationEmulation) size
/// and dispatch its passes with. Zero output_vertices: the pipeline does not emulate it.
struct TessEmulationPlan {
    u32 input_vertices{};      ///< Patch control points.
    u32 output_vertices{};     ///< Control shader invocations per patch.
    u32 input_vertex_bytes{};  ///< Stride of a captured vertex.
    u32 record_words{};        ///< Words of a patch record (TessellationEmulation::RecordWords).
    u32 patches_per_group{};   ///< Patches per control shader group.
    u32 domain{};              ///< d3d12_tessellate.comp: isolines 0, triangles 1, quads 2.
    u32 partitioning{};        ///< d3d12_tessellate.comp: odd 0, even 1, integer 2.
    bool ccw{};                ///< Triangles wind counterclockwise.

    [[nodiscard]] bool Active() const noexcept {
        return output_vertices != 0;
    }
};

class GraphicsPipeline {
public:
    enum class State : u8 {
        Building, ///< DXIL or PSO being built on a worker.
        Ready,    ///< PSO built, or the pipeline failed (Handle() null): drawable or skipped.
        Dormant,  ///< No PSO, DXIL kept or on disk: Wake builds it.
    };

    static constexpr size_t NUM_STAGES = Maxwell::MaxShaderStage;
    using DxilStages = std::array<std::vector<u8>, NUM_STAGES>;
    /// The linked stages, and the stream output declaration of the last pre-rasterization stage
    /// when the pipeline captures transform feedback natively (empty otherwise).
    struct LinkedDxil {
        DxilStages stages;
        std::vector<D3D12_SO_DECLARATION_ENTRY> stream_output;
        /// The disk cache record the stages can be read back from, if any.
        std::optional<DxilDiskCache::Key> disk_key;
        /// Emulated tessellation (TessEmulationPlan): the vertex shader alone, storing to the
        /// capture, and the control shader as compute. stages then holds the
        /// evaluation shader as the vertex stage, and no hull or domain stage.
        std::vector<u8> tess_capture;
        std::vector<u8> tess_control;
    };
    using SharedDxilStages = std::shared_ptr<const LinkedDxil>;
    /// (trace id, flatten the vertex stage's branches) -> linked DXIL.
    using CompileDxil = std::function<SharedDxilStages(u64, bool)>;

    /// compile_dxil (SPIR-V to DXIL, the costly part of a new pipeline) runs with the PSO build,
    /// on the worker when one is given; it throws std::exception when the pipeline cannot be made.
    GraphicsPipeline(const Device& device, const TextureCacheRuntime& texture_runtime,
                     VideoCore::ShaderNotify* shader_notify, Common::ThreadWorker* worker_thread,
                     const GraphicsPipelineCacheKey& key, CompileDxil compile_dxil,
                     const std::array<const Shader::Info*, NUM_STAGES>& infos,
                     const PipelineLayout& layout, bool build_pso = true,
                     GraphicsPsoCache* pso_cache = nullptr, u32 lower_noperspective_mask = 0,
                     const TessEmulationPlan& tess_plan = {});
    ~GraphicsPipeline();

    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    void AddTransition(GraphicsPipeline* transition);

    [[nodiscard]] GraphicsPipeline* Next(const GraphicsPipelineCacheKey& current_key) noexcept {
        if (key == current_key) {
            return this;
        }
        const size_t index = FindPipelineTransition<GraphicsPipelineCacheKey>(
            transition_keys, transition_order, current_key);
        if (index < transitions.size()) return transitions[index];
        return nullptr;
    }

    [[nodiscard]] bool IsBuilt() const noexcept {
        return build_state.load(std::memory_order::acquire) == State::Ready;
    }

    [[nodiscard]] bool IsDormant() const noexcept {
        return build_state.load(std::memory_order::acquire) == State::Dormant;
    }

    /// GPU thread: the frame before which a pipeline whose PSO ran out of memory is not woken
    /// again; its draws are skipped until then.
    [[nodiscard]] u64 RetryFrame() const noexcept {
        return retry_frame;
    }
    void SetRetryFrame(u64 frame) noexcept {
        retry_frame = frame;
    }
    /// GPU thread, while dormant: whether the last build ran out of memory, cleared on read.
    [[nodiscard]] bool TakeOutOfMemory() noexcept {
        return std::exchange(out_of_memory, false);
    }

    /// Blocks until the PSO is built. A build still in its worker's queue is claimed and run on
    /// the calling thread instead: the GPU thread runs at a higher priority than the workers and
    /// would otherwise wait for the queue ahead of it too. A dormant pipeline must be woken first;
    /// one whose PSO ran out of memory is dormant again when this returns.
    void WaitBuilt();
    /// Waits for the worker up to this long and never takes the build over: a driver build cannot
    /// be cut short once it runs here. True when it is no longer building.
    [[nodiscard]] bool WaitBuiltFor(u64 microseconds);

    /// GPU thread: a dormant pipeline queues the build of its PSO from the kept DXIL. False if it
    /// was not dormant.
    bool Wake(Common::ThreadWorker& worker);

    /// True for the disk cache's pipelines, made at boot; false for those first drawn this session.
    [[nodiscard]] bool FromDisk() const noexcept {
        return from_disk;
    }

    /// GPU thread: whether its PSO was released while idle at some point this session.
    [[nodiscard]] bool WasEvicted() const noexcept {
        return evicted;
    }

    /// GPU thread: its PSO was built ahead of any draw, by the background prewarm.
    void MarkPrewarmed() noexcept {
        prewarmed = true;
    }
    [[nodiscard]] bool Prewarmed() const noexcept {
        return prewarmed;
    }

    /// GPU thread: releases the PSO of a ready pipeline once the GPU is done with it, and its DXIL
    /// when Wake can read it back from disk. Pipelines that failed have nothing to release. True
    /// if released.
    bool Evict(Scheduler& scheduler);

    /// Dormant pipelines' DXIL dropped and read back from disk so far, and read-backs that failed.
    struct DxilReleaseStats {
        u64 released;
        u64 released_bytes;
        u64 reloaded;
        u64 failed;
    };
    [[nodiscard]] static DxilReleaseStats GetDxilReleaseStats() noexcept;

    /// GPU thread: the frame this pipeline was last drawn in.
    void Touch(u64 frame) noexcept {
        last_used_frame = frame;
    }
    [[nodiscard]] u64 LastUsedFrame() const noexcept {
        return last_used_frame;
    }

    /// GPU thread: true only the first time, so a session records each pipeline it draws once.
    [[nodiscard]] bool MarkDrawn() noexcept {
        return !std::exchange(drawn, true);
    }

    /// Key().Hash(), computed once: it identifies the pipeline across sessions.
    [[nodiscard]] u64 KeyHash() const noexcept {
        return key_hash;
    }

    /// Null when D3D12 rejected the pipeline (logged once when it was built).
    [[nodiscard]] ID3D12PipelineState* Handle() const noexcept {
        return pipeline_state.Get();
    }

    [[nodiscard]] const PipelineLayout& Layout() const noexcept {
        return layout;
    }

    [[nodiscard]] const TessEmulationPlan& TessPlan() const noexcept {
        return tess_plan;
    }
    /// With TessPlan().Active(): the capture PSO (vertex shader and stream output) and the control
    /// compute PSO, both built with Handle(), both null when it is.
    [[nodiscard]] ID3D12PipelineState* TessCaptureHandle() const noexcept {
        return tess_capture_state.Get();
    }
    [[nodiscard]] ID3D12PipelineState* TessControlHandle() const noexcept {
        return tess_control_state.Get();
    }
    /// Words per lane of the local memory pool the fragment stage uses, or 0 without it.
    [[nodiscard]] u32 LocalMemoryScratchWords() const noexcept {
        return local_memory_scratch_words;
    }
    [[nodiscard]] std::span<const u8> DiagnosticDxil(size_t stage) const noexcept {
        return dxil && stage < dxil->stages.size() ? std::span<const u8>{dxil->stages[stage]}
                                                   : std::span<const u8>{};
    }
    [[nodiscard]] std::span<const D3D12_SO_DECLARATION_ENTRY> DiagnosticStreamOutput()
        const noexcept {
        return dxil ? std::span<const D3D12_SO_DECLARATION_ENTRY>{dxil->stream_output}
                    : std::span<const D3D12_SO_DECLARATION_ENTRY>{};
    }

    [[nodiscard]] const StageBindings& StageInfo(size_t stage) const noexcept {
        static const StageBindings empty{};
        return stage_infos[stage] ? *stage_infos[stage] : empty;
    }

    [[nodiscard]] bool HasStage(size_t stage) const noexcept {
        return has_stage[stage];
    }

    [[nodiscard]] const GraphicsPipelineCacheKey& Key() const noexcept {
        return key;
    }

    [[nodiscard]] u8 BoundColorTargets() const noexcept {
        return bound_color_targets;
    }

    /// The PSO runs the depth bounds test; the draw sets the bounds (OMSetDepthBounds).
    [[nodiscard]] bool DepthBounds() const noexcept {
        return depth_bounds;
    }

    /// The D3D12 blend factor serves constant-alpha factors, so the draw sets it to the guest's
    /// alpha in all four channels.
    [[nodiscard]] bool BlendConstantAlpha() const noexcept {
        return blend_constants.hardware_alpha;
    }

    /// The fragment shader reads the blend constant, so the draw sets it in the root constants.
    [[nodiscard]] bool ShaderBlendConstants() const noexcept {
        return blend_constants.InShader();
    }

    /// This pipeline culling one face instead of as the guest asked, built the first time a draw
    /// needs it (draws whose faces use different stencil state). The one culling front faces
    /// draws the back ones, with the back-face stencil masks given (D3D12 has one pair for both
    /// faces and the key holds the front one). Null if it cannot be built.
    [[nodiscard]] ID3D12PipelineState* CulledHandle(D3D12_CULL_MODE mode, u8 back_read_mask = 0,
                                                    u8 back_write_mask = 0);

    [[nodiscard]] u32 LoweredNoPerspectiveMask() const noexcept {
        return lower_noperspective_mask;
    }

    /// The vertex attribute a fragment shader's texture handle comes from, passed through flat by
    /// the vertex shader: each run of primitives with one handle is a draw of its own.
    struct VertexTextureHandle {
        u32 attribute{};
        u32 component{};
    };

    void SetVertexTextureHandle(const std::optional<VertexTextureHandle>& source) noexcept {
        vertex_texture_handle = source;
    }

    [[nodiscard]] const std::optional<VertexTextureHandle>& GetVertexTextureHandle() const noexcept {
        return vertex_texture_handle;
    }

    /// How the fragment shader keeps the helper lanes of its derivatives, for draw traces.
    struct FragmentHelpers {
        Shader::Optimization::DemoteReturnVerdict demote_return{};
        bool quad_shuffles{};
        bool derivatives{};
    };

    void SetFragmentHelpers(const FragmentHelpers& helpers) noexcept {
        fragment_helpers = helpers;
    }

    /// Whether the pipeline draws through a geometry shader generated for state D3D12 lacks.
    void SetGeneratesGeometry(bool value) noexcept {
        generates_geometry = value;
    }

    [[nodiscard]] bool GeneratesGeometry() const noexcept {
        return generates_geometry;
    }

    [[nodiscard]] const FragmentHelpers& GetFragmentHelpers() const noexcept {
        return fragment_helpers;
    }

    /// Opt-in isolated float output probe. Preserves linked VS, input layout and rasterization.
    ComPtr<ID3D12PipelineState> CreateMaterialProbePipeline(
        std::span<const u8> pixel_shader, std::span<const u8> geometry_shader = {},
        bool reflection_probe = false,
        DXGI_FORMAT output_format = DXGI_FORMAT_R32G32B32A32_FLOAT) const;

    /// Private capture controls for the exact picking interface; never used for guest draws.
    ComPtr<ID3D12PipelineState> CreatePickProbePipeline(std::span<const u8> ps,
                                                       bool float_outputs) const;

    /// Port of Vulkan's ConfigureImpl: synchronizes and binds the guest's buffers and textures,
    /// writes the descriptor tables and transitions sampled and storage images. The render
    /// targets are updated last (UpdateRenderTargets) but not transitioned. Records no
    /// command-list state, so a flush in the middle (descriptor heaps full) loses nothing.
    void Configure(bool is_indexed, const PipelineBindContext& context, PipelineBindings& out);

private:
    /// flatten_on_fault: on a driver fault, leave the failure to the caller (flatten_retry) instead
    /// of reporting it, since it can retranslate the shaders.
    void Build(bool flatten_on_fault = false);
    /// Creates the capture and control pipelines of an emulated tessellation; false if refused.
    bool BuildTessEmulation(std::span<const D3D12_INPUT_ELEMENT_DESC> elements);
    /// Drops dxil when its disk cache record can stand in for it (no stream output).
    void ReleaseDxil();
    /// Reads back the DXIL ReleaseDxil dropped. False if it could not.
    bool ReloadDxil();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Describe(std::vector<D3D12_INPUT_ELEMENT_DESC>& elements) const;

    void Publish(State next);

    /// A build queued on a worker, which whoever claims it first runs: the worker, or a thread
    /// that cannot go on without it (WaitBuilt).
    struct PendingBuild {
        std::atomic_bool claimed{};
        u64 queued_us{};
        Common::UniqueFunction<void> run;
    };
    /// Queues job on worker, claimable by WaitBuilt; without a worker, runs it here.
    void QueueBuild(Common::ThreadWorker* worker, Common::UniqueFunction<void> job);
    /// Runs a claimed build, recording how long it sat in the queue.
    void RunClaimed(PendingBuild& build);

    const Device& device;
    const TextureCacheRuntime& texture_runtime;
    const GraphicsPipelineCacheKey key;
    const u64 key_hash;
    const PipelineLayout& layout;
    const u32 lower_noperspective_mask;
    const TessEmulationPlan tess_plan;
    FragmentHelpers fragment_helpers{};
    std::optional<VertexTextureHandle> vertex_texture_handle;
    bool generates_geometry{};
    SharedDxilStages dxil; ///< Immutable linked bytecode shared across fixed-state PSOs.
    std::array<bool, NUM_STAGES> has_stage{};
    std::array<std::shared_ptr<const StageBindings>, NUM_STAGES> stage_infos;
    std::array<u32, NUM_STAGES> enabled_uniform_buffer_masks{};
    VideoCommon::UniformBufferSizes uniform_buffer_sizes{};
    /// Each constant buffer the draw binds, in binding order: root CBV slot and shader reach.
    std::vector<ConstantBufferSlot> cbv_slots;
    bool has_images{};
    u32 local_memory_scratch_words{};
    bool flatten_retry{};    ///< Build(true) hit a driver fault: retry with flattened branches.
    bool out_of_memory{};    ///< The last Build() ran out of memory: published dormant.
    bool flattened_vertex{}; ///< dxil holds the flattened vertex stage.
    u8 bound_color_targets{}; ///< Computed once from the immutable PSO write masks.
    bool depth_bounds{};
    BlendConstantPlan blend_constants{};
    /// Guest transform feedback buffer strides, for the stream output declaration.
    std::array<UINT, Maxwell::NumTransformFeedbackBuffers> stream_output_strides{};

    std::vector<GraphicsPipelineCacheKey> transition_keys;
    std::vector<GraphicsPipeline*> transitions;
    std::vector<u32> transition_order;

    ComPtr<ID3D12PipelineState> pipeline_state;
    ComPtr<ID3D12PipelineState> cull_back_state;
    /// Back faces only, one per pair of back-face stencil masks (read | write << 8).
    std::vector<std::pair<u16, ComPtr<ID3D12PipelineState>>> cull_front_states;
    ComPtr<ID3D12PipelineState> tess_capture_state;
    ComPtr<ID3D12PipelineState> tess_control_state;
    GraphicsPsoCache* const pso_cache;
    SharedGraphicsPso shared_pso;

    std::mutex build_mutex;
    std::condition_variable build_condvar;
    std::atomic<State> build_state{State::Building};
    std::shared_ptr<PendingBuild> pending; ///< build_mutex: the build in a queue, if any.

    const bool from_disk;
    u64 last_used_frame{}; ///< GPU thread only.
    bool drawn{};          ///< GPU thread only.
    bool evicted{};        ///< GPU thread only.
    u64 retry_frame{};     ///< GPU thread only.
    /// Where the DXIL dropped by ReleaseDxil is read back from, and which stages it holds.
    std::optional<DxilDiskCache::Key> released_dxil;
    u8 released_stage_mask{};
    bool prewarmed{};      ///< GPU thread only.
    /// What each stage resolved its views and samplers to in the last draw (stage_view_cache=1),
    /// allocated by the first draw that uses it. GPU thread only.
    struct StageViewState;
    std::unique_ptr<StageViewState> stage_views;
    /// What the last draw bound per texture (texture_replay), allocated by the first draw that
    /// uses it. GPU thread only.
    struct TextureReplayState;
    std::unique_ptr<TextureReplayState> texture_replay;
    /// The object itself. Its DXIL and stage bindings are shared with other pipelines.
    Common::MemoryCharge charge{Common::MemoryAccount::Pipelines, sizeof(GraphicsPipeline)};
};

/// pso_probe=1 (boot.cfg): measure the driver's commit for each graphics PSO built.
void SetPsoCostProbe(bool enabled);
/// The measured cost so far, empty without samples.
std::string DescribePsoCost();

} // namespace D3D12
