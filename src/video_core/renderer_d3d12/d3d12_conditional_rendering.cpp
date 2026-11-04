// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <span>
#include <vector>

#include <eden_spirv_to_dxil.h>

#include "common/logging.h"
#include "video_core/host_shaders/d3d12_render_condition_comp_spv.h"
#include "video_core/memory_manager.h"
#include "video_core/query_cache.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_conditional_rendering.h"
#include "video_core/renderer_d3d12/d3d12_query_cache.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"

namespace D3D12 {
namespace {

using RenderEnable = Tegra::Engines::Maxwell3D::Regs::RenderEnable;

/// Holds the comparison's result; 256 bytes is the smallest buffer placement anyway.
constexpr u64 PREDICATE_SIZE = 256;

/// The reports a condition compares: the one at its address and, 16 bytes later, the second.
constexpr u32 REPORTS_SIZE = 24;

/// Where each snapshot's copy of the reports lands in the readback buffer.
constexpr u64 SNAPSHOT_STRIDE = 32;

/// Logs how conditions on GPU-written reports were decided, every 4096 of them.
template <typename Misses>
void CountGpuReportCondition(u64 read_back, u64 predicated, const Misses& misses) {
    if ((read_back + predicated) % 4096 == 0) {
        LOG_INFO(Render,
                 "D3D12: render conditions on GPU-written reports: {} read back without waiting, "
                 "{} predicated on the GPU ({} without a snapshot, {} moved, {} written since the "
                 "copy, {} before the GPU ran the copy)",
                 read_back, predicated, misses.none, misses.moved, misses.written,
                 misses.pending);
    }
}

/// Root parameters of d3d12_render_condition.comp: its push constant (compare_second), the
/// reports (the readonly SSBO at binding 0, t0), the predicate (binding 1, u1) and
/// spirv_to_dxil's compute runtime data, which it does not read.
constexpr u32 COMPARE_CONSTANTS_PARAM = 0;
constexpr u32 COMPARE_REPORTS_PARAM = 1;
constexpr u32 COMPARE_PREDICATE_PARAM = 2;
constexpr u32 COMPARE_RUNTIME_DATA_PARAM = 3;

} // namespace

ConditionalRendering::ConditionalRendering(const Device& device_, Scheduler& scheduler_,
                                           const ShaderCompiler& compiler_,
                                           QueryCache& query_cache_, BufferCache& buffer_cache_)
    : device{device_}, scheduler{scheduler_}, compiler{compiler_}, query_cache{query_cache_},
      buffer_cache{buffer_cache_},
      predicate{CreateCommittedBuffer(device.Get(), PREDICATE_SIZE, D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_STATE_COMMON,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                      "CreateCommittedResource (predicate)")} {}

bool ConditionalRendering::Accelerate(const Tegra::Engines::Maxwell3D::Regs& regs,
                                      Tegra::MemoryManager& gpu_memory) {
    mode = Mode::Render;
    // The fixed modes do not read memory: the CPU evaluates them without waiting.
    if (regs.render_enable_override != RenderEnable::Override::UseRenderEnable) {
        return false;
    }
    const RenderEnable::Mode guest_mode = regs.render_enable.mode;
    if (guest_mode != RenderEnable::Mode::Conditional &&
        guest_mode != RenderEnable::Mode::IfEqual && guest_mode != RenderEnable::Mode::IfNotEqual) {
        return false;
    }
    // Diagnostic ("occlusion_force_visible=1"): every conditional draw renders.
    if (VideoCommon::force_occlusion_visible.load(std::memory_order_relaxed)) {
        return true;
    }
    const GPUVAddr address = regs.render_enable.Address();
    // Conditional renders when the 64-bit report is not zero (as Ryujinx); the others compare
    // the reports at address and address + 16.
    const bool compare_second = guest_mode != RenderEnable::Mode::Conditional;
    const bool render_if_equal = guest_mode == RenderEnable::Mode::IfEqual;
    bool pending_counter = false;
    const std::optional<u64> lhs = CpuReport(address, gpu_memory, pending_counter);
    const std::optional<u64> rhs =
        compare_second ? CpuReport(address + 16, gpu_memory, pending_counter) : u64{0};
    if (lhs && rhs) {
        mode = (*lhs == *rhs) == render_if_equal ? Mode::Render : Mode::Skip;
        return true;
    }
    // The GPU writes a report: counters still pending are summed into the buffer cache first,
    // where the shader compares them.
    if (pending_counter) {
        query_cache.NotifyWFI();
    }
    return PredicateOnGpuReports(address, compare_second, render_if_equal);
}

std::optional<u64> ConditionalRendering::CpuReport(GPUVAddr address,
                                                   Tegra::MemoryManager& gpu_memory,
                                                   bool& pending_counter) {
    if (const std::optional<DAddr> cpu_address = gpu_memory.GpuToCpuAddress(address)) {
        if (query_cache.IsRegionGpuModified(*cpu_address, sizeof(u64))) {
            pending_counter = true;
            return std::nullopt;
        }
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(*cpu_address, sizeof(u64))) {
            return std::nullopt;
        }
    }
    u64 value;
    gpu_memory.ReadBlock(address, &value, sizeof(value));
    return value;
}

bool ConditionalRendering::PredicateOnGpuReports(GPUVAddr address, bool compare_second,
                                                 bool render_if_equal) {
    // Root views need 4-byte aligned addresses; reports are 16-byte aligned.
    if (address % 4 != 0) {
        return false;
    }
    std::scoped_lock lock{buffer_cache.mutex};
    const auto [buffer, offset] = buffer_cache.ObtainBuffer(
        address, REPORTS_SIZE, VideoCommon::ObtainBufferSynchronize::FullSynchronize,
        VideoCommon::ObtainBufferOperation::DoNothing);
    if (!buffer || !buffer->Handle()) {
        return false;
    }
    const u64 buffer_id = buffer->Id();
    const u64 generation = buffer->Generation();
    const auto snapshot = std::ranges::find(snapshots, address, &Snapshot::address);
    if (snapshot != snapshots.end() && snapshot->buffer_id == buffer_id &&
        snapshot->generation == generation && snapshot->offset == offset &&
        scheduler.IsFree(snapshot->tick)) {
        // Nothing wrote the buffer since the copy, and the GPU has run it: decide as the shader.
        std::array<u32, REPORTS_SIZE / sizeof(u32)> reports;
        const size_t index = static_cast<size_t>(snapshot - snapshots.begin());
        std::memcpy(reports.data(), readback_data.data() + index * SNAPSHOT_STRIDE, REPORTS_SIZE);
        const bool differ = reports[0] != (compare_second ? reports[4] : 0) ||
                            reports[1] != (compare_second ? reports[5] : 0);
        mode = differ != render_if_equal ? Mode::Render : Mode::Skip;
        CountGpuReportCondition(++read_back_conditions, predicated_conditions, snapshot_misses);
        return true;
    }
    if (snapshot == snapshots.end()) {
        ++snapshot_misses.none;
    } else if (snapshot->buffer_id != buffer_id || snapshot->offset != offset) {
        ++snapshot_misses.moved;
    } else if (snapshot->generation != generation) {
        ++snapshot_misses.written;
    } else {
        ++snapshot_misses.pending;
    }
    if (!compare_pipeline) {
        CreateComparePipeline();
    }
    CommandStream* const cmd = scheduler.CommandList();
    const u64 tick = scheduler.CurrentTick();
    buffer->Transition(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false);
    // As in Predicate: the barrier orders this write after the earlier predicated draws' reads.
    const D3D12_RESOURCE_STATES current =
        predicate_tick == tick ? D3D12_RESOURCE_STATE_PREDICATION : D3D12_RESOURCE_STATE_COMMON;
    TransitionResource(cmd, predicate.Get(), current, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->SetComputeRootSignature(compare_root_signature.Get());
    scheduler.SetPipelineState(compare_pipeline.Get());
    const u32 compare_word = compare_second ? 1U : 0U;
    cmd->SetComputeRoot32BitConstants(COMPARE_CONSTANTS_PARAM, 1, &compare_word, 0);
    cmd->SetComputeRootShaderResourceView(COMPARE_REPORTS_PARAM, buffer->Address() + offset);
    cmd->SetComputeRootUnorderedAccessView(COMPARE_PREDICATE_PARAM,
                                           predicate->GetGPUVirtualAddress());
    dxil_spirv_compute_runtime_data runtime_data{};
    runtime_data.group_count_x = 1;
    runtime_data.group_count_y = 1;
    runtime_data.group_count_z = 1;
    std::array<u32, sizeof(runtime_data) / sizeof(u32)> words{};
    std::memcpy(words.data(), &runtime_data, sizeof(runtime_data));
    cmd->SetComputeRoot32BitConstants(COMPARE_RUNTIME_DATA_PARAM, static_cast<UINT>(words.size()),
                                      words.data(), 0);
    cmd->Dispatch(1, 1, 1);
    TransitionResource(cmd, predicate.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_PREDICATION);
    buffer->Transition(D3D12_RESOURCE_STATE_COPY_SOURCE, false);
    RecordSnapshot(cmd, address, buffer_id, generation, buffer->Handle(), offset);
    CountGpuReportCondition(read_back_conditions, ++predicated_conditions, snapshot_misses);
    predicate_tick = tick;
    // The predicate is not zero when the reports differ (or the first is not zero); the op names
    // the value that skips the draws.
    op = render_if_equal ? D3D12_PREDICATION_OP_NOT_EQUAL_ZERO : D3D12_PREDICATION_OP_EQUAL_ZERO;
    mode = Mode::Predicated;
    if (!logged_gpu_reports) {
        LOG_INFO(Render, "D3D12: render conditions on GPU-written reports compared on the GPU ({})",
                 compare_second ? (render_if_equal ? "if equal" : "if not equal") : "if not zero");
        logged_gpu_reports = true;
    }
    return true;
}

void ConditionalRendering::RecordSnapshot(CommandStream* cmd, GPUVAddr address, u64 buffer_id,
                                          u64 generation, ID3D12Resource* source, u64 offset) {
    if (!readback) {
        try {
            readback = CreateMappedBuffer(device.Get(), NUM_SNAPSHOTS * SNAPSHOT_STRIDE,
                                          D3D12_HEAP_TYPE_READBACK, readback_data);
        } catch (const StagingOutOfMemory&) {
            return; // the next comparison tries again
        }
    }
    auto snapshot = std::ranges::find(snapshots, address, &Snapshot::address);
    if (snapshot == snapshots.end()) {
        snapshot = snapshots.begin() + next_snapshot;
        next_snapshot = (next_snapshot + 1) % NUM_SNAPSHOTS;
    }
    const u64 index = static_cast<u64>(snapshot - snapshots.begin());
    // The readback heap stays in COPY_DEST; copies on the one queue land in order.
    cmd->CopyBufferRegion(readback.Get(), index * SNAPSHOT_STRIDE, source, offset, REPORTS_SIZE);
    *snapshot = {
        .address = address,
        .buffer_id = buffer_id,
        .generation = generation,
        .offset = offset,
        .tick = scheduler.CurrentTick(),
    };
}

void ConditionalRendering::CreateComparePipeline() {
    std::array<D3D12_ROOT_PARAMETER, 4> params{};
    params[COMPARE_CONSTANTS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[COMPARE_CONSTANTS_PARAM].Constants = {
        .ShaderRegister = 0, .RegisterSpace = PUSH_CONSTANT_SPACE, .Num32BitValues = 1};
    params[COMPARE_REPORTS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[COMPARE_REPORTS_PARAM].Descriptor = {.ShaderRegister = 0, .RegisterSpace = 0};
    params[COMPARE_PREDICATE_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[COMPARE_PREDICATE_PARAM].Descriptor = {.ShaderRegister = 1, .RegisterSpace = 0};
    params[COMPARE_RUNTIME_DATA_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[COMPARE_RUNTIME_DATA_PARAM].Constants = {
        .ShaderRegister = 0,
        .RegisterSpace = RUNTIME_DATA_SPACE,
        .Num32BitValues = static_cast<UINT>(sizeof(dxil_spirv_compute_runtime_data) / sizeof(u32)),
    };
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
    compare_root_signature =
        CreateRootSignature(device.Get(), desc, "render condition root signature");
    const std::vector<u8> dxil =
        compiler.Compile(D3D12_RENDER_CONDITION_COMP_SPV, DXIL_SPIRV_SHADER_COMPUTE);
    const D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{
        .pRootSignature = compare_root_signature.Get(),
        .CS = {dxil.data(), dxil.size()},
    };
    ThrowIfFailed(device.Get()->CreateComputePipelineState(&pipeline_desc,
                                                           IID_PPV_ARGS(&compare_pipeline)),
                  "CreateComputePipelineState (render condition)");
}

void ConditionalRendering::Bind(CommandStream* cmd) {
    const u64 tick = scheduler.CurrentTick();
    if (predicate_tick != tick) {
        TransitionResource(cmd, predicate.Get(), D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_PREDICATION);
        predicate_tick = tick;
    }
    cmd->SetPredication(predicate.Get(), 0, op);
}

} // namespace D3D12
