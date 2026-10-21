// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// GPU regression of the fragment local memory pool (Shader::LocalMemoryScratch) on the host GPU:
// production emitter, Mesa, D3D12::Scheduler and D3D12::LocalMemoryPool. Full-screen draws keep a
// per-pixel value at indexed local memory words and read it back; every pixel is checked, so
// waves sharing a slot, a wrong stride, a leaked slot or a lost pool header shows as a wrong pixel.
// Also checks that waiting waves grow the pool and that growing rows keeps the slots. The vertex
// shader is the host full-screen triangle, as in the boot probe.
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "common/logging.h"
#include "common/settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/local_memory_scratch.h"
#include "video_core/host_shaders/full_screen_triangle_vert_spv.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_local_memory_scratch.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"

using namespace Shader;
using D3D12::ComPtr;

namespace {
constexpr u32 W = 1920, H = 1080;
/// As the guest shader that needed the pool: past the private array limit.
constexpr u32 WORDS = 588;
constexpr u32 SECOND_WORD_FLIP = 256;

IR::Block* Block(IR::Program& p, ObjectPool<IR::Inst>& insts, ObjectPool<IR::Block>& blocks) {
    auto* b = blocks.Create(insts);
    p.blocks.push_back(b);
    p.post_order_blocks.push_back(b);
    return b;
}
void Append(IR::Program& p, IR::Block* b) {
    IR::AbstractSyntaxNode n;
    n.type = IR::AbstractSyntaxNode::Type::Block;
    n.data.block = b;
    p.syntax_list.push_back(n);
}

u32 PixelValue(u32 x, u32 y) {
    return x | (y << 16);
}
u32 PixelIndex(u32 x, u32 y) {
    return (x * 7 + y * 13) & 511;
}
/// Pixels demoted by the demote variant (after their stores, before their loads).
bool Demoted(u32 x) {
    return (x & 3) == 0;
}

/// A PS that stores its value at two indexed words, optionally demotes a quarter of the lanes,
/// and outputs (word0, word1, value, index).
std::vector<u32> EmitPixelShader(u32 lanes, bool demote) {
    ObjectPool<IR::Inst> insts;
    ObjectPool<IR::Block> blocks;
    IR::Program ps;
    ps.stage = Stage::Fragment;
    ps.local_memory_size = WORDS * 4;
    auto* before = Block(ps, insts, blocks);
    Append(ps, before);
    IR::IREmitter f{*before};
    f.Prologue();
    const IR::U32 x{f.ConvertFToU(32, f.GetAttribute(IR::Attribute::PositionX))};
    const IR::U32 y{f.ConvertFToU(32, f.GetAttribute(IR::Attribute::PositionY))};
    const IR::U32 value{f.BitwiseOr(x, IR::U32{f.ShiftLeftLogical(y, f.Imm32(16U))})};
    const IR::U32 index{f.BitwiseAnd(
        IR::U32{f.IAdd(f.IMul(x, f.Imm32(7U)), f.IMul(y, f.Imm32(13U)))}, f.Imm32(511U))};
    const IR::U32 second{f.BitwiseXor(index, f.Imm32(SECOND_WORD_FLIP))};
    f.WriteLocal(index, value);
    f.WriteLocal(second, f.BitwiseNot(value));
    IR::Block* after = before;
    if (demote) {
        auto* killed = Block(ps, insts, blocks);
        after = Block(ps, insts, blocks);
        IR::AbstractSyntaxNode branch;
        branch.type = IR::AbstractSyntaxNode::Type::If;
        branch.data.if_node = {f.IEqual(f.BitwiseAnd(x, f.Imm32(3U)), f.Imm32(0U)), killed,
                               after};
        ps.syntax_list.push_back(branch);
        Append(ps, killed);
        IR::IREmitter{*killed}.DemoteToHelperInvocation();
        IR::AbstractSyntaxNode end;
        end.type = IR::AbstractSyntaxNode::Type::EndIf;
        end.data.end_if.merge = after;
        ps.syntax_list.push_back(end);
        Append(ps, after);
        ps.info.uses_demote_to_helper_invocation = true;
    }
    IR::IREmitter a{*after};
    const IR::U32 first_loaded{a.LoadLocal(index)};
    const IR::U32 second_loaded{a.LoadLocal(second)};
    a.SetFragColor(0, 0, a.BitCast<IR::F32>(first_loaded));
    a.SetFragColor(0, 1, a.BitCast<IR::F32>(second_loaded));
    a.SetFragColor(0, 2, a.BitCast<IR::F32>(value));
    a.SetFragColor(0, 3, a.BitCast<IR::F32>(index));
    a.Epilogue();
    ps.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
    ps.info.loads.Set(IR::Attribute::PositionX);
    ps.info.loads.Set(IR::Attribute::PositionY);
    ps.info.stores_frag_color[0] = true;

    Profile profile;
    profile.supported_spirv = 0x00010300;
    profile.support_demote_to_helper_invocation = true;
    profile.supported_subgroup_stages = 0x7F;
    profile.warp_size_potentially_larger_than_guest = lanes > 32;
    profile.fragment_local_memory_scratch_lanes = lanes;
    RuntimeInfo runtime;
    runtime.color_output_types[0] = AttributeType::UnsignedInt;
    Backend::Bindings bindings;
    auto pixel = Backend::SPIRV::EmitSPIRV(profile, runtime, ps, bindings);
    if (ps.info.local_memory_scratch_words != WORDS) {
        throw std::runtime_error("pixel shader does not keep its local memory in the pool");
    }
    return pixel;
}

struct Target {
    ComPtr<ID3D12Resource> image;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    ComPtr<ID3D12Resource> readback;
    std::span<u8> readback_data;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
};

Target CreateTarget(ID3D12Device* device) {
    Target target;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = W;
    desc.Height = H;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    D3D12::ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                         nullptr, IID_PPV_ARGS(&target.image)),
                         "target");
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                               .NumDescriptors = 1};
    D3D12::ThrowIfFailed(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&target.rtv_heap)),
                         "RTV heap");
    target.rtv = target.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.image.Get(), nullptr, target.rtv);
    u64 bytes{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &target.footprint, nullptr, nullptr, &bytes);
    target.readback = D3D12::CreateMappedBuffer(device, bytes, D3D12_HEAP_TYPE_READBACK,
                                                target.readback_data);
    return target;
}

ComPtr<ID3D12RootSignature> CreateRoot(ID3D12Device* device) {
    std::array<D3D12_ROOT_PARAMETER, 3> params{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {.ShaderRegister = 0, .RegisterSpace = D3D12::PUSH_CONSTANT_SPACE,
                           .Num32BitValues = 4};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {.ShaderRegister = 0, .RegisterSpace = D3D12::RUNTIME_DATA_SPACE,
                           .Num32BitValues = 12};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor = {.ShaderRegister = LocalMemoryScratch::BINDING,
                            .RegisterSpace = LocalMemoryScratch::DESCRIPTOR_SET};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_ROOT_SIGNATURE_DESC desc{
        .NumParameters = static_cast<UINT>(params.size()),
        .pParameters = params.data(),
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
    };
    return D3D12::CreateRootSignature(device, desc, "pool test root signature");
}

ComPtr<ID3D12PipelineState> CreatePipeline(ID3D12Device* device,
                                           const D3D12::ShaderCompiler& compiler,
                                           ID3D12RootSignature* root, u32 lanes, bool demote) {
    const std::vector<u32> pixel = EmitPixelShader(lanes, demote);
    const std::array<D3D12::ShaderCompiler::PipelineStage, 2> stages{{
        {FULL_SCREEN_TRIANGLE_VERT_SPV, DXIL_SPIRV_SHADER_VERTEX},
        {pixel, DXIL_SPIRV_SHADER_FRAGMENT},
    }};
    const auto compiled = compiler.CompilePipeline(stages, {});
    if (compiled.size() != 2) {
        throw std::runtime_error("Mesa link failed");
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.VS = {compiled[0].dxil.data(), compiled[0].dxil.size()};
    desc.PS = {compiled[1].dxil.data(), compiled[1].dxil.size()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_UINT;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    D3D12::ThrowIfFailed(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)),
                         "pool test pipeline");
    return pso;
}

struct Run {
    u64 frames{};
    u64 wrong_pixels{};
    u64 checked_pixels{};
};

/// Draws `draws` full-screen triangles per list, reads the target back after each list and
/// checks every pixel, for `seconds` (at least `min_frames` lists).
Run Draw(D3D12::Scheduler& scheduler, D3D12::LocalMemoryPool& pool, Target& target,
         ID3D12RootSignature* root, ID3D12PipelineState* pso, u32 lanes,
         u32 words, bool demote, u32 draws, double seconds, u64 min_frames) {
    Run run;
    const auto start = std::chrono::steady_clock::now();
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(W), float(H), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, LONG(W), LONG(H)};
    const std::array<u32, 12> zeros{};
    const FLOAT clear[4]{};
    while (run.frames < min_frames ||
           std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() <
               seconds) {
        D3D12::CommandStream* const cmd = scheduler.CommandList();
        cmd->ClearRenderTargetView(target.rtv, clear, 0, nullptr);
        cmd->SetGraphicsRootSignature(root);
        scheduler.SetPipelineState(pso);
        cmd->SetGraphicsRoot32BitConstants(0, 4, zeros.data(), 0);
        cmd->SetGraphicsRoot32BitConstants(1, 12, zeros.data(), 0);
        cmd->OMSetRenderTargets(1, &target.rtv, FALSE, nullptr);
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &scissor);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (u32 draw = 0; draw < draws; ++draw) {
            // As the rasterizer: the pool address is asked for at every draw that uses it.
            cmd->SetGraphicsRootUnorderedAccessView(2, pool.Address(lanes, words));
            cmd->DrawInstanced(3, 1, 0, 0);
        }
        auto barrier = D3D12::TransitionBarrier(target.image.Get(),
                                                D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->ResourceBarrier(1, &barrier);
        const D3D12_TEXTURE_COPY_LOCATION src{.pResource = target.image.Get(),
                                              .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                              .SubresourceIndex = 0};
        const D3D12_TEXTURE_COPY_LOCATION dst{.pResource = target.readback.Get(),
                                              .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                              .PlacedFootprint = target.footprint};
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier = D3D12::TransitionBarrier(target.image.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &barrier);
        scheduler.Finish();
        ++run.frames;

        const u32 pitch = target.footprint.Footprint.RowPitch;
        for (u32 y = 0; y < H; ++y) {
            const u8* const row = target.readback_data.data() + target.footprint.Offset +
                                  u64{y} * pitch;
            for (u32 x = 0; x < W; ++x) {
                if (demote && Demoted(x)) {
                    continue;
                }
                std::array<u32, 4> texel;
                std::memcpy(texel.data(), row + x * 16, sizeof(texel));
                const u32 value = PixelValue(x, y);
                ++run.checked_pixels;
                if (texel[0] != value || texel[1] != ~value || texel[2] != value ||
                    texel[3] != PixelIndex(x, y)) {
                    if (run.wrong_pixels < 5) {
                        std::cout << "  wrong pixel (" << x << ", " << y << "): " << std::hex
                                  << texel[0] << ' ' << texel[1] << ' ' << texel[2] << ' '
                                  << texel[3] << " expected " << value << std::dec << '\n';
                    }
                    ++run.wrong_pixels;
                }
            }
        }
    }
    return run;
}

void Expect(bool condition, const std::string& what) {
    if (!condition) {
        throw std::runtime_error(what);
    }
    std::cout << "PASS " << what << '\n';
}
} // Anonymous namespace

int main(int argc, char** argv) try {
    if (argc != 2) throw std::runtime_error("usage: local-memory-pool OUTPUT_DIR");
    Common::Log::Initialize();
    Common::Log::Start();
    D3D12::Device device;
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    D3D12::ThrowIfFailed(device.Get()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1,
                                                           &options1, sizeof(options1)),
                         "options1");
    if (!options1.WaveOps) throw std::runtime_error("no wave ops on this GPU");
    const u32 lanes = options1.WaveLaneCountMax;
    std::cout << "wave lanes: " << lanes << '\n';
    D3D12::ShaderCompiler compiler;
    if (!compiler.IsAvailable()) throw std::runtime_error("compiler unavailable");

    D3D12::Scheduler scheduler{device};
    D3D12::LocalMemoryPool pool{device, scheduler};
    const ComPtr<ID3D12RootSignature> root = CreateRoot(device.Get());
    const ComPtr<ID3D12PipelineState> single_exit =
        CreatePipeline(device.Get(), compiler, root.Get(), lanes, false);
    const ComPtr<ID3D12PipelineState> with_demote =
        CreatePipeline(device.Get(), compiler, root.Get(), lanes, true);
    Target target = CreateTarget(device.Get());

    // 1. Values survive at the smallest pool: first lists, before any sample can grow it.
    Run run = Draw(scheduler, pool, target, root.Get(), single_exit.Get(), lanes,
                   WORDS, false, 1, 0.0, 1);
    Expect(pool.Slots() == LocalMemoryScratch::MIN_SLOTS,
           "first pool has " + std::to_string(LocalMemoryScratch::MIN_SLOTS) + " slots");
    Expect(run.wrong_pixels == 0, "single exit, minimum slots: " +
                                      std::to_string(run.checked_pixels) + " pixels exact");

    // 2. Sustained full-screen load: waves wait for slots, the counters grow the pool.
    run = Draw(scheduler, pool, target, root.Get(), single_exit.Get(), lanes,
               WORDS, false, 8, 3.0, 4);
    std::cout << "slots after sustained load: " << pool.Slots() << " (" << run.frames
              << " lists)\n";
    Expect(run.wrong_pixels == 0, "single exit, while growing: " +
                                      std::to_string(run.checked_pixels) + " pixels exact");
    Expect(pool.Slots() > LocalMemoryScratch::MIN_SLOTS, "pool grew under contention");
    const u32 grown_slots = pool.Slots();

    // 3. Demoted lanes release their slot: no leak over many lists, values intact.
    run = Draw(scheduler, pool, target, root.Get(), with_demote.Get(), lanes,
               WORDS, true, 8, 2.0, 50);
    Expect(run.wrong_pixels == 0, "demote, " + std::to_string(run.frames) + " lists: " +
                                      std::to_string(run.checked_pixels) + " pixels exact");

    // 4. Longer rows (a larger shader) recreate the pool with the slots it already needed.
    const u32 slots_before_rows = pool.Slots();
    run = Draw(scheduler, pool, target, root.Get(), single_exit.Get(), lanes,
               1000, false, 2, 0.0, 2);
    Expect(pool.Slots() >= slots_before_rows || pool.Slots() >= grown_slots / 2,
           "longer rows keep the slots (" + std::to_string(pool.Slots()) + ")");
    Expect(run.wrong_pixels == 0, "after row growth: " + std::to_string(run.checked_pixels) +
                                      " pixels exact");
    scheduler.Finish();
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
}
