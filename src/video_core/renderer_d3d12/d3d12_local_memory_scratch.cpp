// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <exception>
#include <stdexcept>

#include "common/alignment.h"
#include "common/logging.h"
#include "shader_recompiler/local_memory_scratch.h"
#include "video_core/host_shaders/d3d12_scratch_probe_frag_spv.h"
#include "video_core/host_shaders/full_screen_triangle_vert_spv.h"
#include "video_core/renderer_d3d12/d3d12_local_memory_scratch.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"

namespace D3D12 {
namespace {
std::atomic<PixelShaderScratchMode> scratch_mode{PixelShaderScratchMode::Auto};

/// Pool rows are allocated in steps, so pipelines of similar sizes share one pool.
constexpr u32 ROW_STEP = 64;
/// Slots stop doubling at this pool size.
constexpr u64 MAX_POOL_BYTES = u64{128} << 20;
/// Pool counters are sampled at most this often.
constexpr auto SAMPLE_INTERVAL = std::chrono::milliseconds{250};
/// Bytes copied per sample: the header words with the counters.
constexpr u64 SAMPLE_BYTES = Shader::LocalMemoryScratch::HEADER_WORDS * sizeof(u32);
/// A sample shows the pool short of slots when at least MIN_WAITS waves, and more than one in
/// WAIT_RATIO, scanned the whole bitmap before finding a slot.
constexpr u32 MIN_WAITS = 16;
constexpr u64 WAIT_RATIO = 32;

constexpr u32 PROBE_INDEX = 511;
constexpr u32 PROBE_VALUE = 0x3f123456;
constexpr DWORD PROBE_TIMEOUT_MS = 5000;

/// Draws one pixel of d3d12_scratch_probe.frag on a private queue and returns its first output.
u32 RunProbe(ID3D12Device* device, const ShaderCompiler& compiler) {
    const std::array<ShaderCompiler::PipelineStage, 2> stages{{
        {FULL_SCREEN_TRIANGLE_VERT_SPV, DXIL_SPIRV_SHADER_VERTEX},
        {D3D12_SCRATCH_PROBE_FRAG_SPV, DXIL_SPIRV_SHADER_FRAGMENT},
    }};
    const auto compiled = compiler.CompilePipeline(stages, {});

    std::array<D3D12_ROOT_PARAMETER, 2> params{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {.ShaderRegister = 0, .RegisterSpace = PUSH_CONSTANT_SPACE,
                           .Num32BitValues = 4};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {.ShaderRegister = 0, .RegisterSpace = RUNTIME_DATA_SPACE,
                           .Num32BitValues = 12};
    const D3D12_ROOT_SIGNATURE_DESC root_desc{
        .NumParameters = static_cast<UINT>(params.size()),
        .pParameters = params.data(),
        .NumStaticSamplers = 0,
        .pStaticSamplers = nullptr,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
    };
    const ComPtr<ID3D12RootSignature> root =
        CreateRootSignature(device, root_desc, "scratch probe root signature");

    constexpr DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_UINT;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.Get();
    pso_desc.VS = {compiled[0].dxil.data(), compiled[0].dxil.size()};
    pso_desc.PS = {compiled[1].dxil.data(), compiled[1].dxil.size()};
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = format;
    pso_desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso)),
                  "scratch probe pipeline");

    D3D12_RESOURCE_DESC target_desc{};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = 1;
    target_desc.Height = 1;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = format;
    target_desc.SampleDesc.Count = 1;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const D3D12_HEAP_PROPERTIES default_heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> target;
    ThrowIfFailed(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE,
                                                  &target_desc,
                                                  D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                  IID_PPV_ARGS(&target)),
                  "scratch probe target");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    u64 readback_bytes{};
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr,
                                  &readback_bytes);
    const ComPtr<ID3D12Resource> readback =
        CreateCommittedBuffer(device, readback_bytes, D3D12_HEAP_TYPE_READBACK,
                              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
                              "scratch probe readback");

    const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                                   .NumDescriptors = 1};
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ThrowIfFailed(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)),
                  "scratch probe RTV heap");
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    const D3D12_COMMAND_QUEUE_DESC queue_desc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    ThrowIfFailed(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)),
                  "scratch probe queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&allocator)),
                  "scratch probe allocator");
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                            pso.Get(), IID_PPV_ARGS(&cmd)),
                  "scratch probe command list");
    const std::array<u32, 4> constants{PROBE_INDEX, PROBE_VALUE, 0, 0};
    const std::array<u32, 12> runtime_data{};
    const std::array<float, 4> clear{};
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, 1, 1};
    cmd->ClearRenderTargetView(rtv, clear.data(), 0, nullptr);
    cmd->SetGraphicsRootSignature(root.Get());
    cmd->SetGraphicsRoot32BitConstants(0, 4, constants.data(), 0);
    cmd->SetGraphicsRoot32BitConstants(1, 12, runtime_data.data(), 0);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {target.Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_COPY_SOURCE};
    cmd->ResourceBarrier(1, &barrier);
    const D3D12_TEXTURE_COPY_LOCATION src{.pResource = target.Get(),
                                          .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                          .SubresourceIndex = 0};
    const D3D12_TEXTURE_COPY_LOCATION dst{.pResource = readback.Get(),
                                          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                          .PlacedFootprint = footprint};
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    ThrowIfFailed(cmd->Close(), "scratch probe close");

    ComPtr<ID3D12Fence> fence;
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
                  "scratch probe fence");
    const HANDLE event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!event) {
        throw std::runtime_error("scratch probe event");
    }
    ID3D12CommandList* const lists[]{cmd.Get()};
    queue->ExecuteCommandLists(1, lists);
    HRESULT hr = queue->Signal(fence.Get(), 1);
    if (SUCCEEDED(hr)) {
        hr = fence->SetEventOnCompletion(1, event);
    }
    const DWORD wait = SUCCEEDED(hr) ? WaitForSingleObjectEx(event, PROBE_TIMEOUT_MS, FALSE)
                                     : WAIT_FAILED;
    CloseHandle(event);
    ThrowIfFailed(hr, "scratch probe fence");
    if (wait != WAIT_OBJECT_0) {
        throw std::runtime_error("scratch probe GPU timeout");
    }
    void* mapped{};
    const D3D12_RANGE range{0, sizeof(u32)};
    ThrowIfFailed(readback->Map(0, &range, &mapped), "scratch probe map");
    u32 result{};
    std::memcpy(&result, mapped, sizeof(result));
    const D3D12_RANGE written{};
    readback->Unmap(0, &written);
    return result;
}
} // Anonymous namespace

void SetPixelShaderScratchMode(PixelShaderScratchMode mode) {
    scratch_mode.store(mode, std::memory_order_relaxed);
}

u32 SelectFragmentScratchLanes(const Device& device, const ShaderCompiler& compiler,
                               bool wave_ops, u32 wave_lanes) {
    const PixelShaderScratchMode mode = scratch_mode.load(std::memory_order_relaxed);
    // The pool's allocation needs wave ops (ballots of 128 lanes at most).
    if (mode == PixelShaderScratchMode::Native || !wave_ops || wave_lanes == 0 ||
        wave_lanes > Shader::LocalMemoryScratch::MASK_WORDS * 32) {
        LOG_INFO(Render, "D3D12: pixel shader local memory in private arrays ({})",
                 mode == PixelShaderScratchMode::Native ? "forced" : "no wave ops");
        return 0;
    }
    if (mode == PixelShaderScratchMode::Uav) {
        LOG_INFO(Render, "D3D12: pixel shader local memory in the UAV pool (forced, {} lanes)",
                 wave_lanes);
        return wave_lanes;
    }
    if (!compiler.IsAvailable()) {
        return 0;
    }
    try {
        const u32 result = RunProbe(device.Get(), compiler);
        const bool works = result == PROBE_VALUE;
        LOG_INFO(Render,
                 "D3D12: pixel shader private array probe read {:#010x} (expected {:#010x}): "
                 "local memory in {}",
                 result, PROBE_VALUE, works ? "private arrays" : "the UAV pool");
        return works ? 0 : wave_lanes;
    } catch (const std::exception& e) {
        // Without an answer, keep the native path: the pool costs memory and atomics.
        LOG_ERROR(Render,
                  "D3D12: pixel shader private array probe failed ({}); keeping private "
                  "arrays, ps_scratch=uav forces the pool",
                  e.what());
        return 0;
    }
}

LocalMemoryPool::LocalMemoryPool(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_} {}

LocalMemoryPool::~LocalMemoryPool() = default;

D3D12_GPU_VIRTUAL_ADDRESS LocalMemoryPool::BeginList() {
    list_tick = scheduler.CurrentTick();
    if (!ReadSamples()) {
        RecordSample();
    }
    return address;
}

D3D12_GPU_VIRTUAL_ADDRESS LocalMemoryPool::Grow(u32 lanes, u32 words) {
    using Shader::LocalMemoryScratch::MIN_SLOTS;
    using Shader::LocalMemoryScratch::PoolBytes;
    const bool same_lanes = lanes == pool_lanes;
    const u32 new_rows = Common::AlignUp(std::max(words, same_lanes ? rows : 0U), ROW_STEP);
    // Longer rows keep the slots the pool needed, as far as the cap allows.
    u32 new_slots = same_lanes ? slots : MIN_SLOTS;
    while (new_slots > MIN_SLOTS && PoolBytes(lanes, new_rows, new_slots) > MAX_POOL_BYTES) {
        new_slots /= 2;
    }
    Recreate(lanes, new_rows, new_slots);
    return address;
}

void LocalMemoryPool::Recreate(u32 lanes, u32 new_rows, u32 new_slots) {
    using namespace Shader::LocalMemoryScratch;
    const u64 bytes = PoolBytes(lanes, new_rows, new_slots);
    // Default heaps are zeroed: empty counters and bitmap, no live lanes.
    ComPtr<ID3D12Resource> new_buffer =
        CreateCommittedBuffer(device.Get(), bytes, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_COMMON,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "local memory pool");
    std::span<u8> upload_data;
    ComPtr<ID3D12Resource> upload =
        CreateMappedBuffer(device.Get(), sizeof(u32), D3D12_HEAP_TYPE_UPLOAD, upload_data);
    const u32 bitmap_words = new_slots / 32;
    std::memcpy(upload_data.data(), &bitmap_words, sizeof(bitmap_words));

    // The copy promotes the new pool from the common state; its draws need it as a UAV.
    CommandStream* const cmd = scheduler.CommandList();
    cmd->CopyBufferRegion(new_buffer.Get(), BITMAP_WORDS_WORD * sizeof(u32), upload.Get(), 0,
                          sizeof(u32));
    const D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(
        new_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->ResourceBarrier(1, &barrier);
    scheduler.DeferRelease(std::move(upload));
    if (buffer) {
        scheduler.DeferRelease(std::move(buffer));
    }
    LOG_INFO(Render, "D3D12: local memory pool of {} words x {} lanes x {} slots ({} KiB)",
             new_rows, lanes, new_slots, bytes / 1024);
    buffer = std::move(new_buffer);
    address = buffer->GetGPUVirtualAddress();
    rows = new_rows;
    pool_lanes = lanes;
    slots = new_slots;
    ++generation;
    last = {};
    capped = false;
    // The pool is a UAV for the rest of this list: no sample until the next one.
    list_tick = scheduler.CurrentTick();
}

bool LocalMemoryPool::ReadSamples() {
    using namespace Shader::LocalMemoryScratch;
    // Samples complete in submission order: from the oldest, which is the next one to write.
    for (u32 i = 0; i < SAMPLES; ++i) {
        const u32 index = (next_sample + i) % SAMPLES;
        Sample& sample = samples[index];
        if (!sample.pending) {
            continue;
        }
        if (!scheduler.IsFree(sample.tick)) {
            break;
        }
        sample.pending = false;
        if (sample.generation != generation) {
            continue;
        }
        Counters now;
        std::memcpy(&now, readback_data.data() + index * SAMPLE_BYTES, sizeof(now));
        // The counters only grow (and wrap): the difference is what happened since the last sample.
        const Counters delta{
            .acquires = now.acquires - last.acquires,
            .overflows = now.overflows - last.overflows,
            .waits = now.waits - last.waits,
        };
        last = now;
        const bool short_of_slots =
            delta.overflows != 0 ||
            (delta.waits >= MIN_WAITS && u64{delta.waits} * WAIT_RATIO > delta.acquires);
        if (!short_of_slots) {
            continue;
        }
        const u32 new_slots = slots * 2;
        if (new_slots > MAX_SLOTS || PoolBytes(pool_lanes, rows, new_slots) > MAX_POOL_BYTES) {
            if (!capped) {
                LOG_WARNING(Render,
                            "D3D12: local memory pool at its limit of {} slots ({} of {} waves "
                            "waited, {} shared the overflow slot)",
                            slots, delta.waits, delta.acquires, delta.overflows);
                capped = true;
            }
            continue;
        }
        LOG_INFO(Render,
                 "D3D12: local memory pool short of slots ({} of {} waves waited, {} shared the "
                 "overflow slot): growing to {}",
                 delta.waits, delta.acquires, delta.overflows, new_slots);
        Recreate(pool_lanes, rows, new_slots);
        return true;
    }
    return false;
}

void LocalMemoryPool::RecordSample() {
    const auto now = std::chrono::steady_clock::now();
    Sample& sample = samples[next_sample];
    if (now < next_sample_time || sample.pending) {
        return;
    }
    if (!readback) {
        try {
            readback = CreateMappedBuffer(device.Get(), SAMPLES * SAMPLE_BYTES,
                                          D3D12_HEAP_TYPE_READBACK, readback_data);
        } catch (const std::exception& e) {
            // The pool keeps working at its current size.
            LOG_WARNING(Render, "D3D12: local memory pool counters unavailable ({})", e.what());
            next_sample_time = std::chrono::steady_clock::time_point::max();
            return;
        }
    }
    // First use of the pool in this list: the copy promotes it from the common state, and the
    // draws need it back as a UAV.
    CommandStream* const cmd = scheduler.CommandList();
    cmd->CopyBufferRegion(readback.Get(), next_sample * SAMPLE_BYTES, buffer.Get(), 0,
                          SAMPLE_BYTES);
    const D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(
        buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->ResourceBarrier(1, &barrier);
    sample = {.tick = list_tick, .generation = generation, .pending = true};
    next_sample = (next_sample + 1) % SAMPLES;
    next_sample_time = now + SAMPLE_INTERVAL;
}

} // namespace D3D12
