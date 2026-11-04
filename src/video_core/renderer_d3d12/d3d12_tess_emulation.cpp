// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

#include <spirv_to_dxil.h>

#include "common/bug_tracker.h"
#include "common/logging.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/tessellation_emulation.h"
#include "video_core/host_shaders/d3d12_tessellate_comp_spv.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_command_stream.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_tess_emulation.h"

namespace D3D12 {

namespace {

namespace Tess = Shader::TessellationEmulation;

/// Root parameters of d3d12_tessellate.comp: its push constants, the patch records it reads the
/// levels from (the readonly SSBO at binding 0, t0), the counts, points, indices and draw record
/// (bindings 1 to 4, u1 to u4) and spirv_to_dxil's compute runtime data, which it does not read.
constexpr u32 TESSELLATOR_CONSTANTS_PARAM = 0;
constexpr u32 TESSELLATOR_FACTORS_PARAM = 1;
constexpr u32 TESSELLATOR_COUNTS_PARAM = 2;
constexpr u32 TESSELLATOR_POINTS_PARAM = 3;
constexpr u32 TESSELLATOR_INDICES_PARAM = 4;
constexpr u32 TESSELLATOR_RECORD_PARAM = 5;
constexpr u32 TESSELLATOR_RUNTIME_DATA_PARAM = 6;
constexpr u32 TESSELLATOR_PARAMS = 7;

/// Push constants of d3d12_tessellate.comp, in its order.
struct TessellatorConstants {
    u32 mode;
    u32 nr_patches;
    u32 partitioning;
    u32 domain;
    u32 points_mode;
    u32 ccw;
    u32 factor_stride;
    u32 factor_offset;
    u32 max_points;
    u32 max_indices;
    u32 yz_flip_mask;
    u32 draw_id;
    u32 lanes_per_patch;
};
constexpr u32 TESSELLATOR_CONSTANT_WORDS = sizeof(TessellatorConstants) / sizeof(u32);

constexpr u32 MODE_COUNT = 0;
constexpr u32 MODE_PREFIX = 1;
constexpr u32 MODE_GENERATE = 2;
constexpr u32 MODE_FUSED = 3;
constexpr u32 MODE_COUNT_PREFIX = 4;
constexpr u32 TESSELLATOR_GROUP_SIZE = 64;
/// Invocations that generate a patch together. With fewer than MIN_FUSED_LANES per patch, the
/// one-group pass leaves generation to its own dispatch.
constexpr u32 LANES_PER_PATCH = 16;
constexpr u32 MIN_FUSED_LANES = 8;

/// The draw record: INDIRECT_DRAW_INDEXED_WORDS, then the points and indices all patches need and
/// the first patch dropped.
constexpr u32 RECORD_WORDS = 13;
constexpr u32 RECORD_NEEDED_POINTS = 10;
constexpr u32 READBACK_SLOT_BYTES = 2 * sizeof(u32);

/// First sizes of the point and index buffers; they grow to what draws need.
constexpr u64 INITIAL_POINTS = 64 * 1024;
constexpr u64 INITIAL_INDICES = 256 * 1024;
/// The tessellator counts in 32 bits, and D3D12 caps a buffer view a little below 4 GiB.
constexpr u64 MAX_BUFFER_BYTES = u64{1} << 31;

constexpr u32 MAX_DISPATCH_GROUPS = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;

[[nodiscard]] constexpr u32 DivCeil(u32 value, u32 divisor) {
    return (value + divisor - 1) / divisor;
}

void SetComputeRuntimeData(CommandStream* cmd, u32 param, u32 groups) {
    dxil_spirv_compute_runtime_data runtime_data{};
    runtime_data.group_count_x = groups;
    runtime_data.group_count_y = 1;
    runtime_data.group_count_z = 1;
    std::array<u32, sizeof(runtime_data) / sizeof(u32)> words{};
    std::memcpy(words.data(), &runtime_data, sizeof(runtime_data));
    cmd->SetComputeRoot32BitConstants(param, static_cast<UINT>(words.size()), words.data(), 0);
}

} // Anonymous namespace

TessEmulation::TessEmulation(const Device& device_, Scheduler& scheduler_,
                             const ShaderCompiler& compiler_)
    : device{device_}, scheduler{scheduler_}, compiler{compiler_} {}

TessEmulation::~TessEmulation() {
    for (TrackedBuffer* const buffer :
         {&capture, &records, &counts, &points, &indices, &record}) {
        scheduler.DeferRelease(std::move(buffer->resource));
    }
    if (readback) {
        readback->Unmap(0, nullptr);
        scheduler.DeferRelease(std::move(readback));
    }
    if (stamp_readback) {
        stamp_readback->Unmap(0, nullptr);
        scheduler.DeferRelease(std::move(stamp_readback));
    }
}

void TessEmulation::Transition(TrackedBuffer& buffer, D3D12_RESOURCE_STATES next) {
    const u64 tick = scheduler.CurrentTick();
    const D3D12_RESOURCE_STATES before =
        buffer.state_tick == tick ? buffer.state : D3D12_RESOURCE_STATE_COMMON;
    if (before != next) {
        barriers.push_back(TransitionBarrier(buffer.resource.Get(), before, next));
    } else if (next == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        barriers.push_back(UavBarrier(buffer.resource.Get()));
    }
    buffer.state = next;
    buffer.state_tick = tick;
}

void TessEmulation::FlushBarriers(CommandStream* cmd) {
    if (!barriers.empty()) {
        cmd->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        barriers.clear();
    }
}

void TessEmulation::Reserve(TrackedBuffer& buffer, u64 size, bool unordered_access,
                            const char* what) {
    if (buffer.size >= size) {
        return;
    }
    const u64 new_size = std::bit_ceil(std::max<u64>(size, 64 * 1024));
    if (buffer.resource) {
        scheduler.DeferRelease(std::move(buffer.resource));
    }
    buffer.resource = CreateCommittedBuffer(
        device.Get(), new_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
        unordered_access ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE,
        what);
    buffer.address = buffer.resource->GetGPUVirtualAddress();
    buffer.size = new_size;
    buffer.state = D3D12_RESOURCE_STATE_COMMON;
    buffer.state_tick = 0;
}

void TessEmulation::CreateTessellatorPipeline() {
    std::array<D3D12_ROOT_PARAMETER, TESSELLATOR_PARAMS> params{};
    params[TESSELLATOR_CONSTANTS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[TESSELLATOR_CONSTANTS_PARAM].Constants = {.ShaderRegister = 0,
                                                     .RegisterSpace = PUSH_CONSTANT_SPACE,
                                                     .Num32BitValues = TESSELLATOR_CONSTANT_WORDS};
    params[TESSELLATOR_FACTORS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[TESSELLATOR_FACTORS_PARAM].Descriptor = {.ShaderRegister = 0, .RegisterSpace = 0};
    for (u32 binding = 1; binding <= 4; ++binding) {
        params[TESSELLATOR_FACTORS_PARAM + binding].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[TESSELLATOR_FACTORS_PARAM + binding].Descriptor = {.ShaderRegister = binding,
                                                                  .RegisterSpace = 0};
    }
    params[TESSELLATOR_RUNTIME_DATA_PARAM].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[TESSELLATOR_RUNTIME_DATA_PARAM].Constants = {
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
    tessellator_root_signature =
        CreateRootSignature(device.Get(), desc, "tessellator root signature");
    const std::vector<u8> dxil =
        compiler.Compile(D3D12_TESSELLATE_COMP_SPV, DXIL_SPIRV_SHADER_COMPUTE);
    const D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{
        .pRootSignature = tessellator_root_signature.Get(),
        .CS = {dxil.data(), dxil.size()},
    };
    ThrowIfFailed(device.Get()->CreateComputePipelineState(&pipeline_desc,
                                                           IID_PPV_ARGS(&tessellator_pipeline)),
                  "CreateComputePipelineState (tessellator)");
    record.resource = CreateCommittedBuffer(
        device.Get(), RECORD_WORDS * sizeof(u32), D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        "CreateCommittedResource (tessellation draw record)");
    record.address = record.resource->GetGPUVirtualAddress();
    record.size = RECORD_WORDS * sizeof(u32);
    readback = CreateCommittedBuffer(device.Get(), READBACK_SLOTS * READBACK_SLOT_BYTES,
                                     D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                                     D3D12_RESOURCE_FLAG_NONE,
                                     "CreateCommittedResource (tessellation needs readback)");
    void* mapped = nullptr;
    ThrowIfFailed(readback->Map(0, nullptr, &mapped), "Map (tessellation needs readback)");
    readback_words = static_cast<const u32*>(mapped);
}

void TessEmulation::ReadNeeds() {
    for (u32 slot = 0; slot < READBACK_SLOTS; ++slot) {
        const u64 tick = readback_ticks[slot];
        if (tick == 0 || !scheduler.IsKnownFree(tick)) {
            continue;
        }
        readback_ticks[slot] = 0;
        needed_points = std::max<u64>(needed_points, readback_words[slot * 2]);
        needed_indices = std::max<u64>(needed_indices, readback_words[slot * 2 + 1]);
    }
}

void TessEmulation::Stamp(CommandStream* cmd, u32 step) {
    if (step == 0) {
        stamping = false;
        if (!VideoCore::Perf::DetailedGpuProfileEnabled() || stamp_countdown-- != 0) {
            return;
        }
        stamp_countdown = STAMP_EVERY - 1;
        if (!stamps_tried) {
            stamps_tried = true;
            u64 frequency = 0;
            ID3D12Device* const dev = device.Get();
            const D3D12_QUERY_HEAP_DESC heap_desc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
                                                  .Count = STAMP_SLOTS * STAMPS_PER_DRAW,
                                                  .NodeMask = 0};
            void* mapped = nullptr;
            if (FAILED(device.Queue()->GetTimestampFrequency(&frequency)) || frequency == 0 ||
                FAILED(dev->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&stamp_heap)))) {
                stamp_heap.Reset();
                return;
            }
            stamp_readback = CreateCommittedBuffer(
                dev, STAMP_SLOTS * STAMPS_PER_DRAW * sizeof(u64), D3D12_HEAP_TYPE_READBACK,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
                "CreateCommittedResource (tessellation timestamps)");
            ThrowIfFailed(stamp_readback->Map(0, nullptr, &mapped),
                          "Map (tessellation timestamps)");
            stamp_data = static_cast<const u64*>(mapped);
            stamp_ns_per_tick = 1e9 / static_cast<double>(frequency);
        }
        if (!stamp_heap || stamp_ticks[stamp_next] != 0) {
            return; // the GPU still holds this slot: the draw goes unmeasured
        }
        stamping = true;
        stamp_slot = stamp_next;
        stamp_next = (stamp_next + 1) % STAMP_SLOTS;
    }
    if (!stamping) {
        return;
    }
    const u32 first = stamp_slot * STAMPS_PER_DRAW;
    cmd->EndQuery(stamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + step);
    if (step == STAMPS_PER_DRAW - 1) {
        cmd->ResolveQueryData(stamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first,
                              STAMPS_PER_DRAW, stamp_readback.Get(), first * sizeof(u64));
        stamp_ticks[stamp_slot] = scheduler.CurrentTick();
        stamping = false;
    }
}

void TessEmulation::ReadStamps() {
    using VideoCore::Perf::Counter;
    static constexpr std::array<Counter, STAMPS_PER_DRAW - 1> steps{
        Counter::TessCaptureGpuNs,    Counter::TessControlGpuNs,  Counter::TessBarriersGpuNs,
        Counter::TessTessellateGpuNs, Counter::TessReadbackGpuNs, Counter::TessDrawGpuNs};
    for (u32 slot = 0; slot < STAMP_SLOTS; ++slot) {
        const u64 tick = stamp_ticks[slot];
        if (tick == 0 || !scheduler.IsKnownFree(tick)) {
            continue;
        }
        stamp_ticks[slot] = 0;
        const u64* const stamps = stamp_data + slot * STAMPS_PER_DRAW;
        if (stamps[STAMPS_PER_DRAW - 1] <= stamps[0]) {
            continue;
        }
        VideoCore::Perf::Add(Counter::TessDrawsSampled, 1);
        for (u32 step = 0; step < steps.size(); ++step) {
            const u64 ticks = stamps[step + 1] > stamps[step] ? stamps[step + 1] - stamps[step] : 0;
            VideoCore::Perf::Add(steps[step],
                                 static_cast<u64>(static_cast<double>(ticks) * stamp_ns_per_tick));
        }
    }
}

u32 TessEmulation::Capture(CommandStream* cmd, const GraphicsPipeline& pipeline,
                           const DrawShape& shape) {
    const TessEmulationPlan& plan = pipeline.TessPlan();
    patches_per_instance = shape.num_vertices / plan.input_vertices;
    const u64 patches = u64{patches_per_instance} * shape.num_instances;
    if (patches == 0) {
        return 0;
    }
    const u64 groups = DivCeil(static_cast<u32>(std::min<u64>(patches, ~0u)),
                               plan.patches_per_group);
    if (patches > ~0u / plan.record_words || groups > MAX_DISPATCH_GROUPS) [[unlikely]] {
        BUG_TRACK(UnsupportedState, "tessellated draw of {} patches is larger than the "
                                    "emulation dispatches",
                  patches);
        return 0;
    }
    // The capture holds a slot per vertex an instance can name: its draw positions, or every
    // index up to the largest one.
    const u32 draw_vertices = patches_per_instance * plan.input_vertices;
    slots_per_instance = shape.is_indexed ? shape.index_slots : draw_vertices;
    const u64 slots = u64{slots_per_instance} * shape.num_instances;
    const u64 capture_bytes = slots * plan.input_vertex_bytes;
    if (slots > ~0u || capture_bytes > MAX_BUFFER_BYTES) [[unlikely]] {
        BUG_TRACK(UnsupportedState, "tessellated draw captures {} vertices, more than the "
                                    "emulation holds",
                  slots);
        return 0;
    }
    if (!tessellator_pipeline) {
        CreateTessellatorPipeline();
    }
    Reserve(capture, capture_bytes, true, "CreateCommittedResource (tessellation capture)");
    Reserve(records, (patches + 1) * plan.record_words * sizeof(u32), true,
            "CreateCommittedResource (tessellation patch records)");
    // The control shader reads the indices from a word-aligned address. A non-indexed draw reads
    // none; the capture stands in so the view is valid.
    index_size = shape.is_indexed ? shape.index_size : 0;
    index_address = shape.is_indexed ? shape.index_address & ~D3D12_GPU_VIRTUAL_ADDRESS{3}
                                     : capture.address;
    first_index_byte = shape.is_indexed ? static_cast<u32>(shape.index_address & 3) +
                                              shape.first_index * shape.index_size
                                        : 0;
    ReadStamps();
    Stamp(cmd, 0);
    Transition(capture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushBarriers(cmd);

    // Every root parameter is set before a draw; the capture reads only its constants.
    const PipelineLayout& layout = pipeline.Layout();
    const u32 index = layout.TessEmulationIndex();
    cmd->SetGraphicsRootShaderResourceView(index, records.address);
    cmd->SetGraphicsRootUnorderedAccessView(index + 1, capture.address);
    cmd->SetGraphicsRootShaderResourceView(index + 2, records.address);
    std::array<u32, Tess::CONSTANT_WORDS> constants{};
    constants[Tess::VERTICES_PER_INSTANCE_WORD] = slots_per_instance;
    constants[Tess::CAPTURE_VERTICES_WORD] = static_cast<u32>(slots);
    cmd->SetGraphicsRoot32BitConstants(index + 3, Tess::CONSTANT_WORDS, constants.data(), 0);
    cmd->SetGraphicsRootShaderResourceView(index + 4, index_address);

    scheduler.SetPipelineState(pipeline.TessCaptureHandle());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    if (shape.is_indexed) {
        cmd->DrawIndexedInstanced(draw_vertices, shape.num_instances, shape.first_index,
                                  static_cast<INT>(shape.base_vertex), shape.base_instance);
    } else {
        cmd->DrawInstanced(draw_vertices, shape.num_instances, shape.base_vertex,
                           shape.base_instance);
    }
    Stamp(cmd, 1);
    Transition(capture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return static_cast<u32>(patches);
}

void TessEmulation::Control(CommandStream* cmd, const GraphicsPipeline& pipeline, u32 patches) {
    const TessEmulationPlan& plan = pipeline.TessPlan();
    const PipelineLayout& layout = pipeline.Layout();
    const u32 index = layout.TessEmulationIndex();
    const u32 groups = DivCeil(patches, plan.patches_per_group);
    Transition(records, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushBarriers(cmd);
    cmd->SetComputeRootShaderResourceView(index, capture.address);
    cmd->SetComputeRootUnorderedAccessView(index + 1, records.address);
    cmd->SetComputeRootShaderResourceView(index + 2, capture.address);
    std::array<u32, Tess::CONSTANT_WORDS> constants{};
    constants[Tess::TOTAL_PATCHES_WORD] = patches;
    constants[Tess::PATCHES_PER_INSTANCE_WORD] = patches_per_instance;
    constants[Tess::VERTICES_PER_INSTANCE_WORD] = slots_per_instance;
    constants[Tess::TRASH_RECORD_WORD] = patches;
    constants[Tess::INDEX_SIZE_WORD] = index_size;
    constants[Tess::FIRST_INDEX_BYTE_WORD] = first_index_byte;
    cmd->SetComputeRoot32BitConstants(index + 3, Tess::CONSTANT_WORDS, constants.data(), 0);
    cmd->SetComputeRootShaderResourceView(index + 4, index_address);
    SetComputeRuntimeData(cmd, PipelineLayout::RUNTIME_DATA_INDEX, groups);
    scheduler.SetPipelineState(pipeline.TessControlHandle());
    cmd->Dispatch(groups, 1, 1);
    Stamp(cmd, 2);
    Transition(records, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void TessEmulation::Tessellate(CommandStream* cmd, const GraphicsPipeline& pipeline, u32 patches,
                               u32 yz_flip_mask, u32 draw_id) {
    const TessEmulationPlan& plan = pipeline.TessPlan();
    ReadNeeds();
    const u64 point_bytes = std::min<u64>(
        std::max<u64>(needed_points, INITIAL_POINTS) * Tess::POINT_WORDS * sizeof(u32),
        MAX_BUFFER_BYTES);
    const u64 index_bytes =
        std::min<u64>(std::max<u64>(needed_indices, INITIAL_INDICES) * sizeof(u32),
                      MAX_BUFFER_BYTES);
    Reserve(points, point_bytes, true, "CreateCommittedResource (tessellation points)");
    Reserve(indices, index_bytes, true, "CreateCommittedResource (tessellation indices)");
    Reserve(counts, u64{patches} * 2 * sizeof(u32), true,
            "CreateCommittedResource (tessellation counts)");

    Transition(counts, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(points, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(indices, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(record, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushBarriers(cmd);
    Stamp(cmd, 3);

    cmd->SetComputeRootSignature(tessellator_root_signature.Get());
    scheduler.SetPipelineState(tessellator_pipeline.Get());
    cmd->SetComputeRootShaderResourceView(TESSELLATOR_FACTORS_PARAM, records.address);
    cmd->SetComputeRootUnorderedAccessView(TESSELLATOR_COUNTS_PARAM, counts.address);
    cmd->SetComputeRootUnorderedAccessView(TESSELLATOR_POINTS_PARAM, points.address);
    cmd->SetComputeRootUnorderedAccessView(TESSELLATOR_INDICES_PARAM, indices.address);
    cmd->SetComputeRootUnorderedAccessView(TESSELLATOR_RECORD_PARAM, record.address);

    TessellatorConstants constants{
        .mode = MODE_COUNT,
        .nr_patches = patches,
        .partitioning = plan.partitioning,
        .domain = plan.domain,
        .points_mode = 0,
        .ccw = plan.ccw ? 1U : 0U,
        .factor_stride = plan.record_words,
        .factor_offset = 0,
        .max_points = static_cast<u32>(points.size / (Tess::POINT_WORDS * sizeof(u32))),
        .max_indices = static_cast<u32>(indices.size / sizeof(u32)),
        .yz_flip_mask = yz_flip_mask,
        .draw_id = draw_id,
        .lanes_per_patch = LANES_PER_PATCH,
    };
    const auto pass = [&](u32 mode, u32 groups) {
        constants.mode = mode;
        cmd->SetComputeRoot32BitConstants(TESSELLATOR_CONSTANTS_PARAM, TESSELLATOR_CONSTANT_WORDS,
                                          &constants, 0);
        SetComputeRuntimeData(cmd, TESSELLATOR_RUNTIME_DATA_PARAM, groups);
        cmd->Dispatch(groups, 1, 1);
    };
    const std::array<D3D12_RESOURCE_BARRIER, 2> uav{UavBarrier(counts.resource.Get()),
                                                    UavBarrier(record.resource.Get())};
    const u32 fused_lanes = TESSELLATOR_GROUP_SIZE / std::bit_ceil(patches);
    if (patches <= TESSELLATOR_GROUP_SIZE && fused_lanes >= MIN_FUSED_LANES) {
        // One group counts, sums and generates, with no barrier between the passes.
        constants.lanes_per_patch = fused_lanes;
        pass(MODE_FUSED, 1);
    } else {
        if (patches <= TESSELLATOR_GROUP_SIZE) {
            pass(MODE_COUNT_PREFIX, 1);
        } else {
            pass(MODE_COUNT, DivCeil(patches, TESSELLATOR_GROUP_SIZE));
            cmd->ResourceBarrier(1, uav.data());
            pass(MODE_PREFIX, 1);
        }
        cmd->ResourceBarrier(2, uav.data());
        // Each group generates GROUP_SIZE / lanes patches.
        u32 lanes = LANES_PER_PATCH;
        while (lanes > 1 &&
               DivCeil(patches, TESSELLATOR_GROUP_SIZE / lanes) > MAX_DISPATCH_GROUPS) {
            lanes /= 2;
        }
        constants.lanes_per_patch = lanes;
        pass(MODE_GENERATE, DivCeil(patches, TESSELLATOR_GROUP_SIZE / lanes));
    }
    Stamp(cmd, 4);

    Transition(points, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(indices, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    Transition(record, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
    FlushBarriers(cmd);
    // What this draw needed, for the buffers of the next ones.
    const u32 slot = readback_next;
    readback_next = (readback_next + 1) % READBACK_SLOTS;
    cmd->CopyBufferRegion(readback.Get(), slot * READBACK_SLOT_BYTES, record.resource.Get(),
                          RECORD_NEEDED_POINTS * sizeof(u32), READBACK_SLOT_BYTES);
    readback_ticks[slot] = scheduler.CurrentTick();
    Stamp(cmd, 5);
}

D3D12_PRIMITIVE_TOPOLOGY TessEmulation::Draw(CommandStream* cmd,
                                             const GraphicsPipeline& pipeline) {
    const PipelineLayout& layout = pipeline.Layout();
    const u32 index = layout.TessEmulationIndex();
    cmd->SetGraphicsRootShaderResourceView(index, records.address);
    cmd->SetGraphicsRootShaderResourceView(index + 2, points.address);
    const D3D12_INDEX_BUFFER_VIEW index_view{
        .BufferLocation = indices.address,
        .SizeInBytes = static_cast<UINT>(indices.size),
        .Format = DXGI_FORMAT_R32_UINT,
    };
    cmd->IASetIndexBuffer(&index_view);
    const D3D12_PRIMITIVE_TOPOLOGY topology =
        pipeline.TessPlan().domain == static_cast<u32>(Shader::TessPrimitive::Isolines)
            ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
            : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    cmd->IASetPrimitiveTopology(topology);
    cmd->ExecuteIndirect(layout.DrawSignature(true), 1, record.resource.Get(), 0, nullptr, 0);
    Stamp(cmd, 6);
    return topology;
}

} // namespace D3D12
