// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <stdexcept>
#include <string>

#include <eden_spirv_to_dxil.h>
#include <fmt/format.h>
#include <spirv_to_dxil.h>

#include "common/bug_tracker.h"
#include "common/cityhash.h"
#include "common/logging.h"
#include "shader_recompiler/local_memory_scratch.h"
#include "shader_recompiler/transform_feedback_storage.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"

namespace D3D12 {

namespace {

/// Descriptor ranges of both tables, in slot order.
struct TableRanges {
    std::vector<D3D12_DESCRIPTOR_RANGE> resources;
    std::vector<D3D12_DESCRIPTOR_RANGE> samplers;
    u32 num_resources{};
    u32 num_samplers{};

    void Add(D3D12_DESCRIPTOR_RANGE_TYPE type, u32 reg, u32 count) {
        const bool sampler = type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        u32& offset = sampler ? num_samplers : num_resources;
        (sampler ? samplers : resources)
            .push_back({
                .RangeType = type,
                .NumDescriptors = count,
                .BaseShaderRegister = reg,
                .RegisterSpace = 0,
                .OffsetInDescriptorsFromTableStart = offset,
            });
        offset += count;
    }
};

/// Constant buffers moved out of the resource table into root CBVs.
struct RootCbvs {
    size_t budget{};            ///< how many root CBVs the root signature has room for
    std::vector<u32> registers; ///< shader register of each root CBV
    std::vector<u32> slots;     ///< per constant buffer of the stream: root slot or NO_ROOT_CBV
};

/// Walks one stage's descriptors in the order spirv_emit_context.cpp assigns bindings.
void AddStage(const Shader::Info& info, u32& binding, TableRanges& ranges, RootCbvs& root) {
    for (const auto& desc : info.constant_buffer_descriptors) {
        // Root descriptors cannot be indexed, so arrays stay in the table. So do buffers read at
        // dynamic offsets (sized 64 KiB by the shader info): a root CBV has no bounds, and games
        // bind those smaller than that (skinning matrices), where a table CBV reads zeros.
        constexpr u32 DYNAMIC_SIZE = 0x10000;
        if (desc.count == 1 && info.constant_buffer_used_sizes[desc.index] < DYNAMIC_SIZE &&
            root.registers.size() < root.budget) {
            root.slots.push_back(static_cast<u32>(root.registers.size()));
            root.registers.push_back(binding);
        } else {
            ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, binding, desc.count);
            root.slots.insert(root.slots.end(), desc.count, PipelineLayout::NO_ROOT_CBV);
        }
        binding += desc.count;
    }
    for (const auto& desc : info.storage_buffers_descriptors) {
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, binding, desc.count);
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, binding, desc.count);
        binding += desc.count;
    }
    // Texture buffers, image buffers and images take one binding each (the backend rejects or
    // ignores arrays of them), so their ranges hold a single descriptor.
    for (size_t i = 0; i < info.texture_buffer_descriptors.size(); ++i) {
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, binding, 1);
        ++binding;
    }
    for (size_t i = 0; i < info.image_buffer_descriptors.size(); ++i) {
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, binding, 1);
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, binding, 1);
        ++binding;
    }
    // Profile::descriptor_arrays_use_count: a texture array takes one binding per element.
    for (const auto& desc : info.texture_descriptors) {
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, binding, desc.count);
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, binding, desc.count);
        binding += desc.count;
    }
    for (const auto& desc : info.image_descriptors) {
        if (desc.count != 1) {
            BUG_TRACK_KEY(UnsupportedState, desc.count,
                          "storage image array of {} elements: only the first is bound",
                          desc.count);
            const Common::BugTracker::TapMute bug_tracker_mute;
            LOG_WARNING(Render, "D3D12: image array of {} elements; only the first is bound",
                        desc.count);
        }
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, binding, 1);
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, binding, 1);
        ++binding;
    }
}

void AppendKey(std::vector<u32>& key, std::span<const D3D12_DESCRIPTOR_RANGE> ranges) {
    key.push_back(static_cast<u32>(ranges.size()));
    for (const D3D12_DESCRIPTOR_RANGE& range : ranges) {
        key.push_back(static_cast<u32>(range.RangeType));
        key.push_back(range.NumDescriptors);
        key.push_back(range.BaseShaderRegister);
    }
}

} // Anonymous namespace

RootSignatureCache::RootSignatureCache(const Device& device_) : device{device_} {}

RootSignatureCache::~RootSignatureCache() = default;

void RootSignatureCache::CreateCommandSignatures(PipelineLayout& layout, bool is_compute) {
    static_assert(offsetof(dxil_spirv_vertex_runtime_data, draw_id) ==
                  (INDIRECT_DRAW_CONSTANT_WORDS - 1) * sizeof(u32));
    static_assert(offsetof(dxil_spirv_compute_runtime_data, group_count_z) ==
                  (INDIRECT_DISPATCH_CONSTANT_WORDS - 1) * sizeof(u32));
    const auto create = [&](D3D12_INDIRECT_ARGUMENT_TYPE type, u32 constant_words, u32 words,
                            ComPtr<ID3D12CommandSignature>& out) {
        const std::array<D3D12_INDIRECT_ARGUMENT_DESC, 2> arguments{{
            {.Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT,
             .Constant = {.RootParameterIndex = PipelineLayout::RUNTIME_DATA_INDEX,
                          .DestOffsetIn32BitValues = 0,
                          .Num32BitValuesToSet = constant_words}},
            {.Type = type},
        }};
        const D3D12_COMMAND_SIGNATURE_DESC desc{
            .ByteStride = words * static_cast<u32>(sizeof(u32)),
            .NumArgumentDescs = static_cast<UINT>(arguments.size()),
            .pArgumentDescs = arguments.data(),
            .NodeMask = 0,
        };
        const HRESULT hr = device.Get()->CreateCommandSignature(
            &desc, layout.root_signature.Get(), IID_PPV_ARGS(&out));
        if (FAILED(hr)) {
            LOG_ERROR(Render, "D3D12: CreateCommandSignature failed (0x{:08X})",
                      static_cast<u32>(hr));
        }
    };
    if (is_compute) {
        create(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH, INDIRECT_DISPATCH_CONSTANT_WORDS,
               INDIRECT_DISPATCH_WORDS, layout.dispatch_signature);
    } else {
        create(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, INDIRECT_DRAW_CONSTANT_WORDS,
               INDIRECT_DRAW_WORDS, layout.draw_signature);
        create(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED, INDIRECT_DRAW_CONSTANT_WORDS,
               INDIRECT_DRAW_INDEXED_WORDS, layout.draw_indexed_signature);
    }
}

const PipelineLayout& RootSignatureCache::Get(std::span<const Shader::Info* const> infos,
                                              bool is_compute, bool stream_output,
                                              bool emulate_reduction) {
    bool integer_textures = false;
    bool textures = false;
    bool local_memory_scratch = false;
    u32 xfb_buffers = 0;
    for (const Shader::Info* info : infos) {
        if (info) {
            local_memory_scratch |= info->local_memory_scratch_words != 0;
            xfb_buffers |= info->xfb_storage_buffers;
            for (const auto& desc : info->texture_descriptors) {
                integer_textures |= desc.is_integer;
            }
            textures |= !info->texture_descriptors.empty();
        }
    }
    // The sampler table (eden_integer_sampler_state rows): integer samples always read it, float
    // ones when the pipeline emulates MIN/MAX reduction.
    const bool sampler_table = integer_textures || (emulate_reduction && textures);
    const u32 runtime_data_words =
        static_cast<u32>((is_compute ? sizeof(dxil_spirv_compute_runtime_data)
                                     : sizeof(dxil_spirv_vertex_runtime_data)) /
                         sizeof(u32));
    RootCbvs root;
    if (!is_compute) {
        // Root constants take a dword each, tables one, root descriptors two.
        constexpr u32 ROOT_DWORDS = 64;
        const u32 xfb_words =
            xfb_buffers != 0 ? Shader::TransformFeedbackStorage::CONSTANT_WORDS +
                                   2 * static_cast<u32>(std::popcount(xfb_buffers))
                             : 0;
        const u32 used = PUSH_CONSTANT_WORDS + runtime_data_words + 2 +
                         (sampler_table ? 2 : 0) + (local_memory_scratch ? 2 : 0) + xfb_words;
        root.budget = std::min<size_t>(PipelineLayout::MAX_ROOT_CBVS,
                                       used < ROOT_DWORDS ? (ROOT_DWORDS - used) / 2 : 0);
    }
    TableRanges ranges;
    u32 binding = 0;
    for (const Shader::Info* info : infos) {
        if (info) {
            AddStage(*info, binding, ranges, root);
        }
    }
    if (sampler_table && binding > EDEN_INTEGER_SAMPLER_MAX_BINDINGS) {
        throw std::runtime_error(fmt::format(
            "D3D12: {} bindings exceed the integer sampler table", binding));
    }
    std::vector<u32> key{(is_compute ? 1u : 0u) | (sampler_table ? 2u : 0u) |
                         (local_memory_scratch ? 4u : 0u) | (xfb_buffers << 3) |
                         (stream_output ? 1u << 7 : 0u)};
    AppendKey(key, ranges.resources);
    AppendKey(key, ranges.samplers);
    key.push_back(static_cast<u32>(root.registers.size()));
    key.insert(key.end(), root.registers.begin(), root.registers.end());
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(key.data()),
                                        key.size() * sizeof(u32));

    std::scoped_lock lock{mutex};
    auto& bucket = layouts[hash];
    for (const auto& [existing_key, layout] : bucket) {
        if (existing_key == key) {
            return *layout;
        }
    }

    auto layout = std::make_unique<PipelineLayout>();
    layout->runtime_data_words = runtime_data_words;
    layout->num_resources = ranges.num_resources;
    layout->num_samplers = ranges.num_samplers;

    std::vector<D3D12_ROOT_PARAMETER> params;
    params.push_back({
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
        .Constants = {.ShaderRegister = 0,
                      .RegisterSpace = PUSH_CONSTANT_SPACE,
                      .Num32BitValues = PUSH_CONSTANT_WORDS},
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
    });
    params.push_back({
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
        .Constants = {.ShaderRegister = 0,
                      .RegisterSpace = RUNTIME_DATA_SPACE,
                      .Num32BitValues = layout->runtime_data_words},
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
    });
    if (!ranges.resources.empty()) {
        layout->resource_table_index = static_cast<u32>(params.size());
        params.push_back({
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = {.NumDescriptorRanges = static_cast<UINT>(ranges.resources.size()),
                                .pDescriptorRanges = ranges.resources.data()},
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        });
    }
    if (!ranges.samplers.empty()) {
        layout->sampler_table_index = static_cast<u32>(params.size());
        params.push_back({
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = {.NumDescriptorRanges = static_cast<UINT>(ranges.samplers.size()),
                                .pDescriptorRanges = ranges.samplers.data()},
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        });
    }
    if (sampler_table) {
        layout->integer_sampler_index = static_cast<u32>(params.size());
        params.push_back({
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
            .Descriptor = {.ShaderRegister = 0, .RegisterSpace = EDEN_INTEGER_SAMPLER_SPACE},
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        });
    }
    if (local_memory_scratch) {
        layout->local_memory_scratch_index = static_cast<u32>(params.size());
        params.push_back({
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV,
            .Descriptor = {.ShaderRegister = Shader::LocalMemoryScratch::BINDING,
                           .RegisterSpace = Shader::LocalMemoryScratch::DESCRIPTOR_SET},
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
        });
    }
    if (xfb_buffers != 0) {
        namespace Xfb = Shader::TransformFeedbackStorage;
        layout->xfb_constants_index = static_cast<u32>(params.size());
        params.push_back({
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
            .Constants = {.ShaderRegister = Xfb::CONSTANTS_BINDING,
                          .RegisterSpace = Xfb::DESCRIPTOR_SET,
                          .Num32BitValues = Xfb::CONSTANT_WORDS},
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX,
        });
        for (u32 index = 0; index < Xfb::NUM_BUFFERS; ++index) {
            if ((xfb_buffers & (1U << index)) == 0) {
                continue;
            }
            layout->xfb_buffer_indices[index] = static_cast<u32>(params.size());
            params.push_back({
                .ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV,
                .Descriptor = {.ShaderRegister = index, .RegisterSpace = Xfb::DESCRIPTOR_SET},
                .ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX,
            });
        }
    }
    if (!root.registers.empty()) {
        layout->root_cbv_index = static_cast<u32>(params.size());
        for (const u32 reg : root.registers) {
            params.push_back({
                .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
                .Descriptor = {.ShaderRegister = reg, .RegisterSpace = 0},
                .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
            });
        }
    }
    layout->root_cbv_registers = std::move(root.registers);
    layout->cbv_root_slots = std::move(root.slots);
    D3D12_ROOT_SIGNATURE_FLAGS flags =
        is_compute ? D3D12_ROOT_SIGNATURE_FLAG_NONE
                   : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (stream_output) {
        flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
        layout->stream_output = true;
    }
    const D3D12_ROOT_SIGNATURE_DESC desc{
        .NumParameters = static_cast<UINT>(params.size()),
        .pParameters = params.data(),
        .NumStaticSamplers = 0,
        .pStaticSamplers = nullptr,
        .Flags = flags,
    };
    layout->root_signature = CreateRootSignature(device.Get(), desc, "guest root signature");
    CreateCommandSignatures(*layout, is_compute);
    LOG_DEBUG(Render,
              "D3D12: root signature {:016x} ({} resource and {} sampler descriptors, {} root "
              "CBVs)",
              hash, ranges.num_resources, ranges.num_samplers, layout->NumRootCbvs());

    bucket.emplace_back(std::move(key), std::move(layout));
    return *bucket.back().second;
}

} // namespace D3D12
