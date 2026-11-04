// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <exception>
#include <span>

#include "common/logging.h"
#include "video_core/host_shaders/blit_color_float_frag_spv.h"
#include "video_core/host_shaders/blit_depth_frag_spv.h"
#include "video_core/host_shaders/d3d12_blit_ms_average_frag_spv.h"
#include "video_core/host_shaders/d3d12_blit_ms_depth_frag_spv.h"
#include "video_core/host_shaders/d3d12_blit_ms_sample_frag_spv.h"
#include "video_core/host_shaders/d3d12_blit_ms_sint_frag_spv.h"
#include "video_core/host_shaders/d3d12_blit_ms_stencil_bit_frag_spv.h"
#include "video_core/host_shaders/d3d12_blit_ms_uint_frag_spv.h"
#include "video_core/host_shaders/d3d12_blit_sint_frag_spv.h"
#include "video_core/host_shaders/d3d12_blit_stencil_bit_frag_spv.h"
#include "video_core/host_shaders/d3d12_blit_uint_frag_spv.h"
#include "video_core/host_shaders/d3d12_clear_sint_frag_spv.h"
#include "video_core/host_shaders/d3d12_clear_uint_frag_spv.h"
#include "video_core/host_shaders/d3d12_flat_to_msaa_depth_frag_spv.h"
#include "video_core/host_shaders/d3d12_flat_to_msaa_frag_spv.h"
#include "video_core/host_shaders/d3d12_flat_to_msaa_sint_frag_spv.h"
#include "video_core/host_shaders/d3d12_flat_to_msaa_stencil_bit_frag_spv.h"
#include "video_core/host_shaders/d3d12_flat_to_msaa_uint_frag_spv.h"
#include "video_core/host_shaders/d3d12_msaa_to_flat_depth_frag_spv.h"
#include "video_core/host_shaders/d3d12_msaa_to_flat_frag_spv.h"
#include "video_core/host_shaders/d3d12_msaa_to_flat_sint_frag_spv.h"
#include "video_core/host_shaders/d3d12_msaa_to_flat_stencil_bit_frag_spv.h"
#include "video_core/host_shaders/d3d12_msaa_to_flat_uint_frag_spv.h"
#include "video_core/host_shaders/full_screen_triangle_vert_spv.h"
#include "video_core/host_shaders/vulkan_color_clear_frag_spv.h"
#include "video_core/host_shaders/vulkan_color_clear_vert_spv.h"
#include "video_core/host_shaders/vulkan_depthstencil_clear_frag_spv.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"

namespace D3D12 {

namespace {

/// Root parameters (one signature for every helper pipeline): the push-constant words the helper
/// shaders use (four, a fifth for the stencil bit, a sixth for the source's samples, a seventh for
/// the target's),
/// spirv_to_dxil's vertex runtime data, then one SRV and one sampler table. The root constants
/// cover two whole 16-byte registers.
constexpr u32 PUSH_CONSTANTS_PARAM = 0;
constexpr u32 SRV_TABLE_PARAM = 2;
constexpr u32 SAMPLER_TABLE_PARAM = 3;
constexpr u32 PUSH_CONSTANT_WORDS = 4;
constexpr u32 STENCIL_BIT_WORD = 4;
constexpr u32 SAMPLE_COUNT_WORD = 5;
/// The multisample copies' stencil bit, after their three ivec2.
constexpr u32 MSAA_COPY_BIT_WORD = 6;
constexpr u32 ROOT_CONSTANT_WORDS = 8;
constexpr u32 RUNTIME_DATA_WORDS = 12;

/// Keys of the helper's samplers in SamplerHeap: guest samplers count up from 1 and the
/// presenter's linear sampler is 0, so these never meet them.
constexpr u64 NEAREST_SAMPLER_KEY = ~0ULL;

constexpr u64 LINEAR_SAMPLER_KEY = ~0ULL - 1;

D3D12_DEPTH_STENCILOP_DESC ReplaceStencil() {
    return {
        .StencilFailOp = D3D12_STENCIL_OP_KEEP,
        .StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
        .StencilPassOp = D3D12_STENCIL_OP_REPLACE,
        .StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS,
    };
}

/// As Vulkan's BindBlitState: the destination is the region's bounding box (a mirrored
/// destination is not mirrored).
D3D12_RECT BlitRect(const VideoCommon::Region2D& region) {
    return {
        .left = std::min(region.start.x, region.end.x),
        .top = std::min(region.start.y, region.end.y),
        .right = std::max(region.start.x, region.end.x),
        .bottom = std::max(region.start.y, region.end.y),
    };
}

} // Anonymous namespace

BlitImageHelper::BlitImageHelper(const Device& device_, Scheduler& scheduler_,
                                 const ShaderCompiler& compiler, DescriptorRing& descriptor_ring_,
                                 SamplerHeap& sampler_heap_,
                                 CpuDescriptorAllocator& sampler_descriptors)
    : device{device_}, scheduler{scheduler_}, descriptor_ring{descriptor_ring_},
      sampler_heap{sampler_heap_} {
    if (!compiler.IsAvailable()) {
        LOG_WARNING(Render, "D3D12: no shader path, texture blits and masked clears are skipped");
        return;
    }
    // Linked like guest pipelines, so the vertex outputs match the pixel inputs.
    const auto compile = [&compiler](std::span<const u32> vs, std::span<const u32> ps) {
        const std::array<ShaderCompiler::PipelineStage, 2> stages{{
            {vs, DXIL_SPIRV_SHADER_VERTEX},
            {ps, DXIL_SPIRV_SHADER_FRAGMENT},
        }};
        auto compiled = compiler.CompilePipeline(stages, {});
        return Program{std::move(compiled[0].dxil), std::move(compiled[1].dxil)};
    };
    try {
        programs[static_cast<size_t>(Kind::BlitColor)] =
            compile(FULL_SCREEN_TRIANGLE_VERT_SPV, BLIT_COLOR_FLOAT_FRAG_SPV);
        programs[static_cast<size_t>(Kind::BlitDepth)] =
            compile(FULL_SCREEN_TRIANGLE_VERT_SPV, BLIT_DEPTH_FRAG_SPV);
        programs[static_cast<size_t>(Kind::ClearColor)] =
            compile(VULKAN_COLOR_CLEAR_VERT_SPV, VULKAN_COLOR_CLEAR_FRAG_SPV);
        programs[static_cast<size_t>(Kind::ClearDepthStencil)] =
            compile(VULKAN_COLOR_CLEAR_VERT_SPV, VULKAN_DEPTHSTENCIL_CLEAR_FRAG_SPV);
        programs[static_cast<size_t>(Kind::BlitStencilBit)] =
            compile(FULL_SCREEN_TRIANGLE_VERT_SPV, D3D12_BLIT_STENCIL_BIT_FRAG_SPV);
        CreateRootSignature();
        CreateSamplers(sampler_descriptors);
        available = true;
        LOG_INFO(Render, "D3D12: blit and clear helpers ready ({} programs)", programs.size());
    } catch (const std::exception& exception) {
        LOG_ERROR(Render, "D3D12: blit helpers unavailable: {}", exception.what());
    }
    // Apart from the core helpers: without these, only multisampled blits and exact integer clears
    // are lost.
    try {
        if (available) {
            const auto blit = [&](Kind kind, std::span<const u32> ps) {
                programs[static_cast<size_t>(kind)] = compile(FULL_SCREEN_TRIANGLE_VERT_SPV, ps);
            };
            blit(Kind::BlitColorMsAverage, D3D12_BLIT_MS_AVERAGE_FRAG_SPV);
            blit(Kind::BlitColorMsSample, D3D12_BLIT_MS_SAMPLE_FRAG_SPV);
            blit(Kind::BlitUint, D3D12_BLIT_UINT_FRAG_SPV);
            blit(Kind::BlitSint, D3D12_BLIT_SINT_FRAG_SPV);
            blit(Kind::BlitUintMs, D3D12_BLIT_MS_UINT_FRAG_SPV);
            blit(Kind::BlitSintMs, D3D12_BLIT_MS_SINT_FRAG_SPV);
            blit(Kind::BlitDepthMs, D3D12_BLIT_MS_DEPTH_FRAG_SPV);
            blit(Kind::BlitStencilBitMs, D3D12_BLIT_MS_STENCIL_BIT_FRAG_SPV);
            blit(Kind::MsaaToFlat, D3D12_MSAA_TO_FLAT_FRAG_SPV);
            blit(Kind::MsaaToFlatUint, D3D12_MSAA_TO_FLAT_UINT_FRAG_SPV);
            blit(Kind::MsaaToFlatSint, D3D12_MSAA_TO_FLAT_SINT_FRAG_SPV);
            blit(Kind::MsaaToFlatDepth, D3D12_MSAA_TO_FLAT_DEPTH_FRAG_SPV);
            blit(Kind::MsaaToFlatStencilBit, D3D12_MSAA_TO_FLAT_STENCIL_BIT_FRAG_SPV);
            blit(Kind::FlatToMsaa, D3D12_FLAT_TO_MSAA_FRAG_SPV);
            blit(Kind::FlatToMsaaUint, D3D12_FLAT_TO_MSAA_UINT_FRAG_SPV);
            blit(Kind::FlatToMsaaSint, D3D12_FLAT_TO_MSAA_SINT_FRAG_SPV);
            blit(Kind::FlatToMsaaDepth, D3D12_FLAT_TO_MSAA_DEPTH_FRAG_SPV);
            blit(Kind::FlatToMsaaStencilBit, D3D12_FLAT_TO_MSAA_STENCIL_BIT_FRAG_SPV);
            multisample_available = true;
            programs[static_cast<size_t>(Kind::ClearUint)] =
                compile(VULKAN_COLOR_CLEAR_VERT_SPV, D3D12_CLEAR_UINT_FRAG_SPV);
            programs[static_cast<size_t>(Kind::ClearSint)] =
                compile(VULKAN_COLOR_CLEAR_VERT_SPV, D3D12_CLEAR_SINT_FRAG_SPV);
            integer_clear_available = true;
            LOG_INFO(Render, "D3D12: multisampled blit and integer clear helpers ready");
        }
    } catch (const std::exception& exception) {
        LOG_ERROR(Render, "D3D12: multisampled blit or integer clear helpers unavailable: {}",
                  exception.what());
    }
    try {
        CreatePackPipelines(compiler);
        pack_available = true;
        LOG_INFO(Render, "D3D12: depth-stencil pack shaders ready");
    } catch (const std::exception& exception) {
        LOG_ERROR(Render, "D3D12: depth-stencil pack shaders unavailable, depth-stencil "
                  "contents will not be transferred: {}", exception.what());
    }
    try {
        CreateAstcPipeline(compiler);
        astc_available = true;
        LOG_INFO(Render, "D3D12: ASTC decoder shader ready");
    } catch (const std::exception& exception) {
        LOG_ERROR(Render, "D3D12: ASTC decoder shader unavailable, ASTC is decoded on the CPU: {}",
                  exception.what());
    }
    try {
        CreateBcnPipeline(compiler);
        bcn_available = true;
        LOG_INFO(Render, "D3D12: BCn decoder shader ready");
    } catch (const std::exception& exception) {
        LOG_ERROR(Render, "D3D12: BCn decoder shader unavailable, BC arrays are decoded on the "
                  "CPU: {}", exception.what());
    }
    try {
        CreateBc3Pipeline(compiler);
        bc3_available = true;
        LOG_INFO(Render, "D3D12: BC3 GPU encoder shader ready");
    } catch (const std::exception& exception) {
        LOG_ERROR(Render, "D3D12: BC3 GPU encoder unavailable, recompressed ASTC uses the CPU: {}",
                  exception.what());
    }
    if (pack_available) {
        try {
            CreateUnswizzlePipeline(compiler);
            unswizzle_available = true;
            LOG_INFO(Render, "D3D12: block-linear unswizzle shader ready");
        } catch (const std::exception& exception) {
            LOG_ERROR(Render, "D3D12: block-linear unswizzle shader unavailable, uploads are "
                      "unswizzled on the CPU: {}", exception.what());
        }
    }
}

BlitImageHelper::~BlitImageHelper() = default;

void BlitImageHelper::CreateRootSignature() {
    const D3D12_DESCRIPTOR_RANGE srv_range{
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 1,
        .BaseShaderRegister = 0,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = 0,
    };
    const D3D12_DESCRIPTOR_RANGE sampler_range{
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
        .NumDescriptors = 1,
        .BaseShaderRegister = 0,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = 0,
    };
    std::array<D3D12_ROOT_PARAMETER, 4> params{};
    params[PUSH_CONSTANTS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[PUSH_CONSTANTS_PARAM].Constants = {.ShaderRegister = 0,
                                              .RegisterSpace = PUSH_CONSTANT_SPACE,
                                              .Num32BitValues = ROOT_CONSTANT_WORDS};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {.ShaderRegister = 0, .RegisterSpace = RUNTIME_DATA_SPACE,
                           .Num32BitValues = RUNTIME_DATA_WORDS};
    params[SRV_TABLE_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[SRV_TABLE_PARAM].DescriptorTable = {.NumDescriptorRanges = 1,
                                               .pDescriptorRanges = &srv_range};
    params[SAMPLER_TABLE_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[SAMPLER_TABLE_PARAM].DescriptorTable = {.NumDescriptorRanges = 1,
                                                   .pDescriptorRanges = &sampler_range};
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
    root_signature = D3D12::CreateRootSignature(device.Get(), desc, "blit helpers root signature");
}

void BlitImageHelper::CreateSamplers(CpuDescriptorAllocator& sampler_descriptors) {
    const auto create = [&](D3D12_FILTER filter, u64 key) {
        const D3D12_SAMPLER_DESC desc{
            .Filter = filter,
            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .MipLODBias = 0.0f,
            .MaxAnisotropy = 1,
            .ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
            .BorderColor = {},
            .MinLOD = 0.0f,
            .MaxLOD = 0.0f,
        };
        const D3D12_CPU_DESCRIPTOR_HANDLE handle = sampler_descriptors.Allocate();
        device.Get()->CreateSampler(&desc, handle);
        return Sampling{handle, key};
    };
    nearest_sampler = create(D3D12_FILTER_MIN_MAG_MIP_POINT, NEAREST_SAMPLER_KEY);
    linear_sampler = create(D3D12_FILTER_MIN_MAG_MIP_LINEAR, LINEAR_SAMPLER_KEY);
}

ID3D12PipelineState* BlitImageHelper::Pipeline(Kind kind, const Target& target, u8 mask,
                                               bool depth) {
    const u64 key = static_cast<u64>(kind) | (static_cast<u64>(target.format) << 8) |
                    (static_cast<u64>(mask) << 40) | (static_cast<u64>(depth) << 48) |
                    (static_cast<u64>(target.samples & 0xff) << 52);
    if (const auto it = pipelines.find(key); it != pipelines.end()) {
        return it->second.Get();
    }
    Kind program_kind = kind;
    switch (kind) {
    case Kind::BlitDepthStencil:
        program_kind = Kind::BlitDepth;
        break;
    case Kind::BlitDepthStencilMs:
        program_kind = Kind::BlitDepthMs;
        break;
    case Kind::MsaaToFlatDepthStencil:
        program_kind = Kind::MsaaToFlatDepth;
        break;
    case Kind::FlatToMsaaDepthStencil:
        program_kind = Kind::FlatToMsaaDepth;
        break;
    default:
        break;
    }
    const Program& program = programs[static_cast<size_t>(program_kind)];
    if (program.ps.empty()) {
        return nullptr;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature.Get();
    desc.VS = {program.vs.data(), program.vs.size()};
    desc.PS = {program.ps.data(), program.ps.size()};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.SampleDesc = {.Count = std::max(1U, target.samples), .Quality = 0};
    switch (kind) {
    case Kind::BlitColor:
    case Kind::BlitColorMsAverage:
    case Kind::BlitColorMsSample:
    case Kind::BlitUint:
    case Kind::BlitSint:
    case Kind::BlitUintMs:
    case Kind::BlitSintMs:
    case Kind::MsaaToFlat:
    case Kind::MsaaToFlatUint:
    case Kind::MsaaToFlatSint:
    case Kind::FlatToMsaa:
    case Kind::FlatToMsaaUint:
    case Kind::FlatToMsaaSint:
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = target.format;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        break;
    case Kind::ClearColor:
    case Kind::ClearUint:
    case Kind::ClearSint:
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = target.format;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = mask;
        break;
    case Kind::BlitDepth:
    case Kind::BlitDepthMs:
    case Kind::MsaaToFlatDepth:
    case Kind::FlatToMsaaDepth:
        desc.DSVFormat = target.format;
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        break;
    case Kind::BlitDepthStencil:
    case Kind::BlitDepthStencilMs:
    case Kind::BlitStencilBit:
    case Kind::BlitStencilBitMs:
    case Kind::MsaaToFlatDepthStencil:
    case Kind::FlatToMsaaDepthStencil:
    case Kind::MsaaToFlatStencilBit:
    case Kind::FlatToMsaaStencilBit:
    case Kind::ClearDepthStencil:
        // Depth when asked (the depth-stencil blit and depth clears), the stencil bits of mask
        // from the reference.
        desc.DSVFormat = target.format;
        desc.DepthStencilState = {
            .DepthEnable = depth,
            .DepthWriteMask = depth ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO,
            .DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS,
            .StencilEnable = TRUE,
            .StencilReadMask = 0xFF,
            .StencilWriteMask = mask,
            .FrontFace = ReplaceStencil(),
            .BackFace = ReplaceStencil(),
        };
        break;
    }
    ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT hr = device.Get()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
    if (FAILED(hr)) {
        LOG_ERROR(Render,
                  "D3D12: blit helper pipeline {} for format {} rejected (HRESULT 0x{:08X})",
                  static_cast<u32>(kind), static_cast<u32>(target.format), static_cast<u32>(hr));
    } else {
        LOG_INFO(Render, "D3D12: blit helper pipeline {} built for format {} ({} samples)",
                 static_cast<u32>(kind), static_cast<u32>(target.format), desc.SampleDesc.Count);
    }
    // A rejected pipeline stays cached as null, so it is reported once.
    return pipelines.emplace(key, std::move(pipeline)).first->second.Get();
}

void BlitImageHelper::Begin(ID3D12PipelineState* pipeline, const Target& target, bool is_depth,
                            const D3D12_RECT& rect) {
    CommandStream* const cmd = scheduler.CommandList();
    ID3D12DescriptorHeap* const heaps[] = {descriptor_ring.Heap(), sampler_heap.Heap()};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetGraphicsRootSignature(root_signature.Get());
    scheduler.SetPipelineState(pipeline);
    if (is_depth) {
        cmd->OMSetRenderTargets(0, nullptr, FALSE, &target.view);
    } else {
        cmd->OMSetRenderTargets(1, &target.view, FALSE, nullptr);
    }
    const D3D12_VIEWPORT viewport{
        .TopLeftX = static_cast<f32>(rect.left),
        .TopLeftY = static_cast<f32>(rect.top),
        .Width = static_cast<f32>(rect.right - rect.left),
        .Height = static_cast<f32>(rect.bottom - rect.top),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &rect);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

bool BlitImageHelper::Blit(Kind kind, const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv,
                           const Sampling& sampler, const VideoCommon::Region2D& dst_region,
                           const VideoCommon::Region2D& src_region,
                           const VideoCommon::Extent2D& src_size, u32 src_samples) {
    if (!available || src_size.width == 0 || src_size.height == 0) {
        return false;
    }
    const D3D12_RECT rect = BlitRect(dst_region);
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return false;
    }
    ID3D12PipelineState* const pipeline = Pipeline(kind, dst, 0, false);
    if (!pipeline) {
        return false;
    }
    // Descriptors first: a full ring or sampler heap flushes the command list.
    const D3D12_GPU_DESCRIPTOR_HANDLE srv_table = descriptor_ring.Upload({&src_srv, 1});
    const D3D12_GPU_DESCRIPTOR_HANDLE sampler_table =
        sampler_heap.GetTable({&sampler.key, 1}, {&sampler.handle, 1});
    const bool is_depth = kind == Kind::BlitDepth || kind == Kind::BlitDepthMs;
    BeginBlit(pipeline, dst, is_depth, rect, srv_table, sampler_table, src_region, src_size);
    if (kind == Kind::BlitColorMsAverage) {
        scheduler.CommandList()->SetGraphicsRoot32BitConstants(PUSH_CONSTANTS_PARAM, 1,
                                                               &src_samples, SAMPLE_COUNT_WORD);
    } else if (kind == Kind::BlitUintMs || kind == Kind::BlitSintMs || kind == Kind::BlitDepthMs) {
        // A linear blit averages a multisampled depth source; integers always average.
        SetSampleCounts(src_samples, dst.samples, &sampler == &linear_sampler);
    }
    scheduler.CommandList()->DrawInstanced(3, 1, 0, 0);
    return true;
}

bool BlitImageHelper::BlitDepthStencil(const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE depth_srv,
                                       D3D12_CPU_DESCRIPTOR_HANDLE stencil_srv,
                                       const VideoCommon::Region2D& dst_region,
                                       const VideoCommon::Region2D& src_region,
                                       const VideoCommon::Extent2D& src_size,
                                       u32 src_samples, bool linear) {
    const bool src_multisampled = src_samples > 1;
    if (!available || src_size.width == 0 || src_size.height == 0) {
        return false;
    }
    const D3D12_RECT rect = BlitRect(dst_region);
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return false;
    }
    ID3D12PipelineState* const depth_pipeline = Pipeline(
        src_multisampled ? Kind::BlitDepthStencilMs : Kind::BlitDepthStencil, dst, 0xFF, true);
    if (!depth_pipeline) {
        return false;
    }
    std::array<ID3D12PipelineState*, 8> bit_pipelines{};
    for (u32 bit = 0; bit < bit_pipelines.size(); ++bit) {
        bit_pipelines[bit] =
            Pipeline(src_multisampled ? Kind::BlitStencilBitMs : Kind::BlitStencilBit, dst,
                     static_cast<u8>(1U << bit), false);
        if (!bit_pipelines[bit]) {
            return false;
        }
    }
    // Both tables in one upload (a second could flush the list between them): depth, stencil.
    const std::array srvs{depth_srv, stencil_srv};
    const D3D12_GPU_DESCRIPTOR_HANDLE depth_table = descriptor_ring.Upload(srvs);
    const D3D12_GPU_DESCRIPTOR_HANDLE stencil_table{depth_table.ptr + descriptor_ring.Stride()};
    // Depth filtered as the guest asks; the stencil passes fetch texels, whatever the sampler.
    const Sampling& depth_sampler = linear ? linear_sampler : nearest_sampler;
    const D3D12_GPU_DESCRIPTOR_HANDLE sampler_table =
        sampler_heap.GetTable({&depth_sampler.key, 1}, {&depth_sampler.handle, 1});

    BeginBlit(depth_pipeline, dst, true, rect, depth_table, sampler_table, src_region, src_size);
    CommandStream* const cmd = scheduler.CommandList();
    if (src_multisampled) {
        // The stencil passes keep them: they set only the bit word.
        SetSampleCounts(src_samples, dst.samples, linear);
    }
    cmd->OMSetStencilRef(0);
    cmd->DrawInstanced(3, 1, 0, 0);
    // Same target, viewport and mapping: only the PSO (its write mask) and the bit change.
    cmd->SetGraphicsRootDescriptorTable(SRV_TABLE_PARAM, stencil_table);
    cmd->OMSetStencilRef(0xFF);
    for (u32 bit = 0; bit < bit_pipelines.size(); ++bit) {
        scheduler.SetPipelineState(bit_pipelines[bit]);
        const u32 bit_mask = 1U << bit;
        cmd->SetGraphicsRoot32BitConstants(PUSH_CONSTANTS_PARAM, 1, &bit_mask, STENCIL_BIT_WORD);
        cmd->DrawInstanced(3, 1, 0, 0);
    }
    return true;
}

bool BlitImageHelper::CopyMultisample(const Target& dst, bool to_flat, MsaaCopy type,
                                      D3D12_CPU_DESCRIPTOR_HANDLE src_srv,
                                      D3D12_CPU_DESCRIPTOR_HANDLE stencil_srv,
                                      const D3D12_RECT& dst_rect, s32 src_offset_x,
                                      s32 src_offset_y, s32 scale_x, s32 scale_y) {
    if (!multisample_available || scale_x <= 0 || scale_y <= 0) {
        return false;
    }
    if (dst_rect.right <= dst_rect.left || dst_rect.bottom <= dst_rect.top) {
        return true;
    }
    const bool depth = type == MsaaCopy::Depth || type == MsaaCopy::DepthStencil;
    const bool stencil = type == MsaaCopy::DepthStencil;
    if (stencil && stencil_srv.ptr == 0) {
        return false;
    }
    Kind kind = Kind::MsaaToFlat;
    switch (type) {
    case MsaaCopy::Float:
        kind = to_flat ? Kind::MsaaToFlat : Kind::FlatToMsaa;
        break;
    case MsaaCopy::Uint:
        kind = to_flat ? Kind::MsaaToFlatUint : Kind::FlatToMsaaUint;
        break;
    case MsaaCopy::Sint:
        kind = to_flat ? Kind::MsaaToFlatSint : Kind::FlatToMsaaSint;
        break;
    case MsaaCopy::Depth:
        kind = to_flat ? Kind::MsaaToFlatDepth : Kind::FlatToMsaaDepth;
        break;
    case MsaaCopy::DepthStencil:
        kind = to_flat ? Kind::MsaaToFlatDepthStencil : Kind::FlatToMsaaDepthStencil;
        break;
    }
    ID3D12PipelineState* const pipeline =
        Pipeline(kind, dst, stencil ? u8{0xFF} : u8{0}, stencil);
    if (!pipeline) {
        return false;
    }
    std::array<ID3D12PipelineState*, 8> bit_pipelines{};
    if (stencil) {
        const Kind bit_kind = to_flat ? Kind::MsaaToFlatStencilBit : Kind::FlatToMsaaStencilBit;
        for (u32 bit = 0; bit < bit_pipelines.size(); ++bit) {
            bit_pipelines[bit] = Pipeline(bit_kind, dst, static_cast<u8>(1U << bit), false);
            if (!bit_pipelines[bit]) {
                return false;
            }
        }
    }
    // Descriptors first, both in one upload: a full ring or sampler heap flushes the list.
    const std::array srvs{src_srv, stencil ? stencil_srv : src_srv};
    const D3D12_GPU_DESCRIPTOR_HANDLE src_table =
        descriptor_ring.Upload(std::span{srvs.data(), stencil ? size_t{2} : size_t{1}});
    const D3D12_GPU_DESCRIPTOR_HANDLE stencil_table{src_table.ptr + descriptor_ring.Stride()};
    const D3D12_GPU_DESCRIPTOR_HANDLE sampler_table =
        sampler_heap.GetTable({&nearest_sampler.key, 1}, {&nearest_sampler.handle, 1});

    Begin(pipeline, dst, depth, dst_rect);
    CommandStream* const cmd = scheduler.CommandList();
    // The shaders' push constants: dst_offset, src_offset and scale, an ivec2 each, from word 0.
    const std::array<s32, 6> push{dst_rect.left, dst_rect.top, src_offset_x,
                                  src_offset_y,  scale_x,      scale_y};
    cmd->SetGraphicsRoot32BitConstants(PUSH_CONSTANTS_PARAM, static_cast<u32>(push.size()),
                                       push.data(), 0);
    cmd->SetGraphicsRootDescriptorTable(SRV_TABLE_PARAM, src_table);
    cmd->SetGraphicsRootDescriptorTable(SAMPLER_TABLE_PARAM, sampler_table);
    if (stencil) {
        cmd->OMSetStencilRef(0);
    }
    cmd->DrawInstanced(3, 1, 0, 0);
    if (!stencil) {
        return true;
    }
    // Same target, viewport and offsets: only the PSO (its write mask) and the bit change.
    cmd->SetGraphicsRootDescriptorTable(SRV_TABLE_PARAM, stencil_table);
    cmd->OMSetStencilRef(0xFF);
    for (u32 bit = 0; bit < bit_pipelines.size(); ++bit) {
        scheduler.SetPipelineState(bit_pipelines[bit]);
        const u32 bit_mask = 1U << bit;
        cmd->SetGraphicsRoot32BitConstants(PUSH_CONSTANTS_PARAM, 1, &bit_mask,
                                           MSAA_COPY_BIT_WORD);
        cmd->DrawInstanced(3, 1, 0, 0);
    }
    return true;
}

void BlitImageHelper::SetSampleCounts(u32 src_samples, u32 dst_samples, bool average) {
    // The source's in SAMPLE_COUNT_WORD, then the target's, then whether depth averages.
    const std::array<u32, 3> counts{src_samples, std::max(1U, dst_samples), average ? 1U : 0U};
    scheduler.CommandList()->SetGraphicsRoot32BitConstants(
        PUSH_CONSTANTS_PARAM, static_cast<u32>(counts.size()), counts.data(), SAMPLE_COUNT_WORD);
}

void BlitImageHelper::BeginBlit(ID3D12PipelineState* pipeline, const Target& dst, bool is_depth,
                                const D3D12_RECT& rect, D3D12_GPU_DESCRIPTOR_HANDLE srv_table,
                                D3D12_GPU_DESCRIPTOR_HANDLE sampler_table,
                                const VideoCommon::Region2D& src_region,
                                const VideoCommon::Extent2D& src_size) {
    Begin(pipeline, dst, is_depth, rect);
    CommandStream* const cmd = scheduler.CommandList();
    // A mirrored source is mirrored, through a negative scale.
    // full_screen_triangle.vert computes texcoord = (x, y) / 2 * scale + offset with Vulkan's
    // clip space, where y = 0 is the top of the viewport; D3D12's y = 0 is the bottom, so the
    // vertical mapping is mirrored here: v' = (1 - v), hence scale -sy and offset oy + sy.
    const f32 width = static_cast<f32>(src_size.width);
    const f32 height = static_cast<f32>(src_size.height);
    const f32 scale_x = static_cast<f32>(src_region.end.x - src_region.start.x) / width;
    const f32 scale_y = static_cast<f32>(src_region.end.y - src_region.start.y) / height;
    const f32 offset_x = static_cast<f32>(src_region.start.x) / width;
    const f32 offset_y = static_cast<f32>(src_region.start.y) / height;
    const std::array<f32, PUSH_CONSTANT_WORDS> push{scale_x, -scale_y, offset_x,
                                                    offset_y + scale_y};
    cmd->SetGraphicsRoot32BitConstants(PUSH_CONSTANTS_PARAM, PUSH_CONSTANT_WORDS, push.data(), 0);
    cmd->SetGraphicsRootDescriptorTable(SRV_TABLE_PARAM, srv_table);
    cmd->SetGraphicsRootDescriptorTable(SAMPLER_TABLE_PARAM, sampler_table);
}

bool BlitImageHelper::BlitColor(const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv,
                                const Sampling& sampler, const VideoCommon::Region2D& dst_region,
                                const VideoCommon::Region2D& src_region,
                                const VideoCommon::Extent2D& src_size) {
    return Blit(Kind::BlitColor, dst, src_srv, sampler, dst_region, src_region, src_size);
}

bool BlitImageHelper::BlitColorMultisampled(const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv,
                                            u32 src_samples, bool per_sample,
                                            const VideoCommon::Region2D& dst_region,
                                            const VideoCommon::Region2D& src_region,
                                            const VideoCommon::Extent2D& src_size) {
    return Blit(per_sample ? Kind::BlitColorMsSample : Kind::BlitColorMsAverage, dst, src_srv,
                nearest_sampler, dst_region, src_region, src_size, src_samples);
}

bool BlitImageHelper::BlitInteger(const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv,
                                  bool is_signed, u32 src_samples,
                                  const VideoCommon::Region2D& dst_region,
                                  const VideoCommon::Region2D& src_region,
                                  const VideoCommon::Extent2D& src_size) {
    const bool src_multisampled = src_samples > 1;
    const Kind kind = is_signed ? (src_multisampled ? Kind::BlitSintMs : Kind::BlitSint)
                                : (src_multisampled ? Kind::BlitUintMs : Kind::BlitUint);
    return Blit(kind, dst, src_srv, nearest_sampler, dst_region, src_region, src_size,
                src_samples);
}

bool BlitImageHelper::BlitDepth(const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv,
                                const VideoCommon::Region2D& dst_region,
                                const VideoCommon::Region2D& src_region,
                                const VideoCommon::Extent2D& src_size, u32 src_samples,
                                bool linear) {
    return Blit(src_samples > 1 ? Kind::BlitDepthMs : Kind::BlitDepth, dst, src_srv,
                linear ? linear_sampler : nearest_sampler, dst_region, src_region, src_size,
                src_samples);
}

void BlitImageHelper::ClearColor(const Target& dst, u8 color_mask, const std::array<f32, 4>& color,
                                 const D3D12_RECT& rect) {
    if (!available || rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    // D3D12's write mask bits are red 1, green 2, blue 4, alpha 8: the guest's order.
    ID3D12PipelineState* const pipeline =
        Pipeline(Kind::ClearColor, dst, static_cast<u8>(color_mask & 0xF), false);
    if (!pipeline) {
        return;
    }
    Begin(pipeline, dst, false, rect);
    scheduler.CommandList()->SetGraphicsRoot32BitConstants(PUSH_CONSTANTS_PARAM,
                                                           PUSH_CONSTANT_WORDS, color.data(), 0);
    scheduler.CommandList()->DrawInstanced(3, 1, 0, 0);
}

bool BlitImageHelper::ClearColorInteger(const Target& dst, u8 color_mask,
                                        const std::array<u32, 4>& bits, bool is_signed,
                                        const D3D12_RECT& rect) {
    if (!integer_clear_available) {
        return false;
    }
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return true;
    }
    ID3D12PipelineState* const pipeline =
        Pipeline(is_signed ? Kind::ClearSint : Kind::ClearUint, dst,
                 static_cast<u8>(color_mask & 0xF), false);
    if (!pipeline) {
        return false;
    }
    Begin(pipeline, dst, false, rect);
    scheduler.CommandList()->SetGraphicsRoot32BitConstants(PUSH_CONSTANTS_PARAM,
                                                           PUSH_CONSTANT_WORDS, bits.data(), 0);
    scheduler.CommandList()->DrawInstanced(3, 1, 0, 0);
    return true;
}

void BlitImageHelper::ClearDepthStencil(const Target& dst, bool clear_depth, f32 depth,
                                        u8 stencil_mask, u8 stencil_value,
                                        const D3D12_RECT& rect) {
    if (!available || rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    ID3D12PipelineState* const pipeline =
        Pipeline(Kind::ClearDepthStencil, dst, stencil_mask, clear_depth);
    if (!pipeline) {
        return;
    }
    Begin(pipeline, dst, true, rect);
    CommandStream* const cmd = scheduler.CommandList();
    const std::array<f32, PUSH_CONSTANT_WORDS> push{depth, 0.0f, 0.0f, 0.0f};
    cmd->SetGraphicsRoot32BitConstants(PUSH_CONSTANTS_PARAM, PUSH_CONSTANT_WORDS, push.data(), 0);
    cmd->OMSetStencilRef(stencil_value);
    cmd->DrawInstanced(3, 1, 0, 0);
}

} // namespace D3D12
