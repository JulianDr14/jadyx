// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <vector>

#include <eden_spirv_to_dxil.h>

#include "video_core/host_shaders/d3d12_draw_auto_comp_spv.h"
#include "video_core/renderer_d3d12/d3d12_command_stream.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_stream_output.h"

namespace D3D12 {

namespace {

/// Each counter gets 16 bytes: BufferFilledSizeLocation only needs 4-byte alignment, but some
/// drivers write more than the 32-bit count there.
constexpr u64 COUNTER_STRIDE = StreamOutput::COUNTERS_BYTES / 4;

/// Root parameters of d3d12_draw_auto.comp: its push constants (stride, yz_flip_mask), the byte
/// count (the readonly SSBO at binding 0, t0), the record (binding 1, u1) and spirv_to_dxil's
/// compute runtime data, which it does not read.
constexpr u32 DRAW_AUTO_CONSTANTS_PARAM = 0;
constexpr u32 DRAW_AUTO_SOURCE_PARAM = 1;
constexpr u32 DRAW_AUTO_RECORD_PARAM = 2;
constexpr u32 DRAW_AUTO_RUNTIME_DATA_PARAM = 3;
constexpr u32 DRAW_AUTO_CONSTANT_WORDS = 2;

} // Anonymous namespace

std::vector<D3D12_SO_DECLARATION_ENTRY> MakeStreamOutputDeclaration(
    const eden_spirv_to_dxil_stream_output& stream_output) {
    static constexpr std::array<const char*, 8> semantic_names{
        nullptr,           "TEXCOORD",        "SV_Position",          "SV_ClipDistance",
        "SV_CullDistance", "SV_PrimitiveID",  "SV_ViewportArrayIndex", "SV_RenderTargetArrayIndex",
    };
    const u32 count = std::min<u32>(stream_output.entry_count, EDEN_SPIRV_TO_DXIL_SO_MAX_ENTRIES);
    std::vector<D3D12_SO_DECLARATION_ENTRY> entries;
    entries.reserve(count);
    for (u32 index = 0; index < count; ++index) {
        const eden_spirv_to_dxil_so_entry& entry = stream_output.entries[index];
        const char* const name =
            entry.semantic < semantic_names.size() ? semantic_names[entry.semantic] : nullptr;
        entries.push_back({
            .Stream = entry.stream,
            .SemanticName = name,
            .SemanticIndex = name ? entry.semantic_index : 0U,
            .StartComponent = name ? entry.start_component : BYTE{0},
            .ComponentCount = entry.component_count,
            .OutputSlot = entry.output_slot,
        });
    }
    return entries;
}

D3D12_STREAM_OUTPUT_DESC DescribeStreamOutput(std::span<const D3D12_SO_DECLARATION_ENTRY> entries,
                                              std::span<const UINT, 4> strides, bool rasterize) {
    UINT num_strides = 0;
    for (const D3D12_SO_DECLARATION_ENTRY& entry : entries) {
        num_strides = std::max<UINT>(num_strides, entry.OutputSlot + 1U);
    }
    return {
        .pSODeclaration = entries.data(),
        .NumEntries = static_cast<UINT>(entries.size()),
        .pBufferStrides = strides.data(),
        .NumStrides = num_strides,
        .RasterizedStream = rasterize ? 0U : D3D12_SO_NO_RASTERIZED_STREAM,
    };
}

StreamOutput::StreamOutput(const Device& device_, Scheduler& scheduler_,
                           const ShaderCompiler& compiler_)
    : device{device_}, scheduler{scheduler_}, compiler{compiler_} {
    counters.resource = CreateCommittedBuffer(
        device.Get(), COUNTER_STRIDE * Maxwell::NumTransformFeedbackBuffers,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE,
        "CreateCommittedResource (stream output counters)");
    counters.address = counters.resource->GetGPUVirtualAddress();
}

StreamOutput::~StreamOutput() {
    scheduler.DeferRelease(std::move(counters.resource));
    scheduler.DeferRelease(std::move(draw_auto_record.resource));
}

void StreamOutput::Transition(CommandStream* cmd, TrackedBuffer& buffer,
                              D3D12_RESOURCE_STATES next) {
    const u64 tick = scheduler.CurrentTick();
    const D3D12_RESOURCE_STATES before =
        buffer.state_tick == tick ? buffer.state : D3D12_RESOURCE_STATE_COMMON;
    if (before != next) {
        TransitionResource(cmd, buffer.resource.Get(), before, next);
    } else if (next == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        const D3D12_RESOURCE_BARRIER barrier = UavBarrier(buffer.resource.Get());
        cmd->ResourceBarrier(1, &barrier);
    }
    buffer.state = next;
    buffer.state_tick = tick;
}

void StreamOutput::Bind(
    CommandStream* cmd, const Maxwell& regs,
    const std::array<BufferCacheRuntime::TransformFeedbackBinding, 4>& bindings) {
    const bool begins = !active;
    active = true;
    std::array<D3D12_WRITEBUFFERIMMEDIATE_PARAMETER, Maxwell::NumTransformFeedbackBuffers> clears{};
    UINT num_clears = 0;
    std::array<D3D12_STREAM_OUTPUT_BUFFER_VIEW, Maxwell::NumTransformFeedbackBuffers> views{};
    for (size_t index = 0; index < slots.size(); ++index) {
        const auto& buffer = regs.transform_feedback.buffers[index];
        const auto& binding = bindings[index];
        const Slot slot{
            .address = buffer.Address(),
            .size = buffer.size,
            .start_offset = buffer.start_offset,
            .enabled = buffer.enable != 0 && binding.size != 0,
        };
        if (begins || !slots[index].SameBinding(slot)) {
            slots[index] = slot;
            clears[num_clears++] = {.Dest = counters.address + index * COUNTER_STRIDE, .Value = 0};
        }
        if (!slot.enabled) {
            continue;
        }
        slots[index].written = true;
        binding.buffer->Transition(D3D12_RESOURCE_STATE_STREAM_OUT);
        views[index] = {
            .BufferLocation = binding.address,
            .SizeInBytes = binding.size,
            .BufferFilledSizeLocation = counters.address + index * COUNTER_STRIDE,
        };
    }
    if (num_clears != 0) {
        Transition(cmd, counters, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->WriteBufferImmediate(num_clears, clears.data());
    }
    Transition(cmd, counters, D3D12_RESOURCE_STATE_STREAM_OUT);
    cmd->SOSetTargets(0, static_cast<UINT>(views.size()), views.data());
    targets_tick = scheduler.CurrentTick();
}

std::optional<u32> StreamOutput::ByteCountSlot(const Maxwell& regs, u32 stream) const {
    for (u32 index = 0; index < Maxwell::NumTransformFeedbackBuffers; ++index) {
        const auto& buffer = regs.transform_feedback.buffers[index];
        if (buffer.enable == 0 || regs.transform_feedback.controls[index].stream != stream) {
            continue;
        }
        const Slot current{
            .address = buffer.Address(),
            .size = buffer.size,
            .start_offset = buffer.start_offset,
            .enabled = true,
        };
        if (slots[index].written && slots[index].SameBinding(current)) {
            return index;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

void StreamOutput::UnbindTargets(CommandStream* cmd) {
    // The GPU only stores a filled size when its target is unbound: until then the counter
    // still holds the count from before the last draws of this list.
    if (targets_tick == scheduler.CurrentTick()) {
        const std::array<D3D12_STREAM_OUTPUT_BUFFER_VIEW, Maxwell::NumTransformFeedbackBuffers>
            unbound{};
        cmd->SOSetTargets(0, static_cast<UINT>(unbound.size()), unbound.data());
        targets_tick = 0;
    }
}

void StreamOutput::CopyByteCount(CommandStream* cmd, u32 slot, ID3D12Resource* dst,
                                 u64 dst_offset) {
    UnbindTargets(cmd);
    Transition(cmd, counters, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(dst, dst_offset, counters.resource.Get(), slot * COUNTER_STRIDE,
                          sizeof(u32));
}

void StreamOutput::CopyCounters(CommandStream* cmd, ID3D12Resource* dst, u64 dst_offset) {
    UnbindTargets(cmd);
    Transition(cmd, counters, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(dst, dst_offset, counters.resource.Get(), 0, COUNTERS_BYTES);
}

void StreamOutput::CreateDrawAutoPipeline() {
    std::array<D3D12_ROOT_PARAMETER, 4> params{};
    params[DRAW_AUTO_CONSTANTS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[DRAW_AUTO_CONSTANTS_PARAM].Constants = {.ShaderRegister = 0,
                                                   .RegisterSpace = PUSH_CONSTANT_SPACE,
                                                   .Num32BitValues = DRAW_AUTO_CONSTANT_WORDS};
    params[DRAW_AUTO_SOURCE_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[DRAW_AUTO_SOURCE_PARAM].Descriptor = {.ShaderRegister = 0, .RegisterSpace = 0};
    params[DRAW_AUTO_RECORD_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[DRAW_AUTO_RECORD_PARAM].Descriptor = {.ShaderRegister = 1, .RegisterSpace = 0};
    params[DRAW_AUTO_RUNTIME_DATA_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[DRAW_AUTO_RUNTIME_DATA_PARAM].Constants = {
        .ShaderRegister = 0, .RegisterSpace = RUNTIME_DATA_SPACE,
        .Num32BitValues = static_cast<UINT>(sizeof(dxil_spirv_compute_runtime_data) / sizeof(u32))};
    for (auto& param : params) {
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    const D3D12_ROOT_SIGNATURE_DESC desc{
        .NumParameters = static_cast<UINT>(params.size()),
        .pParameters = params.data(),
        .NumStaticSamplers = 0,
        .pStaticSamplers = nullptr,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
    };
    draw_auto_root_signature =
        CreateRootSignature(device.Get(), desc, "DrawAuto root signature");
    const std::vector<u8> dxil = compiler.Compile(D3D12_DRAW_AUTO_COMP_SPV,
                                                  DXIL_SPIRV_SHADER_COMPUTE);
    const D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{
        .pRootSignature = draw_auto_root_signature.Get(),
        .CS = {dxil.data(), dxil.size()},
    };
    ThrowIfFailed(device.Get()->CreateComputePipelineState(&pipeline_desc,
                                                           IID_PPV_ARGS(&draw_auto_pipeline)),
                  "CreateComputePipelineState (DrawAuto)");
    draw_auto_record.resource = CreateCommittedBuffer(
        device.Get(), INDIRECT_DRAW_WORDS * sizeof(u32), D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        "CreateCommittedResource (DrawAuto record)");
    draw_auto_record.address = draw_auto_record.resource->GetGPUVirtualAddress();
}

ID3D12Resource* StreamOutput::BuildDrawAutoRecord(CommandStream* cmd,
                                                  D3D12_GPU_VIRTUAL_ADDRESS byte_count,
                                                  u32 stride, u32 yz_flip_mask) {
    if (!draw_auto_pipeline) {
        CreateDrawAutoPipeline();
    }
    Transition(cmd, draw_auto_record, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->SetComputeRootSignature(draw_auto_root_signature.Get());
    scheduler.SetPipelineState(draw_auto_pipeline.Get());
    const std::array<u32, DRAW_AUTO_CONSTANT_WORDS> constants{stride, yz_flip_mask};
    cmd->SetComputeRoot32BitConstants(DRAW_AUTO_CONSTANTS_PARAM, DRAW_AUTO_CONSTANT_WORDS,
                                      constants.data(), 0);
    cmd->SetComputeRootShaderResourceView(DRAW_AUTO_SOURCE_PARAM, byte_count);
    cmd->SetComputeRootUnorderedAccessView(DRAW_AUTO_RECORD_PARAM, draw_auto_record.address);
    dxil_spirv_compute_runtime_data runtime_data{};
    runtime_data.group_count_x = 1;
    runtime_data.group_count_y = 1;
    runtime_data.group_count_z = 1;
    std::array<u32, sizeof(runtime_data) / sizeof(u32)> words{};
    std::memcpy(words.data(), &runtime_data, sizeof(runtime_data));
    cmd->SetComputeRoot32BitConstants(DRAW_AUTO_RUNTIME_DATA_PARAM,
                                      static_cast<UINT>(words.size()), words.data(), 0);
    cmd->Dispatch(1, 1, 1);
    Transition(cmd, draw_auto_record, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    return draw_auto_record.resource.Get();
}

} // namespace D3D12
