// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

#include <spirv_to_dxil.h>

#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/host_shaders/d3d12_fg_compose_comp_spv.h"
#include "video_core/host_shaders/d3d12_fg_luma_comp_spv.h"
#include "video_core/host_shaders/d3d12_fg_motion_comp_spv.h"
#include "video_core/renderer_d3d12/d3d12_command_stream.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_frame_gen.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"

namespace D3D12 {

namespace {

std::atomic<FrameGenMode> frame_gen_mode{FrameGenMode::Off};

// Root signature shared by the three shaders: push constants, a table of five sampled textures
// (t0-t4, bindings 0-4) and one storage image (u5, binding 5), the statistics buffer as a root
// UAV (u6, binding 6) and spirv_to_dxil's runtime data. s0-s4 are static bilinear samplers.
constexpr u32 CONSTANTS_PARAM = 0;
constexpr u32 TABLE_PARAM = 1;
constexpr u32 STATS_PARAM = 2;
constexpr u32 RUNTIME_DATA_PARAM = 3;
constexpr u32 CONSTANT_WORDS = 8;
constexpr u32 NUM_SOURCES = 5;
constexpr u32 TABLE_SIZE = NUM_SOURCES + 1;

constexpr u32 GROUP_SIZE = 8;
constexpr u32 BLOCK_SIZE = 8;
/// Statistics words: each slot's motion error, then d3d12_fg_motion.comp's global motion (from
/// word 8: each slot's vector, from word 16 two words per wide vector, 41 x 41).
constexpr u32 STATS_SIZE = 16 * 1024;
constexpr u32 CPU_DESCRIPTORS = 96;

// d3d12_fg_motion.comp flags.
constexpr u32 HAS_COARSE = 1;
constexpr u32 HAS_TEMPORAL = 2;
constexpr u32 FULL_SEARCH = 4;
constexpr u32 FINEST = 8;
constexpr u32 GLOBAL_PICK = 16;
constexpr u32 HAS_GLOBAL = 32;
constexpr float MOTION_LAMBDA = 0.4f;
/// Mean level 0 motion error (0-255 luma) above which a pair is a scene cut.
constexpr float CUT_THRESHOLD = 28.0f;

constexpr D3D12_RESOURCE_STATES READ_STATE = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES COLOR_READ_STATE =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;

u32 FloatBits(float value) {
    return std::bit_cast<u32>(value);
}

void SetRuntimeData(auto* cmd, u32 groups_x, u32 groups_y) {
    dxil_spirv_compute_runtime_data data{};
    data.group_count_x = groups_x;
    data.group_count_y = groups_y;
    data.group_count_z = 1;
    std::array<u32, sizeof(data) / sizeof(u32)> words{};
    std::memcpy(words.data(), &data, sizeof(data));
    cmd->SetComputeRoot32BitConstants(RUNTIME_DATA_PARAM, static_cast<UINT>(words.size()),
                                      words.data(), 0);
}

u32 TableIndex(u32 a, u32 b) {
    return (a * FrameGen::NUM_SLOTS + b) * TABLE_SIZE;
}

} // namespace

void SetFrameGenMode(FrameGenMode mode) {
    frame_gen_mode.store(mode, std::memory_order_relaxed);
}

FrameGenMode GetFrameGenMode() noexcept {
    return frame_gen_mode.load(std::memory_order_relaxed);
}

FrameGen::FrameGen(const Device& device_, Scheduler& scheduler_, const ShaderCompiler& compiler_,
                   DescriptorRing& descriptor_ring_, SamplerHeap& sampler_heap_,
                   ID3D12Fence* present_fence_, u32 width_, u32 height_)
    : device{device_}, scheduler{scheduler_}, compiler{compiler_},
      descriptor_ring{descriptor_ring_}, sampler_heap{sampler_heap_},
      present_fence{present_fence_}, width{width_}, height{height_} {}

FrameGen::~FrameGen() = default;

bool FrameGen::EnsureReady() {
    if (state == State::NotBuilt) {
        try {
            CreatePipelines();
            CreateResources();
            state = State::Ready;
            LOG_INFO(Render,
                     "D3D12: frame generation ready ({}x{}, motion at 1/{}, {} history frames)",
                     width, height, luma_scale, NUM_SLOTS);
        } catch (const std::exception& e) {
            LOG_ERROR(Render, "D3D12: frame generation unavailable: {}", e.what());
            state = State::Failed;
            // Nothing half-built stays: the memory goes back to the game.
            for (Slot& slot : slots) {
                slot = Slot{};
            }
            scratch_motion = {};
            output = Texture{};
            stats.Reset();
        }
    }
    return state == State::Ready;
}

void FrameGen::CreatePipelines() {
    const std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{{
        {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
         .NumDescriptors = NUM_SOURCES,
         .BaseShaderRegister = 0,
         .RegisterSpace = 0,
         .OffsetInDescriptorsFromTableStart = 0},
        {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
         .NumDescriptors = 1,
         .BaseShaderRegister = NUM_SOURCES,
         .RegisterSpace = 0,
         .OffsetInDescriptorsFromTableStart = NUM_SOURCES},
    }};
    std::array<D3D12_ROOT_PARAMETER, 4> params{};
    params[CONSTANTS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[CONSTANTS_PARAM].Constants = {.ShaderRegister = 0,
                                         .RegisterSpace = PUSH_CONSTANT_SPACE,
                                         .Num32BitValues = CONSTANT_WORDS};
    params[TABLE_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[TABLE_PARAM].DescriptorTable = {.NumDescriptorRanges = static_cast<UINT>(ranges.size()),
                                           .pDescriptorRanges = ranges.data()};
    params[STATS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[STATS_PARAM].Descriptor = {.ShaderRegister = NUM_SOURCES + 1, .RegisterSpace = 0};
    params[RUNTIME_DATA_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[RUNTIME_DATA_PARAM].Constants = {
        .ShaderRegister = 0,
        .RegisterSpace = RUNTIME_DATA_SPACE,
        .Num32BitValues = static_cast<UINT>(sizeof(dxil_spirv_compute_runtime_data) / sizeof(u32))};
    for (auto& param : params) {
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    std::array<D3D12_STATIC_SAMPLER_DESC, NUM_SOURCES> samplers{};
    for (u32 i = 0; i < NUM_SOURCES; ++i) {
        samplers[i] = {
            .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .MipLODBias = 0.0f,
            .MaxAnisotropy = 1,
            .ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
            .BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
            .MinLOD = 0.0f,
            .MaxLOD = 0.0f,
            .ShaderRegister = i,
            .RegisterSpace = 0,
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        };
    }
    const D3D12_ROOT_SIGNATURE_DESC desc{
        .NumParameters = static_cast<UINT>(params.size()),
        .pParameters = params.data(),
        .NumStaticSamplers = static_cast<UINT>(samplers.size()),
        .pStaticSamplers = samplers.data(),
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
    };
    root_signature = CreateRootSignature(device.Get(), desc, "frame generation root signature");

    const auto build = [&](std::span<const u32> spirv, const char* what) {
        const std::vector<u8> dxil = compiler.Compile(spirv, DXIL_SPIRV_SHADER_COMPUTE);
        const D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{
            .pRootSignature = root_signature.Get(),
            .CS = {dxil.data(), dxil.size()},
        };
        ComPtr<ID3D12PipelineState> pipeline;
        ThrowIfFailed(device.Get()->CreateComputePipelineState(&pipeline_desc,
                                                               IID_PPV_ARGS(&pipeline)),
                      what);
        return pipeline;
    };
    luma_pipeline = build(D3D12_FG_LUMA_COMP_SPV, "CreateComputePipelineState (frame gen luma)");
    motion_pipeline =
        build(D3D12_FG_MOTION_COMP_SPV, "CreateComputePipelineState (frame gen motion)");
    compose_pipeline =
        build(D3D12_FG_COMPOSE_COMP_SPV, "CreateComputePipelineState (frame gen compose)");
}

FrameGen::Texture FrameGen::CreateTexture(u32 tex_width, u32 tex_height, DXGI_FORMAT format,
                                          bool uav, D3D12_RESOURCE_STATES initial,
                                          const wchar_t* name) {
    ID3D12Device* const dev = device.Get();
    Texture texture;
    texture.width = tex_width;
    texture.height = tex_height;
    texture.state = initial;
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = tex_width,
        .Height = tex_height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = format,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE,
    };
    ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial,
                                               nullptr, IID_PPV_ARGS(&texture.resource)),
                  "CreateCommittedResource (frame generation)");
    texture.resource->SetName(name);

    const auto next_handle = [&] {
        if (cpu_used >= CPU_DESCRIPTORS) {
            throw std::runtime_error("frame generation: out of descriptors");
        }
        D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(cpu_used++) * descriptor_size;
        return handle;
    };
    const D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{
        .Format = format,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D = {.MostDetailedMip = 0, .MipLevels = 1, .PlaneSlice = 0,
                      .ResourceMinLODClamp = 0.0f},
    };
    texture.srv = next_handle();
    dev->CreateShaderResourceView(texture.resource.Get(), &srv_desc, texture.srv);
    if (uav) {
        const D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{
            .Format = format,
            .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
            .Texture2D = {.MipSlice = 0, .PlaneSlice = 0},
        };
        texture.uav = next_handle();
        dev->CreateUnorderedAccessView(texture.resource.Get(), nullptr, &uav_desc, texture.uav);
    }
    return texture;
}

void FrameGen::CreateResources() {
    ID3D12Device* const dev = device.Get();
    descriptor_size =
        dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_DESCRIPTOR_HEAP_DESC cpu_desc{
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = CPU_DESCRIPTORS,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 0,
    };
    ThrowIfFailed(dev->CreateDescriptorHeap(&cpu_desc, IID_PPV_ARGS(&cpu_heap)),
                  "CreateDescriptorHeap (frame generation)");
    const D3D12_DESCRIPTOR_HEAP_DESC table_desc{
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = NUM_SLOTS * NUM_SLOTS * TABLE_SIZE,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
        .NodeMask = 0,
    };
    ThrowIfFailed(dev->CreateDescriptorHeap(&table_desc, IID_PPV_ARGS(&table_heap)),
                  "CreateDescriptorHeap (frame generation tables)");
    cpu_used = 0;

    // Unused table entries hold null views.
    null_srv = cpu_heap->GetCPUDescriptorHandleForHeapStart();
    null_srv.ptr += static_cast<SIZE_T>(cpu_used++) * descriptor_size;
    null_uav = cpu_heap->GetCPUDescriptorHandleForHeapStart();
    null_uav.ptr += static_cast<SIZE_T>(cpu_used++) * descriptor_size;
    const D3D12_SHADER_RESOURCE_VIEW_DESC null_srv_desc{
        .Format = DXGI_FORMAT_R8_UNORM,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D = {.MostDetailedMip = 0, .MipLevels = 1, .PlaneSlice = 0,
                      .ResourceMinLODClamp = 0.0f},
    };
    dev->CreateShaderResourceView(nullptr, &null_srv_desc, null_srv);
    const D3D12_UNORDERED_ACCESS_VIEW_DESC null_uav_desc{
        .Format = DXGI_FORMAT_R8_UNORM,
        .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
        .Texture2D = {.MipSlice = 0, .PlaneSlice = 0},
    };
    dev->CreateUnorderedAccessView(nullptr, nullptr, &null_uav_desc, null_uav);

    std::array<std::pair<u32, u32>, NUM_LEVELS> level_sizes{};
    // 4K outputs (FSR's) estimate motion at a quarter resolution: the same texels per output
    // pixel as a 1080p output at half, and the same cost.
    luma_scale = static_cast<u64>(width) * height > 2560ULL * 1440ULL ? 4U : 2U;
    u32 level_width = Common::DivCeil(width, luma_scale);
    u32 level_height = Common::DivCeil(height, luma_scale);
    for (u32 level = 0; level < NUM_LEVELS; ++level) {
        level_sizes[level] = {level_width, level_height};
        level_width = std::max(Common::DivCeil(level_width, 2U), 1U);
        level_height = std::max(Common::DivCeil(level_height, 2U), 1U);
    }
    const auto blocks = [&](u32 level) {
        return std::pair{Common::DivCeil(level_sizes[level].first, BLOCK_SIZE),
                         Common::DivCeil(level_sizes[level].second, BLOCK_SIZE)};
    };

    for (u32 i = 0; i < NUM_SLOTS; ++i) {
        Slot& slot = slots[i];
        slot = Slot{};
        slot.color = CreateTexture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, false,
                                   COLOR_READ_STATE, L"Frame gen color");
        for (u32 level = 0; level < NUM_LEVELS; ++level) {
            slot.luma[level] =
                CreateTexture(level_sizes[level].first, level_sizes[level].second,
                              DXGI_FORMAT_R8_UNORM, true, READ_STATE, L"Frame gen luma");
        }
        const auto [blocks_x, blocks_y] = blocks(0);
        slot.motion = CreateTexture(blocks_x, blocks_y, DXGI_FORMAT_R16G16B16A16_FLOAT, true,
                                    READ_STATE, L"Frame gen motion");
    }
    for (u32 level = 1; level < NUM_LEVELS; ++level) {
        const auto [blocks_x, blocks_y] = blocks(level);
        scratch_motion[level] = CreateTexture(blocks_x, blocks_y, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                              true, READ_STATE, L"Frame gen coarse motion");
    }
    output = CreateTexture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, true,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Frame gen output");
    stats = CreateCommittedBuffer(dev, STATS_SIZE, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_STATE_COMMON,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  "CreateCommittedResource (frame generation statistics)");

    // One compose table per ordered slot pair: A and B's color, level 0 luma, B's motion, the
    // output.
    const D3D12_CPU_DESCRIPTOR_HANDLE table_base = table_heap->GetCPUDescriptorHandleForHeapStart();
    for (u32 a = 0; a < NUM_SLOTS; ++a) {
        for (u32 b = 0; b < NUM_SLOTS; ++b) {
            const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, TABLE_SIZE> entries{
                slots[a].color.srv, slots[b].color.srv,  slots[a].luma[0].srv,
                slots[b].luma[0].srv, slots[b].motion.srv, output.uav,
            };
            for (u32 i = 0; i < TABLE_SIZE; ++i) {
                D3D12_CPU_DESCRIPTOR_HANDLE dest = table_base;
                dest.ptr += static_cast<SIZE_T>(TableIndex(a, b) + i) * descriptor_size;
                dev->CopyDescriptorsSimple(1, dest, entries[i],
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
        }
    }
}

void FrameGen::Transition(CommandStream* cmd, Texture& texture, D3D12_RESOURCE_STATES next) {
    if (texture.state != next) {
        TransitionResource(cmd, texture.resource.Get(), texture.state, next);
        texture.state = next;
    }
}

void FrameGen::Dispatch(CommandStream* cmd, ID3D12PipelineState* pipeline,
                        const std::array<u32, CONSTANT_WORDS>& constants,
                        const std::array<const Texture*, NUM_SOURCES>& sources, Texture& dest,
                        u32 groups_x, u32 groups_y) {
    // Descriptors first: a full ring flushes the command list.
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, TABLE_SIZE> descriptors{};
    for (u32 i = 0; i < NUM_SOURCES; ++i) {
        descriptors[i] = sources[i] ? sources[i]->srv : null_srv;
    }
    descriptors[NUM_SOURCES] = dest.uav;
    const D3D12_GPU_DESCRIPTOR_HANDLE table = descriptor_ring.Upload(descriptors);
    cmd = scheduler.CommandList();
    Transition(cmd, dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The rasterizer binds these same heaps: its draws need nothing rebound after this.
    ID3D12DescriptorHeap* const heaps[] = {descriptor_ring.Heap(), sampler_heap.Heap()};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetComputeRootSignature(root_signature.Get());
    scheduler.SetPipelineState(pipeline);
    cmd->SetComputeRoot32BitConstants(CONSTANTS_PARAM, CONSTANT_WORDS, constants.data(), 0);
    cmd->SetComputeRootDescriptorTable(TABLE_PARAM, table);
    cmd->SetComputeRootUnorderedAccessView(STATS_PARAM, stats->GetGPUVirtualAddress());
    SetRuntimeData(cmd, groups_x, groups_y);
    cmd->Dispatch(groups_x, groups_y, 1);
    Transition(cmd, dest, READ_STATE);
}

void FrameGen::WaitForPresentFence(u64 value) {
    if (!device.HasPresentQueue()) {
        // One queue: the outputs that read the slot were submitted before it was unpinned, so
        // they run before this ingest. The GPU thread does not wait on the display.
        return;
    }
    const u64 completed = present_fence->GetCompletedValue();
    if (value == 0 || completed >= value || completed == UINT64_MAX) {
        return;
    }
    // No event: the present thread owns its event.
    ThrowIfFailed(present_fence->SetEventOnCompletion(value, nullptr),
                  "ID3D12Fence::SetEventOnCompletion (frame generation)");
}

u32 FrameGen::OnFrame(u64 vsync_index, bool allowed, double arrival, double queue_time) {
    FrameGenMode mode = allowed ? GetFrameGenMode() : FrameGenMode::Off;
    if (mode != FrameGenMode::Off && state == State::Failed) {
        mode = FrameGenMode::Off;
    }
    u32 rate{};
    {
        std::scoped_lock lock{mutex};
        const bool queue_clock = pacer.QueueClock();
        rate = pacer.OnFrame(mode, vsync_index, arrival, queue_time);
        frame_content = pacer.Content();
        if (pacer.QueueClock() != queue_clock) {
            LOG_INFO(Render, "D3D12: frame generation paces on the game's {}",
                     pacer.QueueClock() ? "queue times" : "vsyncs");
        }
        if (rate != target) {
            LOG_INFO(Render, "D3D12: frame generation {} (game at {:.1f} fps)",
                     rate != 0 ? fmt::format("to {} fps", rate) : std::string{"off"},
                     pacer.BaseInterval() > 0.0 ? 1.0 / pacer.BaseInterval() : 0.0);
            target = rate;
            clock_valid = false;
        }
    }
    if (rate != 0 && !EnsureReady()) {
        std::scoped_lock lock{mutex};
        target = 0;
        return 0;
    }
    return rate;
}

std::optional<u32> FrameGen::RecordIngest(ID3D12Resource* image) {
    if (state != State::Ready) {
        return std::nullopt;
    }
    u32 index{};
    u64 last_read{};
    std::optional<u32> prev;
    {
        std::scoped_lock lock{mutex};
        // The oldest slot no output holds; never the previous frame, motion starts from it.
        std::optional<u32> pick;
        for (u32 i = 0; i < NUM_SLOTS; ++i) {
            const Slot& slot = slots[i];
            if (slot.pins != 0 || (last_slot && *last_slot == i && continuity)) {
                continue;
            }
            if (!slot.valid) {
                pick = i;
                break;
            }
            if (!pick || slot.serial < slots[*pick].serial) {
                pick = i;
            }
        }
        if (!pick) {
            return std::nullopt;
        }
        index = *pick;
        Slot& slot = slots[index];
        slot.valid = false;
        last_read = slot.last_read;
        if (continuity && last_slot && slots[*last_slot].valid) {
            prev = last_slot;
        }
        if (!continuity) {
            ++next_serial; // no motion across the gap
        }
        slot.serial = next_serial++;
        slot.has_motion = prev.has_value();
    }
    // The outputs that read it are long done: it is the oldest the pacer let go.
    WaitForPresentFence(last_read);

    Slot& slot = slots[index];
    CommandStream* cmd = scheduler.CommandList();
    Transition(cmd, slot.color, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(slot.color.resource.Get(), image);
    Transition(cmd, slot.color, COLOR_READ_STATE);

    // The luma pyramid; level 0 also clears the slot's statistics word.
    for (u32 level = 0; level < NUM_LEVELS; ++level) {
        Texture& dest = slot.luma[level];
        const Texture* source = level == 0 ? &slot.color : &slot.luma[level - 1];
        const std::array<u32, CONSTANT_WORDS> constants{
            dest.width, dest.height, level == 0 ? 1U : 0U, index,
            level == 0 && luma_scale == 4 ? 1U : 0U, 0, 0, 0,
        };
        Dispatch(cmd, luma_pipeline.Get(), constants, {source, nullptr, nullptr, nullptr, nullptr},
                 dest, Common::DivCeil(dest.width, GROUP_SIZE),
                 Common::DivCeil(dest.height, GROUP_SIZE));
    }

    if (prev) {
        Slot& previous = slots[*prev];
        const bool temporal = previous.has_motion;
        const auto stats_barrier = [&] {
            const D3D12_RESOURCE_BARRIER barrier = UavBarrier(stats.Get());
            scheduler.CommandList()->ResourceBarrier(1, &barrier);
        };
        for (u32 step = 0; step < NUM_LEVELS; ++step) {
            const u32 level = NUM_LEVELS - 1 - step;
            Texture& dest = level == 0 ? slot.motion : scratch_motion[level];
            const Texture* coarse = level + 1 < NUM_LEVELS ? &scratch_motion[level + 1] : nullptr;
            // The previous pair's slot rides above the flags (its global vector).
            u32 flags = (coarse ? HAS_COARSE | HAS_GLOBAL : FULL_SEARCH) |
                        (temporal ? HAS_TEMPORAL | (*prev << 8) : 0U);
            if (level == 0) {
                flags |= FINEST;
            }
            std::array<u32, CONSTANT_WORDS> constants{
                slot.luma[level].width,
                slot.luma[level].height,
                dest.width,
                dest.height,
                flags,
                index,
                FloatBits(1.0f / static_cast<float>(1U << level)),
                FloatBits(MOTION_LAMBDA),
            };
            const std::array<const Texture*, NUM_SOURCES> sources{
                &previous.luma[level], &slot.luma[level], coarse,
                temporal ? &previous.motion : nullptr, nullptr};
            if (!coarse) {
                // The wide search runs a workgroup per block and adds to the global motion words
                // the luma pass cleared; one workgroup then picks the frame's vector.
                stats_barrier();
                Dispatch(cmd, motion_pipeline.Get(), constants, sources, dest, dest.width,
                         dest.height);
                stats_barrier();
                // With the previous pair's slot: its global vector counts as a near one.
                constants[4] = flags | GLOBAL_PICK;
                Dispatch(cmd, motion_pipeline.Get(), constants, sources, dest, 1, 1);
                stats_barrier();
                continue;
            }
            Dispatch(cmd, motion_pipeline.Get(), constants, sources, dest,
                     Common::DivCeil(dest.width, GROUP_SIZE),
                     Common::DivCeil(dest.height, GROUP_SIZE));
        }
    }
    return index;
}

void FrameGen::Commit(u32 index, u64 tick, u64 vsync_index) {
    std::scoped_lock lock{mutex};
    Slot& slot = slots[index];
    slot.tick = tick;
    slot.vsync = vsync_index;
    slot.content = frame_content;
    slot.valid = true;
    last_slot = index;
    continuity = true;
}

void FrameGen::Break() {
    std::scoped_lock lock{mutex};
    continuity = false;
    for (Slot& slot : slots) {
        slot.valid = false;
    }
    pacer.Reset();
    clock_valid = false;
}

bool FrameGen::HasFrames() {
    std::scoped_lock lock{mutex};
    if (target == 0) {
        return false;
    }
    return std::ranges::any_of(slots, [](const Slot& slot) { return slot.valid; });
}

u32 FrameGen::Target() {
    std::scoped_lock lock{mutex};
    return target;
}

FrameGen::Output FrameGen::Next(double now) {
    std::scoped_lock lock{mutex};
    Output output_choice;
    if (target == 0) {
        return output_choice;
    }
    // Display slots are evenly spaced; the swapchain's wake-ups only correct the clock when
    // they drift by more than half a slot.
    const double period = 1.0 / static_cast<double>(target);
    if (clock_valid && std::abs(now - (slot_clock + period)) < 0.5 * period) {
        slot_clock += period;
    } else {
        slot_clock = now;
        clock_valid = true;
    }

    // Only frames whose ingest the GPU has finished: the present queue would wait for the others
    // behind the game's rendering and miss its vsync. With one queue nothing waits.
    std::array<FrameGenPacer::Frame, NUM_SLOTS> frames{};
    u32 count = 0;
    std::optional<u32> newest_valid;
    for (u32 i = 0; i < NUM_SLOTS; ++i) {
        const Slot& slot = slots[i];
        if (!slot.valid) {
            continue;
        }
        if (!newest_valid || slot.content > slots[*newest_valid].content) {
            newest_valid = i;
        }
        if (!device.HasPresentQueue() || scheduler.IsFree(slot.tick)) {
            frames[count++] = {.content = slot.content, .serial = slot.serial, .slot = i};
        }
    }
    if (count == 0 && newest_valid) {
        // None finished yet (the first frame): the newest, waited for.
        frames[count++] = {.content = slots[*newest_valid].content,
                           .serial = slots[*newest_valid].serial,
                           .slot = *newest_valid};
    }
    std::sort(frames.begin(), frames.begin() + count,
              [](const auto& lhs, const auto& rhs) { return lhs.content < rhs.content; });
    FrameGenPacer::Choice choice =
        pacer.Pick(slot_clock, std::span<const FrameGenPacer::Frame>(frames.data(), count));
    if (choice.kind == FrameGenPacer::Choice::Kind::None && count > 0) {
        // The swapchain took a slot: it gets the newest frame rather than a stale back buffer.
        choice.kind = FrameGenPacer::Choice::Kind::Real;
        choice.a = count - 1;
    }
    output_choice.kind = choice.kind;
    output_choice.idle = choice.idle;
    output_choice.sync_interval = target == 30 ? 2 : 1;
    if (choice.kind == FrameGenPacer::Choice::Kind::None) {
        return output_choice;
    }
    output_choice.a = frames[choice.a].slot;
    ++slots[output_choice.a].pins;
    output_choice.tick = slots[output_choice.a].tick;
    if (choice.kind == FrameGenPacer::Choice::Kind::Blend) {
        output_choice.b = frames[choice.b].slot;
        output_choice.phase = choice.phase;
        ++slots[output_choice.b].pins;
        output_choice.tick = std::max(output_choice.tick, slots[output_choice.b].tick);
    }
    return output_choice;
}

void FrameGen::RecordOutput(ID3D12GraphicsCommandList* list, const Output& out,
                            ID3D12Resource* back_buffer) {
    using Kind = FrameGenPacer::Choice::Kind;
    if (out.kind == Kind::Real) {
        list->CopyResource(back_buffer, slots[out.a].color.resource.Get());
        return;
    }
    if (out.kind != Kind::Blend) {
        return;
    }
    const Texture& luma = slots[out.a].luma[0];
    const Texture& motion = slots[out.b].motion;
    const std::array<u32, CONSTANT_WORDS> constants{
        width,
        height,
        luma.width,
        luma.height,
        FloatBits(out.phase),
        out.b,
        FloatBits(CUT_THRESHOLD),
        motion.width * motion.height,
    };
    D3D12_GPU_DESCRIPTOR_HANDLE table = table_heap->GetGPUDescriptorHandleForHeapStart();
    table.ptr += static_cast<UINT64>(TableIndex(out.a, out.b)) * descriptor_size;
    ID3D12DescriptorHeap* const heaps[] = {table_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetPipelineState(compose_pipeline.Get());
    list->SetComputeRoot32BitConstants(CONSTANTS_PARAM, CONSTANT_WORDS, constants.data(), 0);
    list->SetComputeRootDescriptorTable(TABLE_PARAM, table);
    list->SetComputeRootUnorderedAccessView(STATS_PARAM, stats->GetGPUVirtualAddress());
    const u32 groups_x = Common::DivCeil(luma.width, GROUP_SIZE);
    const u32 groups_y = Common::DivCeil(luma.height, GROUP_SIZE);
    SetRuntimeData(list, groups_x, groups_y);
    list->Dispatch(groups_x, groups_y, 1);
    TransitionResource(list, output.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(back_buffer, output.resource.Get());
    TransitionResource(list, output.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void FrameGen::Unpin(const Output& out, u64 fence_value) {
    using Kind = FrameGenPacer::Choice::Kind;
    if (out.kind == Kind::None) {
        return;
    }
    std::scoped_lock lock{mutex};
    const auto release = [&](u32 index) {
        Slot& slot = slots[index];
        slot.pins = slot.pins > 0 ? slot.pins - 1 : 0;
        slot.last_read = std::max(slot.last_read, fence_value);
    };
    release(out.a);
    if (out.kind == Kind::Blend) {
        release(out.b);
    }
}

} // namespace D3D12
