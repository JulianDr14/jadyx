// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Validate fragment local memory kept in the pool (Shader::LocalMemoryScratch) through the
// production emitter, SPIR-V validation and the Mesa linker: one exit, and a conditional demote.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <spirv-tools/libspirv.h>
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"

using namespace Shader;

static IR::Block* Block(IR::Program& p, ObjectPool<IR::Inst>& insts, ObjectPool<IR::Block>& blocks) {
    auto* b = blocks.Create(insts);
    p.blocks.push_back(b);
    p.post_order_blocks.push_back(b);
    return b;
}
static void Append(IR::Program& p, IR::Block* b) {
    IR::AbstractSyntaxNode n;
    n.type = IR::AbstractSyntaxNode::Type::Block;
    n.data.block = b;
    p.syntax_list.push_back(n);
}
int main(int argc, char** argv) try {
    if (argc != 2) throw std::runtime_error("usage: local-memory-scratch OUTPUT_DIR");
    Common::Log::Initialize();
    std::filesystem::create_directories(argv[1]);
    D3D12::ShaderCompiler compiler;
    if (!compiler.IsAvailable()) throw std::runtime_error("compiler unavailable");
    auto validator = spvContextCreate(SPV_ENV_VULKAN_1_1);
    // indexed: offsets from a varying, else constant ones, which stay private (registers).
    struct Case { const char* name; bool demote; u32 lanes; bool indexed; bool pool; };
    for (const Case c : {Case{"pool_single_exit", false, 64, true, true},
                         Case{"pool_demote", true, 64, true, true},
                         Case{"private_control", false, 0, true, false},
                         Case{"constant_offsets", false, 64, false, false}}) {
        ObjectPool<IR::Inst> insts;
        ObjectPool<IR::Block> blocks;
        IR::Program vs, ps;
        vs.stage = Stage::VertexB;
        ps.stage = Stage::Fragment;
        auto* vb = Block(vs, insts, blocks);
        Append(vs, vb);
        vs.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
        IR::IREmitter v{*vb};
        v.Prologue();
        for (u32 i = 0; i < 4; ++i) {
            auto a = v.GetAttribute(IR::Attribute::Generic0X + i);
            v.SetAttribute(IR::Attribute::PositionX + i, a, v.Imm32(0U));
            v.SetAttribute(IR::Attribute::Generic0X + i, a, v.Imm32(0U));
            vs.info.loads.Set(IR::Attribute::Generic0X + i);
            vs.info.stores.Set(IR::Attribute::PositionX + i);
            vs.info.stores.Set(IR::Attribute::Generic0X + i);
        }
        v.Epilogue();

        // 588 words, as the guest shader that needed this: past the private array limit.
        ps.local_memory_size = 588 * 4;
        auto* before = Block(ps, insts, blocks);
        Append(ps, before);
        IR::IREmitter f{*before};
        f.Prologue();
        const auto x = f.GetAttribute(IR::Attribute::Generic0X);
        const IR::U32 index{f.ConvertFToU(32, x)};
        const IR::U32 offset{c.indexed ? IR::U32{f.ShiftLeftLogical(index, f.Imm32(2U))}
                                       : f.Imm32(500U)};
        f.WriteLocal(offset, f.Imm32(0x3f123456U));
        IR::Block* after = before;
        if (c.demote) {
            auto* killed = Block(ps, insts, blocks);
            after = Block(ps, insts, blocks);
            IR::AbstractSyntaxNode branch;
            branch.type = IR::AbstractSyntaxNode::Type::If;
            branch.data.if_node = {f.FPLessThan(x, f.Imm32(0.0f)), killed, after};
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
        const IR::U32 loaded{a.LoadLocal(
            c.indexed ? IR::U32{a.ShiftLeftLogical(a.BitwiseXor(index, a.Imm32(1U)), a.Imm32(2U))}
                      : a.Imm32(501U))};
        a.SetFragColor(0, 0, a.BitCast<IR::F32>(loaded));
        a.SetFragColor(0, 1, x);
        a.SetFragColor(0, 2, x);
        a.SetFragColor(0, 3, a.Imm32(1.0f));
        a.Epilogue();
        ps.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
        ps.info.loads.Set(IR::Attribute::Generic0X);
        ps.info.stores_frag_color[0] = true;

        Profile profile;
        profile.supported_spirv = 0x00010300;
        profile.support_demote_to_helper_invocation = true;
        profile.supported_subgroup_stages = 0x7F;
        profile.warp_size_potentially_larger_than_guest = true;
        profile.fragment_local_memory_scratch_lanes = c.lanes;
        RuntimeInfo vr, pr;
        vr.previous_stage_stores.mask.set();
        pr.previous_stage_stores = vs.info.stores;
        Backend::Bindings bindings;
        const auto vertex = Backend::SPIRV::EmitSPIRV(profile, vr, vs, bindings);
        const auto pixel = Backend::SPIRV::EmitSPIRV(profile, pr, ps, bindings);
        if ((ps.info.local_memory_scratch_words != 0) != c.pool)
            throw std::runtime_error(std::string(c.name) + ": wrong storage choice");
        {
            std::ofstream file{std::filesystem::path(argv[1]) / (std::string(c.name) + ".spv"),
                               std::ios::binary};
            file.write(reinterpret_cast<const char*>(pixel.data()), pixel.size() * 4);
        }
        spv_diagnostic diagnostic{};
        if (spvValidateBinary(validator, pixel.data(), pixel.size(), &diagnostic) != SPV_SUCCESS)
            throw std::runtime_error(std::string(c.name) + ": " +
                                     (diagnostic ? diagnostic->error : "invalid SPIR-V"));
        spvDiagnosticDestroy(diagnostic);
        const D3D12::ShaderCompiler::PipelineStage stages[]{
            {vertex, DXIL_SPIRV_SHADER_VERTEX}, {pixel, DXIL_SPIRV_SHADER_FRAGMENT}};
        const auto linked = compiler.CompilePipeline(stages, {});
        if (linked.size() != 2) throw std::runtime_error(std::string(c.name) + ": link failed");
        std::ofstream file{std::filesystem::path(argv[1]) / (std::string(c.name) + ".dxil"),
                           std::ios::binary};
        file.write(reinterpret_cast<const char*>(linked[1].dxil.data()), linked[1].dxil.size());
        if (!file) throw std::runtime_error("write failed");
        std::cout << "PASS " << c.name << '\n';
    }
    spvContextDestroy(validator);
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
