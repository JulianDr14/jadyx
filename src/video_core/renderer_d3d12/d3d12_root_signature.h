// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

/// Root constants for Vulkan push constants (space 30). spirv_to_dxil sizes the CBV to the highest
/// offset read, 16-byte aligned: RescalingLayout needs 8 dwords, RenderAreaLayout 4.
constexpr u32 PUSH_CONSTANT_WORDS = 8;

/// ExecuteIndirect records (see IndirectArgumentRing). A draw record starts with the first five
/// words of the vertex runtime data (first_vertex, base_instance, is_indexed_draw, yz_flip_mask,
/// draw_id), set as root constants, followed by the D3D12 draw arguments; a dispatch record with
/// the group counts, then the dispatch arguments (the same counts).
constexpr u32 INDIRECT_DRAW_CONSTANT_WORDS = 5;
constexpr u32 INDIRECT_DRAW_WORDS = INDIRECT_DRAW_CONSTANT_WORDS + 4;
constexpr u32 INDIRECT_DRAW_INDEXED_WORDS = INDIRECT_DRAW_CONSTANT_WORDS + 5;
constexpr u32 INDIRECT_DISPATCH_CONSTANT_WORDS = 3;
constexpr u32 INDIRECT_DISPATCH_WORDS = INDIRECT_DISPATCH_CONSTANT_WORDS + 3;

/// Root signature of a guest pipeline.
///
/// Layout (root parameters, in this order; the tables only exist when non-empty):
///  - root constants, push constants: b0 space 30
///  - root constants, spirv_to_dxil runtime data (first vertex, y-flip mask, group count...): b0
///    space 31
///  - CBV_SRV_UAV descriptor table
///  - sampler descriptor table
///  - root CBV, integer sampler states (eden_integer_sampler_state per texture binding): b0
///    space 29, only when a texture is integer. D3D12 cannot Sample() integer resources, so
///    spirv_to_dxil turns those samples into texel loads and emulates the sampler with this data
///  - root UAV, local memory pool (Shader::LocalMemoryScratch): u0 space 28, pixel stage only,
///    when the fragment shader keeps its local memory there
///  - root constants and root UAVs, transform feedback (Shader::TransformFeedbackStorage): b4 and
///    u<N> space 27, vertex stage only, when the vertex shader writes transform feedback buffers
///  - graphics only: root CBVs for the first constant buffers that are not arrays, up to
///    MAX_ROOT_CBVS and what the 64-dword root budget leaves. Setting a root address per draw
///    replaces creating a CBV in the table; the shaders still read b<B> space 0
///
/// Descriptor tables follow the binding stream of the SPIR-V backend with
/// unified_descriptor_binding: one counter over the pipeline's stages in order and, inside each
/// stage, constant buffers, storage buffers, texture buffers, image buffers, textures, images.
/// spirv_to_dxil keeps set 0 as space 0 and uses the binding as the register of each class, so a
/// descriptor at binding B lands on b<B>, t<B> or u<B> (a combined sampler also on s<B>).
///
/// Table slots, in that order, per guest descriptor (count = descriptor array length):
///  - constant buffer: count CBVs, none when it is a root CBV
///  - storage buffer, image buffer, image: count UAVs, then count SRVs. spirv_to_dxil turns the
///    ones it proves read-only into SRVs (declared_read_only_images_as_srvs, NonWritable), so both
///    are declared on the same register and both get written
///  - texture buffer: 1 SRV
///  - texture: count SRVs, plus count samplers in the sampler table
class PipelineLayout {
public:
    ID3D12RootSignature* Handle() const noexcept {
        return root_signature.Get();
    }
    u32 RuntimeDataWords() const noexcept {
        return runtime_data_words;
    }
    /// Root parameter index of each table, or NO_TABLE.
    u32 ResourceTableIndex() const noexcept {
        return resource_table_index;
    }
    u32 SamplerTableIndex() const noexcept {
        return sampler_table_index;
    }
    u32 IntegerSamplerIndex() const noexcept {
        return integer_sampler_index;
    }
    u32 LocalMemoryScratchIndex() const noexcept {
        return local_memory_scratch_index;
    }
    /// Root parameter index of the transform feedback constants, or NO_TABLE.
    u32 TransformFeedbackConstantsIndex() const noexcept {
        return xfb_constants_index;
    }
    /// Root parameter index of each transform feedback buffer's UAV, or NO_TABLE.
    const std::array<u32, 4>& TransformFeedbackBufferIndices() const noexcept {
        return xfb_buffer_indices;
    }
    /// The pipelines of this layout capture transform feedback through stream output.
    bool StreamOutput() const noexcept {
        return stream_output;
    }
    u32 NumResourceDescriptors() const noexcept {
        return num_resources;
    }
    u32 NumSamplerDescriptors() const noexcept {
        return num_samplers;
    }
    /// Root parameter index of the first root CBV; the others follow it.
    u32 RootCbvIndex() const noexcept {
        return root_cbv_index;
    }
    u32 NumRootCbvs() const noexcept {
        return static_cast<u32>(root_cbv_registers.size());
    }
    /// Root CBV slot of each constant buffer of the binding stream, in order, or NO_ROOT_CBV for
    /// the ones in the table.
    std::span<const u32> CbvRootSlots() const noexcept {
        return cbv_root_slots;
    }
    /// Command signatures for ExecuteIndirect with the records above; null if D3D12 refused them
    /// (logged). Graphics layouts have the draw ones, compute layouts the dispatch one.
    ID3D12CommandSignature* DrawSignature(bool indexed) const noexcept {
        return indexed ? draw_indexed_signature.Get() : draw_signature.Get();
    }
    ID3D12CommandSignature* DispatchSignature() const noexcept {
        return dispatch_signature.Get();
    }

    static constexpr u32 PUSH_CONSTANTS_INDEX = 0;
    static constexpr u32 RUNTIME_DATA_INDEX = 1;
    static constexpr u32 NO_TABLE = ~0u;
    static constexpr u32 NO_ROOT_CBV = ConstantBufferSlot::TABLE;
    static constexpr u32 MAX_ROOT_CBVS = D3D12::MAX_ROOT_CBVS;

private:
    friend class RootSignatureCache;

    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12CommandSignature> draw_signature;
    ComPtr<ID3D12CommandSignature> draw_indexed_signature;
    ComPtr<ID3D12CommandSignature> dispatch_signature;
    u32 runtime_data_words{};
    u32 resource_table_index{NO_TABLE};
    u32 sampler_table_index{NO_TABLE};
    u32 integer_sampler_index{NO_TABLE};
    u32 local_memory_scratch_index{NO_TABLE};
    u32 xfb_constants_index{NO_TABLE};
    std::array<u32, 4> xfb_buffer_indices{NO_TABLE, NO_TABLE, NO_TABLE, NO_TABLE};
    u32 num_resources{};
    u32 num_samplers{};
    u32 root_cbv_index{NO_TABLE};
    bool stream_output{};
    std::vector<u32> root_cbv_registers;
    std::vector<u32> cbv_root_slots;
};

/// Binding of a stage's first texture, given the stage's first binding. Texture array elements
/// follow it consecutively, in descriptor order. Info is a Shader::Info or a StageBindings.
template <typename Info>
[[nodiscard]] u32 FirstTextureBinding(const Info& info, u32 stage_binding) {
    return stage_binding + Shader::NumDescriptors(info.constant_buffer_descriptors) +
           Shader::NumDescriptors(info.storage_buffers_descriptors) +
           static_cast<u32>(info.texture_buffer_descriptors.size()) +
           static_cast<u32>(info.image_buffer_descriptors.size());
}

/// Bindings one stage takes in the stream above.
template <typename Info>
[[nodiscard]] u32 NumStageBindings(const Info& info) {
    return FirstTextureBinding(info, 0) + Shader::NumDescriptors(info.texture_descriptors) +
           static_cast<u32>(info.image_descriptors.size());
}

/// Builds and deduplicates guest root signatures. Thread-safe: pipelines are built on workers.
class RootSignatureCache {
public:
    explicit RootSignatureCache(const Device& device);
    ~RootSignatureCache();

    /// Layout for the stages of one pipeline, in the order their SPIR-V was emitted. Null entries
    /// (absent stages) are skipped. stream_output: the pipeline declares stream output (native
    /// transform feedback). emulate_reduction: its float samples read the sampler table too
    /// (EDEN_SPIRV_TO_DXIL_EMULATE_REDUCTION). Throws std::runtime_error if D3D12 rejects the
    /// signature.
    const PipelineLayout& Get(std::span<const Shader::Info* const> infos, bool is_compute,
                              bool stream_output = false, bool emulate_reduction = false);

private:
    void CreateCommandSignatures(PipelineLayout& layout, bool is_compute);

    const Device& device;
    std::mutex mutex;
    std::unordered_map<u64, std::vector<std::pair<std::vector<u32>, std::unique_ptr<PipelineLayout>>>>
        layouts;
};

} // namespace D3D12
