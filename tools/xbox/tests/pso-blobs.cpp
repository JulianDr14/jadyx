// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// PSO blob gate: a driver blob stored on disk creates the same PSO again as CachedPSO, a blob the
// driver refuses is deleted, and the stable hash ignores the root signature pointer.
#include "common/logging.h"
#include "video_core/host_shaders/blit_color_float_frag_spv.h"
#include "video_core/host_shaders/full_screen_triangle_vert_spv.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_pso_blob_cache.h"
#include "video_core/renderer_d3d12/d3d12_pso_key.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace D3D12;
int main() try {
    Common::Log::Initialize();
    Device device;
    ShaderCompiler compiler;
    if (!compiler.IsAvailable()) throw std::runtime_error("spirv_to_dxil not available");
    const std::array<ShaderCompiler::PipelineStage, 2> stages{{
        {FULL_SCREEN_TRIANGLE_VERT_SPV, DXIL_SPIRV_SHADER_VERTEX},
        {BLIT_COLOR_FLOAT_FRAG_SPV, DXIL_SPIRV_SHADER_FRAGMENT},
    }};
    auto compiled = compiler.CompilePipeline(stages, {});
    const std::vector<u8> vs = std::move(compiled[0].dxil);
    const std::vector<u8> ps = std::move(compiled[1].dxil);

    // The present blit's root signature.
    const D3D12_DESCRIPTOR_RANGE srv_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    const D3D12_DESCRIPTOR_RANGE sampler_range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, PUSH_CONSTANT_SPACE, 4};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, RUNTIME_DATA_SPACE, 12};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = {1, &srv_range};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable = {1, &sampler_range};
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_ROOT_SIGNATURE_DESC root_desc{4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    auto root = CreateRootSignature(device.Get(), root_desc, "gate root signature");
    auto other_root = CreateRootSignature(device.Get(), root_desc, "gate root signature 2");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root.Get();
    desc.VS = {vs.data(), vs.size()};
    desc.PS = {ps.data(), ps.size()};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;

    // Stable hash: same with another root signature object, different with other state.
    const u64 hash = PsoBlobCache::StableHash(MakeGraphicsPsoKey(desc, nullptr));
    auto other_desc = desc;
    other_desc.pRootSignature = other_root.Get();
    if (PsoBlobCache::StableHash(MakeGraphicsPsoKey(other_desc, nullptr)) != hash)
        throw std::runtime_error("stable hash depends on the root signature pointer");
    other_desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (PsoBlobCache::StableHash(MakeGraphicsPsoKey(other_desc, nullptr)) == hash)
        throw std::runtime_error("stable hash ignores the target format");

    const auto directory = std::filesystem::temp_directory_path() / "jadyx-pso-blob-gate";
    std::filesystem::remove_all(directory);
    PsoBlobCache cache;
    if (!cache.Load(hash).empty()) throw std::runtime_error("blob loaded while closed");
    cache.Open(directory);
    if (!cache.Load(hash).empty()) throw std::runtime_error("blob found in an empty folder");

    ComPtr<ID3D12PipelineState> first;
    ThrowIfFailed(device.Get()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&first)), "PSO");
    cache.Store(hash, first.Get());
    const auto stats_stored = cache.GetStats();
    if (stats_stored.stored != 1 || stats_stored.bytes == 0)
        throw std::runtime_error("the driver gave no blob (GetCachedBlob)");
    const auto file = directory / fmt::format("{:016x}.pso", hash);
    if (!std::filesystem::exists(file)) throw std::runtime_error("blob file missing");

    // Created again from the blob.
    std::vector<u8> blob = cache.Load(hash);
    if (blob.size() != stats_stored.bytes) throw std::runtime_error("blob size changed on disk");
    auto from_blob = desc;
    from_blob.CachedPSO = {blob.data(), blob.size()};
    ComPtr<ID3D12PipelineState> second;
    ThrowIfFailed(device.Get()->CreateGraphicsPipelineState(&from_blob, IID_PPV_ARGS(&second)),
                  "PSO from the stored blob");

    // A blob under another name is not taken.
    std::filesystem::copy_file(file, directory / fmt::format("{:016x}.pso", hash + 1));
    if (!cache.Load(hash + 1).empty()) throw std::runtime_error("renamed blob taken");

    // A damaged blob: the driver refuses it, the cache deletes it.
    {
        std::fstream damaged{file, std::ios::in | std::ios::out | std::ios::binary};
        damaged.seekp(24 + 8);
        const char junk[64]{1, 2, 3, 4, 5, 6, 7, 8};
        damaged.write(junk, sizeof(junk));
    }
    blob = cache.Load(hash);
    from_blob.CachedPSO = {blob.data(), blob.size()};
    ComPtr<ID3D12PipelineState> third;
    const HRESULT damaged_hr =
        device.Get()->CreateGraphicsPipelineState(&from_blob, IID_PPV_ARGS(&third));
    if (SUCCEEDED(damaged_hr)) throw std::runtime_error("damaged blob accepted");
    cache.Reject(hash);
    if (std::filesystem::exists(file)) throw std::runtime_error("refused blob not deleted");
    const auto stats = cache.GetStats();
    if (stats.hits != 2 || stats.misses != 2 || stats.rejected != 1)
        throw std::runtime_error("stats: hits " + std::to_string(stats.hits) + " misses " +
                                 std::to_string(stats.misses) + " rejected " +
                                 std::to_string(stats.rejected));
    std::filesystem::remove_all(directory);
    std::cout << "PASS PSO blobs: " << stats_stored.bytes / 1024
              << " KiB blob stored, PSO created again from it, damaged blob refused (HRESULT 0x"
              << std::hex << static_cast<u32>(damaged_hr) << std::dec
              << ") and deleted, renamed blob ignored, hash stable across root objects\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
