// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <vector>

#include <eden_spirv_to_dxil.h>

#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/host_shaders/d3d12_vertex_realign_comp_spv.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_command_stream.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"
#include "video_core/renderer_d3d12/d3d12_vertex_realign.h"

namespace D3D12 {

namespace {

/// Root parameters of d3d12_vertex_realign.comp: its push constants, the source (binding 0, t0),
/// the copy (binding 1, u1), the word map (binding 2, t2) and spirv_to_dxil's compute runtime
/// data, which it does not read.
constexpr u32 CONSTANTS_PARAM = 0;
constexpr u32 SOURCE_PARAM = 1;
constexpr u32 DESTINATION_PARAM = 2;
constexpr u32 MAP_PARAM = 3;
constexpr u32 RUNTIME_DATA_PARAM = 4;
constexpr u32 CONSTANT_WORDS = 7;

constexpr u32 GROUP_SIZE = 64;
constexpr u32 MAX_GROUPS = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;

/// Copies grow in steps of this, so a stream that grows a little keeps its buffer.
constexpr u64 CAPACITY_GRANULARITY = u64{64} << 10;

constexpr u64 STATS_FRAMES = 1800;

[[nodiscard]] u32 SetOf(u64 buffer_id, u32 offset, u32 size, u32 stride,
                        u64 signature) noexcept {
    u64 hash = buffer_id * 0x9E3779B97F4A7C15ULL;
    hash ^= (u64{offset} << 32 | size) + 0xBF58476D1CE4E5B9ULL + (hash << 6) + (hash >> 2);
    hash ^= (u64{stride} << 40 ^ signature) + 0x94D049BB133111EBULL + (hash << 6) + (hash >> 2);
    hash *= 0xBF58476D1CE4E5B9ULL;
    return static_cast<u32>(hash >> 58); // NUM_SETS == 64
}

} // Anonymous namespace

VertexRealigner::VertexRealigner(const Device& device_, Scheduler& scheduler_,
                                 const ShaderCompiler& compiler_, StagingBufferPool& staging_)
    : device{device_}, scheduler{scheduler_}, compiler{compiler_}, staging{staging_} {}

VertexRealigner::~VertexRealigner() = default;

std::optional<D3D12_VERTEX_BUFFER_VIEW> VertexRealigner::Repack(Buffer& source, u32 offset,
                                                                u32 size,
                                                                const VertexRepack& plan) {
    static_assert(NUM_SETS == 64, "SetOf takes the top 6 bits of the hash");
    const u64 bytes = plan.Words() * 4;
    if (bytes == 0 || bytes > MAX_STREAM_BYTES || plan.words_per_vertex == 0) {
        ++refused;
        return std::nullopt;
    }
    const u64 buffer_id = source.Id();
    const u64 generation = source.Generation();
    const u32 set = SetOf(buffer_id, offset, size, plan.src_stride, plan.signature);
    Entry* entry = nullptr;
    for (u32 way = 0; way < NUM_WAYS; ++way) {
        Entry& candidate = entries[set * NUM_WAYS + way];
        if (candidate.Holds(buffer_id, offset, size, plan.src_stride, plan.signature)) {
            entry = &candidate;
            break;
        }
    }
    bool current = false;
    if (entry) {
        // Writes elsewhere in the buffer (other streams, uniforms...) leave the copy valid.
        current = entry->generation == generation ||
                  !source.WrittenSince(entry->generation, offset, u64{offset} + size);
        if (current) {
            entry->generation = generation;
        }
    } else {
        entry = Victim(set);
        if (!entry || !Reserve(*entry, bytes)) {
            ++refused;
            return std::nullopt;
        }
    }
    entry->last_frame = frame;
    entry->work = scheduler.Work();
    if (current) {
        ++hits;
        const u64 tick = scheduler.CurrentTick();
        if (entry->state_tick != tick) {
            // A buffer is promoted from COMMON (where every list leaves it) on its first read.
            entry->state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            entry->state_tick = tick;
        }
    } else {
        entry->buffer_id = buffer_id;
        entry->generation = generation;
        entry->signature = plan.signature;
        entry->offset = offset;
        entry->size = size;
        entry->src_stride = plan.src_stride;
        entry->bytes = static_cast<u32>(bytes);
        Dispatch(*entry, source, offset, size, plan);
    }
    return D3D12_VERTEX_BUFFER_VIEW{
        .BufferLocation = entry->address,
        .SizeInBytes = entry->bytes,
        .StrideInBytes = plan.view_stride,
    };
}

void VertexRealigner::TickFrame() {
    ++frame;
    for (Entry& entry : entries) {
        if (entry.resource && frame - entry.last_frame > IDLE_FRAMES) {
            Release(entry);
        }
    }
    if (frame % STATS_FRAMES == 0 && dispatches + refused != 0) {
        LOG_INFO(Render,
                 "D3D12: vertex repack: {} copies made ({} KiB), {} reused, {} bound in place, "
                 "{} KiB resident",
                 dispatches, dispatched_bytes / 1024, hits, refused, resident_bytes / 1024);
        hits = 0;
        dispatches = 0;
        dispatched_bytes = 0;
        refused = 0;
    }
}

VertexRealigner::Entry* VertexRealigner::Victim(u32 set) noexcept {
    const u64 work = scheduler.Work();
    Entry* victim = nullptr;
    for (u32 way = 0; way < NUM_WAYS; ++way) {
        Entry& candidate = entries[set * NUM_WAYS + way];
        // A copy another slot of the same draw is bound to must survive until the draw.
        if (work != 0 && candidate.work == work && candidate.buffer_id != 0) {
            continue;
        }
        if (!victim || candidate.buffer_id == 0 ||
            (victim->buffer_id != 0 && candidate.last_frame < victim->last_frame)) {
            victim = &candidate;
        }
    }
    return victim;
}

bool VertexRealigner::Reserve(Entry& entry, u64 bytes) {
    entry.buffer_id = 0; // whatever it held is lost from here on
    if (entry.resource && entry.capacity >= bytes) {
        return true;
    }
    const u64 capacity = Common::AlignUp(bytes, CAPACITY_GRANULARITY);
    if (resident_bytes - entry.capacity + capacity > MAX_RESIDENT_BYTES) {
        return false;
    }
    Release(entry);
    entry.resource = CreateCommittedBuffer(
        device.Get(), capacity, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "CreateCommittedResource (vertex repack)");
    entry.address = entry.resource->GetGPUVirtualAddress();
    entry.capacity = capacity;
    entry.state = D3D12_RESOURCE_STATE_COMMON;
    entry.state_tick = 0;
    entry.charge = Common::MemoryCharge(Common::MemoryAccount::BufferCache, capacity);
    resident_bytes += capacity;
    return true;
}

void VertexRealigner::Release(Entry& entry) {
    if (entry.resource) {
        // Draws already recorded may still read it.
        scheduler.DeferRelease(std::move(entry.resource));
        entry.resource = nullptr;
        resident_bytes -= entry.capacity;
    }
    entry.charge.Reset();
    entry.capacity = 0;
    entry.address = 0;
    entry.buffer_id = 0;
    entry.state = D3D12_RESOURCE_STATE_COMMON;
    entry.state_tick = 0;
}

void VertexRealigner::Dispatch(Entry& entry, Buffer& source, u32 offset, u32 size,
                               const VertexRepack& plan) {
    if (!pipeline) {
        CreatePipeline();
        LOG_INFO(Render,
                 "D3D12: repacking vertex streams the input assembler cannot fetch in place "
                 "(first: offset {:#x} size {} stride {} into {} dwords per vertex)",
                 offset, size, plan.src_stride, plan.words_per_vertex);
    }
    CommandStream* const cmd = scheduler.CommandList();

    // The source is read from its 4-aligned base, up to the dword holding its last byte.
    const u64 base = offset & ~u64{3};
    const u64 end = std::min<u64>(source.SizeBytes(), Common::AlignUp(u64{offset} + size, 4));
    const u32 src_words = static_cast<u32>((end - base) / 4);
    D3D12_GPU_VIRTUAL_ADDRESS map_address = source.Address() + base;
    if (!plan.map.empty()) {
        const StagingBufferRef upload =
            staging.Request(plan.map.size() * sizeof(u32), MemoryUsage::Upload);
        for (size_t word = 0; word < plan.map.size(); ++word) {
            const u32 value = plan.map[word] == VertexRepack::NO_SOURCE ? ~0U : plan.map[word];
            std::memcpy(upload.mapped_span.data() + word * sizeof(u32), &value, sizeof(u32));
        }
        map_address = upload.buffer->GetGPUVirtualAddress() + upload.offset;
    }

    const u32 total_words = static_cast<u32>(plan.Words());
    const u32 groups = (total_words + GROUP_SIZE - 1) / GROUP_SIZE;
    const u32 groups_x = std::min(groups, MAX_GROUPS);
    const u32 groups_y = (groups + groups_x - 1) / groups_x;
    const std::array<u32, CONSTANT_WORDS> constants{
        offset & 3U,
        src_words,
        plan.src_stride,
        plan.words_per_vertex,
        total_words,
        static_cast<u32>(plan.map.size()),
        groups_x * GROUP_SIZE,
    };

    source.Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
    Transition(cmd, entry, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    MarkGpuCommands(cmd, "vertex repack", source.Id(), plan.signature);
    cmd->SetComputeRootSignature(root_signature.Get());
    scheduler.SetPipelineState(pipeline.Get());
    cmd->SetComputeRoot32BitConstants(CONSTANTS_PARAM, CONSTANT_WORDS, constants.data(), 0);
    cmd->SetComputeRootShaderResourceView(SOURCE_PARAM, source.Address() + base);
    cmd->SetComputeRootUnorderedAccessView(DESTINATION_PARAM, entry.address);
    cmd->SetComputeRootShaderResourceView(MAP_PARAM, map_address);
    dxil_spirv_compute_runtime_data runtime_data{};
    runtime_data.group_count_x = groups_x;
    runtime_data.group_count_y = groups_y;
    runtime_data.group_count_z = 1;
    std::array<u32, sizeof(runtime_data) / sizeof(u32)> words{};
    std::memcpy(words.data(), &runtime_data, sizeof(runtime_data));
    cmd->SetComputeRoot32BitConstants(RUNTIME_DATA_PARAM, static_cast<UINT>(words.size()),
                                      words.data(), 0);
    cmd->Dispatch(groups_x, groups_y, 1);
    Transition(cmd, entry, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    ++dispatches;
    dispatched_bytes += entry.bytes;
}

void VertexRealigner::Transition(CommandStream* cmd, Entry& entry, D3D12_RESOURCE_STATES next) {
    const u64 tick = scheduler.CurrentTick();
    const D3D12_RESOURCE_STATES before =
        entry.state_tick == tick ? entry.state : D3D12_RESOURCE_STATE_COMMON;
    if (before != next) {
        TransitionResource(cmd, entry.resource.Get(), before, next);
    } else if (next == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        const D3D12_RESOURCE_BARRIER barrier = UavBarrier(entry.resource.Get());
        cmd->ResourceBarrier(1, &barrier);
    }
    entry.state = next;
    entry.state_tick = tick;
}

void VertexRealigner::CreatePipeline() {
    std::array<D3D12_ROOT_PARAMETER, 5> params{};
    params[CONSTANTS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[CONSTANTS_PARAM].Constants = {.ShaderRegister = 0,
                                         .RegisterSpace = PUSH_CONSTANT_SPACE,
                                         .Num32BitValues = CONSTANT_WORDS};
    params[SOURCE_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[SOURCE_PARAM].Descriptor = {.ShaderRegister = 0, .RegisterSpace = 0};
    params[DESTINATION_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[DESTINATION_PARAM].Descriptor = {.ShaderRegister = 1, .RegisterSpace = 0};
    params[MAP_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[MAP_PARAM].Descriptor = {.ShaderRegister = 2, .RegisterSpace = 0};
    params[RUNTIME_DATA_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[RUNTIME_DATA_PARAM].Constants = {
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
    root_signature = CreateRootSignature(device.Get(), desc, "vertex repack root signature");
    const std::vector<u8> dxil =
        compiler.Compile(D3D12_VERTEX_REALIGN_COMP_SPV, DXIL_SPIRV_SHADER_COMPUTE);
    const D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{
        .pRootSignature = root_signature.Get(),
        .CS = {dxil.data(), dxil.size()},
    };
    ThrowIfFailed(
        device.Get()->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)),
        "CreateComputePipelineState (vertex repack)");
}

} // namespace D3D12
