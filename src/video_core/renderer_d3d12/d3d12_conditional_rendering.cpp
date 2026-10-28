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

namespace D3D12 {
namespace {

using RenderEnable = Tegra::Engines::Maxwell3D::Regs::RenderEnable;

/// Holds the largest ResolveQueryData result (pipeline statistics); 256 bytes is the smallest
/// buffer placement anyway.
constexpr u64 PREDICATE_SIZE = 256;

/// The reports a condition compares: the one at its address and, 16 bytes later, the second.
constexpr u32 REPORTS_SIZE = 24;

/// Root parameters of d3d12_render_condition.comp: its push constant (compare_second), the
/// reports (the readonly SSBO at binding 0, t0), the predicate (binding 1, u1) and
/// spirv_to_dxil's compute runtime data, which it does not read.
constexpr u32 COMPARE_CONSTANTS_PARAM = 0;
constexpr u32 COMPARE_REPORTS_PARAM = 1;
constexpr u32 COMPARE_PREDICATE_PARAM = 2;
constexpr u32 COMPARE_RUNTIME_DATA_PARAM = 3;

/// What "lhs == rhs" comes down to.
struct Equality {
    enum class Kind {
        Unknown,   ///< needs more than one slice (left to the CPU)
        True,      ///< known on the CPU
        False,     ///< known on the CPU
        SliceZero, ///< equal exactly when slice counted zero
    };
    Kind kind;
    const HostCounter* slice{};
};

/// The slices of report that other does not count too, newest first.
std::span<const std::shared_ptr<HostCounter>> OwnSlices(const PendingReport& report,
                                                        const PendingReport& other) {
    const auto first = report.slices.begin();
    const auto last = first + report.num_slices;
    const auto other_last = other.slices.begin() + other.num_slices;
    // Slices form a chain: once one is shared, so are the ones below it.
    const auto shared = std::find_if(first, last, [&](const std::shared_ptr<HostCounter>& slice) {
        return std::find(other.slices.begin(), other_last, slice) != other_last;
    });
    return {first, shared};
}

Equality CompareEqual(const PendingReport& lhs, const PendingReport& rhs) {
    using enum Equality::Kind;
    if (!lhs.complete || !rhs.complete) {
        return {Unknown};
    }
    // A later report of a counter that was not reset counts on from the earlier one: the
    // slices both count cancel out (the before/after pattern around a bounding volume).
    const auto lhs_own = OwnSlices(lhs, rhs);
    const auto rhs_own = OwnSlices(rhs, lhs);
    if (lhs_own.empty() && rhs_own.empty()) {
        return {lhs.known == rhs.known ? True : False};
    }
    if (lhs_own.size() + rhs_own.size() != 1) {
        return {Unknown};
    }
    const bool lhs_counts = !lhs_own.empty();
    const PendingReport& counting = lhs_counts ? lhs : rhs;
    const PendingReport& fixed = lhs_counts ? rhs : lhs;
    // Counters never decrease: the counting report is at least what is known of it.
    if (counting.known > fixed.known) {
        return {False};
    }
    if (counting.known < fixed.known) {
        return {Unknown};
    }
    return {SliceZero, (lhs_counts ? lhs_own : rhs_own).front().get()};
}

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
    const std::optional<PendingReport> lhs = Report(address, gpu_memory);
    const std::optional<PendingReport> rhs =
        compare_second ? Report(address + 16, gpu_memory) : PendingReport{};
    if (!lhs || !rhs) {
        // The shader compares values in guest memory only, not host queries.
        const auto from_memory = [](const std::optional<PendingReport>& report) {
            return !report || report->num_slices == 0;
        };
        return from_memory(lhs) && from_memory(rhs) &&
               PredicateOnGpuReports(address, compare_second, render_if_equal);
    }
    const Equality equality = CompareEqual(*lhs, *rhs);
    switch (equality.kind) {
    case Equality::Kind::Unknown:
        if (!logged_cpu) {
            LOG_WARNING(Render,
                        "D3D12: a render condition (mode {}) needs more than one query slice; "
                        "the CPU waits for its result",
                        static_cast<u32>(guest_mode));
            logged_cpu = true;
        }
        return false;
    case Equality::Kind::True:
    case Equality::Kind::False:
        mode = (equality.kind == Equality::Kind::True) == render_if_equal ? Mode::Render
                                                                           : Mode::Skip;
        return true;
    case Equality::Kind::SliceZero:
        Predicate(*equality.slice, render_if_equal);
        if (!logged_gpu) {
            LOG_INFO(Render, "D3D12: render conditions evaluated on the GPU (mode {})",
                     static_cast<u32>(guest_mode));
            logged_gpu = true;
        }
        return true;
    }
    return false;
}

std::optional<PendingReport> ConditionalRendering::Report(GPUVAddr address,
                                                          Tegra::MemoryManager& gpu_memory) {
    if (const std::optional<DAddr> cpu_address = gpu_memory.GpuToCpuAddress(address)) {
        if (std::optional<PendingReport> cached = query_cache.PeekReport(*cpu_address)) {
            return cached;
        }
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(*cpu_address, sizeof(u64))) {
            return std::nullopt;
        }
    }
    // Not a query the host counts: read it as the CPU path does.
    PendingReport report;
    gpu_memory.ReadBlock(address, &report.known, sizeof(report.known));
    return report;
}

void ConditionalRendering::Predicate(const HostCounter& slice, bool render_if_zero) {
    CommandStream* const cmd = scheduler.CommandList();
    const u64 tick = scheduler.CurrentTick();
    // Earlier predicated draws have read the buffer when this resolve overwrites it: SetPredication
    // takes the value when it executes, and the barrier orders the write after those reads.
    const D3D12_RESOURCE_STATES current =
        predicate_tick == tick ? D3D12_RESOURCE_STATE_PREDICATION : D3D12_RESOURCE_STATE_COMMON;
    TransitionResource(cmd, predicate.Get(), current, D3D12_RESOURCE_STATE_COPY_DEST);
    const QueryPool::Slot& slot = slice.Slot();
    cmd->ResolveQueryData(slot.heap, slice.HostType(), slot.index, 1, predicate.Get(), 0);
    TransitionResource(cmd, predicate.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PREDICATION);
    predicate_tick = tick;
    value_offset = slice.ValueOffset();
    // The op names when draws are skipped.
    op = render_if_zero ? D3D12_PREDICATION_OP_NOT_EQUAL_ZERO : D3D12_PREDICATION_OP_EQUAL_ZERO;
    mode = Mode::Predicated;
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
    predicate_tick = tick;
    value_offset = 0;
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
    cmd->SetPredication(predicate.Get(), value_offset, op);
}

} // namespace D3D12
