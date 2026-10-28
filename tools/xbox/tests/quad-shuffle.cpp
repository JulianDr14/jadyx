// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Validate fragment quad shuffles through fine derivatives with the production emitter and Mesa linker.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
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
    if (argc != 2) throw std::runtime_error("usage: quad-shuffle OUTPUT_DIR");
    Common::Log::Initialize();
    std::filesystem::create_directories(argv[1]);
    D3D12::ShaderCompiler compiler;
    if (!compiler.IsAvailable()) throw std::runtime_error("compiler unavailable");
    auto validator = spvContextCreate(SPV_ENV_UNIVERSAL_1_3);
    for (bool helperless : {false, true}) {
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
        for (u32 c = 0; c < 4; ++c) {
            auto a = v.GetAttribute(IR::Attribute::Generic0X + c);
            v.SetAttribute(IR::Attribute::PositionX + c, a, v.Imm32(0U));
            v.SetAttribute(IR::Attribute::Generic0X + c, a, v.Imm32(0U));
            vs.info.loads.Set(IR::Attribute::Generic0X+c);
            vs.info.stores.Set(IR::Attribute::PositionX+c);
            vs.info.stores.Set(IR::Attribute::Generic0X+c);
        }
        v.Epilogue();
        auto* pb = Block(ps, insts, blocks);
        Append(ps, pb);
        IR::IREmitter f{*pb};
        f.Prologue();
        const IR::U32 value{f.BitCast<IR::U32>(f.GetAttribute(IR::Attribute::Generic0X))};
        const IR::U32 leader{f.QuadBroadcast(value, f.Imm32(0U))};
        const IR::U32 across_x{f.QuadSwap(value, f.Imm32(0U))};
        const IR::U32 across_y{f.QuadSwap(value, f.Imm32(1U))};
        const IR::U32 diagonal{f.QuadBroadcast(value, f.Imm32(3U))};
        f.SetFragColor(0, 0, f.BitCast<IR::F32>(leader));
        f.SetFragColor(0, 1, f.BitCast<IR::F32>(across_x));
        f.SetFragColor(0, 2, f.BitCast<IR::F32>(across_y));
        f.SetFragColor(0, 3, f.BitCast<IR::F32>(diagonal));
        f.Epilogue();
        ps.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
        ps.info.loads.Set(IR::Attribute::Generic0X);
        ps.info.stores_frag_color[0] = true;
        ps.info.uses_quad_shuffles = true;
        Profile profile;
        profile.supported_spirv = 0x00010300;
        profile.support_quad_shuffles = true;
        profile.support_derivative_control = true;
        profile.has_helperless_quad_shuffles = helperless;
        RuntimeInfo vr, pr;
        vr.previous_stage_stores.mask.set();
        pr.previous_stage_stores = vs.info.stores;
        Backend::Bindings bindings;
        const auto vertex = Backend::SPIRV::EmitSPIRV(profile, vr, vs, bindings);
        const auto pixel = Backend::SPIRV::EmitSPIRV(profile, pr, ps, bindings);
        unsigned quad_reads{}, derivatives{};
        for (std::size_t i = 5; i < pixel.size(); i += pixel[i] >> 16) {
            const u32 op{pixel[i] & 0xffff};
            quad_reads += op == 365;
            derivatives += op == 210 || op == 211;
        }
        if ((helperless && (quad_reads || derivatives != 16)) ||
            (!helperless && (!quad_reads || derivatives)))
            throw std::runtime_error("incorrect quad exchange instructions");
        spv_diagnostic diagnostic{};
        if (spvValidateBinary(validator, pixel.data(), pixel.size(), &diagnostic) != SPV_SUCCESS)
            throw std::runtime_error(diagnostic ? diagnostic->error : "invalid SPIR-V");
        spvDiagnosticDestroy(diagnostic);
        const D3D12::ShaderCompiler::PipelineStage stages[]{
            {vertex, DXIL_SPIRV_SHADER_VERTEX}, {pixel, DXIL_SPIRV_SHADER_FRAGMENT}};
        const auto linked = compiler.CompilePipeline(stages, {});
        if (linked.size() != 2) throw std::runtime_error("link failed");
        const auto name = helperless ? "derivatives" : "quad";
        std::ofstream spv{std::filesystem::path(argv[1])/(std::string(name)+".spv"), std::ios::binary};
        spv.write(reinterpret_cast<const char*>(pixel.data()), pixel.size() * sizeof(u32));
        std::ofstream file{std::filesystem::path(argv[1])/(std::string(name)+".dxil"), std::ios::binary};
        file.write(reinterpret_cast<const char*>(linked[1].dxil.data()), linked[1].dxil.size());
        if (!spv || !file) throw std::runtime_error("write failed");
    }
    spvContextDestroy(validator);
    std::cout << "PASS fragment quad shuffles through fine derivatives: SPIR-V validation, linked signed DXIL\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
