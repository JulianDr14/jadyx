// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/ir_opt/workgroup_split_pass.h"

using namespace Shader;
using Op = IR::Opcode;
using V = Optimization::WorkgroupSplitVerdict;

static void Require(bool v, std::source_location at = std::source_location::current()) {
    if (!v) throw std::runtime_error("regression failed at line " + std::to_string(at.line()));
}

/// How a case departs from the plain persistent loop
///   if (lid < N) { i = lid; do { out[i] = wid; i += 64; } while (i < N); }
///   if (leader) { out[0] = wid; }
struct Shape {
    enum class Leader { None, Alone, Empty, EmptyReturn, OtherBound };
    u32 step{64};
    bool guard{true};
    bool guard_other_bound{};
    Leader leader{};
    bool lid_in_math{};
    bool loop{true};
    bool barrier{};
    bool shared_write{};
    bool write_before{}; ///< every lane writes before the guard
    bool write_after{};  ///< every in-range lane writes after the loop, inside the guard
};

constexpr u32 N = 1000;

/// Builds the blocks with the edges and the If/Loop syntax list the frontend gives them.
struct Fixture {
    using Node = IR::AbstractSyntaxNode;
    using Leader = Shape::Leader;
    ObjectPool<IR::Inst> insts;
    ObjectPool<IR::Block> blocks;
    IR::Program p;
    IR::Inst* group_id{};
    IR::Inst* local_id{};
    IR::Inst* step{};

    explicit Fixture(const Shape& s) {
        p.stage = Stage::Compute;
        p.workgroup_size = {64, 1, 1};
        auto* entry = Make();
        IR::IREmitter e{*entry};
        const IR::U32 wid{e.WorkgroupIdX()};
        const IR::U32 lid{e.LocalInvocationIdX()};
        group_id = wid.InstRecursive();
        local_id = lid.InstRecursive();
        const IR::U64 out{e.Imm64(u64{0x1000})};
        const IR::U32 bound{e.Imm32(N)};
        if (s.lid_in_math) e.WriteGlobal32(out, e.IMul(lid, e.Imm32(3U)));
        if (s.barrier) e.Barrier();
        if (s.shared_write) e.WriteShared(32, e.Imm32(0U), wid);
        if (s.write_before) e.WriteGlobal32(out, wid);
        Block(entry);
        if (!s.loop) {
            e.WriteGlobal32(out, wid);
            Return(entry);
            return;
        }

        auto* header = Make();
        auto* body = Make();
        auto* after = Make();
        auto* exit = Make();
        if (s.guard) {
            const IR::U32 limit{e.Imm32(s.guard_other_bound ? N / 2 : N)};
            If(entry, e.ConditionRef(e.LogicalNot(e.IGreaterThanEqual(lid, limit, true))),
               header, exit);
        } else {
            entry->AddBranch(header);
        }
        Block(header);
        IR::Inst* const phi{&*header->PrependNewInst(header->end(), Op::Phi, {},
                                                     static_cast<u32>(IR::Type::U32))};
        header->AddBranch(body);
        Push(Node::Type::Loop);
        Block(body);
        IR::IREmitter b{*body};
        const IR::U32 index{IR::Value{phi}};
        b.WriteGlobal32(IR::U64{b.IAdd(out, IR::U64{b.UConvert(64, index)})}, wid);
        const IR::U32 next{b.IAdd(index, b.Imm32(s.step))};
        step = next.InstRecursive();
        const IR::U1 again{b.ConditionRef(b.ILessThan(next, bound, true))};
        body->AddBranch(header);
        body->AddBranch(after);
        Node& repeat{Push(Node::Type::Repeat)};
        repeat.data.repeat = {again, header, after};
        Block(after);
        if (s.write_after) IR::IREmitter{*after}.WriteGlobal32(out, wid);
        after->AddBranch(exit);
        if (s.guard) Push(Node::Type::EndIf);
        Block(exit);
        phi->AddPhiOperand(entry, lid);
        phi->AddPhiOperand(body, next);
        if (s.leader == Leader::None) {
            Return(exit);
            return;
        }

        IR::IREmitter x{*exit};
        IR::U1 leader{x.IEqual(lid, x.Imm32(0U))};
        if (s.leader != Leader::Alone) {
            const IR::U32 count{x.Imm32(s.leader == Leader::OtherBound ? N / 2 : N)};
            leader = x.LogicalAnd(x.IEqual(count, x.Imm32(0U)), leader);
        }
        auto* tail = Make();
        auto* end = Make();
        IR::IREmitter{*tail}.WriteGlobal32(out, wid);
        if (s.leader == Leader::EmptyReturn) {
            // if (!leader) return; out[0] = wid;  the guest's early EXIT
            auto* leave = Make();
            IR::IREmitter{*leave}.Epilogue();
            If(exit, x.ConditionRef(x.LogicalNot(leader)), leave, tail);
            Block(leave);
            Push(Node::Type::Return);
            Push(Node::Type::EndIf);
            Block(tail);
            Return(tail);
            return;
        }
        If(exit, x.ConditionRef(leader), tail, end);
        Block(tail);
        tail->AddBranch(end);
        Push(Node::Type::EndIf);
        Block(end);
        Return(end);
    }

    IR::Block* Make() {
        auto* b = blocks.Create(insts);
        p.blocks.push_back(b);
        p.post_order_blocks.insert(p.post_order_blocks.begin(), b);
        return b;
    }
    Node& Push(Node::Type type) {
        Node& node{p.syntax_list.emplace_back()};
        node.type = type;
        return node;
    }
    void Block(IR::Block* block) { Push(Node::Type::Block).data.block = block; }
    void If(IR::Block* from, IR::U1 cond, IR::Block* body, IR::Block* merge) {
        from->AddBranch(body);
        from->AddBranch(merge);
        Push(Node::Type::If).data.if_node = {cond, body, merge};
    }
    void Return(IR::Block* last) {
        IR::IREmitter{*last}.Epilogue();
        Push(Node::Type::Return);
    }
};

static Op Resolved(IR::Inst* inst) {
    return IR::Value{inst}.Resolve().InstRecursive()->GetOpcode();
}

int main() try {
    unsigned cases{};
    {
        Fixture f{Shape{}};
        Require(Optimization::WorkgroupSplitPass(f.p, 8) == V::Split);
        Require(f.step->GetOpcode() == Op::IAdd32);
        Require(f.step->Arg(1).IsImmediate() && f.step->Arg(1).U32() == 64 * 8);
        Require(f.group_id->GetOpcode() == Op::Identity);
        Require(Resolved(f.group_id) == Op::ShiftRightLogical32);
        Require(f.local_id->GetOpcode() == Op::Identity);
        Require(Resolved(f.local_id) == Op::IAdd32);
        Require(f.p.info.uses_workgroup_id && f.p.info.uses_local_invocation_id);
        // The rewritten program is no stride loop of width 64 any more.
        Require(Optimization::WorkgroupSplitPass(f.p, 8) != V::Split);
        ++cases;
    }
    for (const auto leader : {Shape::Leader::Empty, Shape::Leader::EmptyReturn}) {
        // lid == 0 && bound == 0: nothing to walk, so only slice 0's first lane runs the tail.
        Fixture f{Shape{.leader = leader}};
        Require(Optimization::WorkgroupSplitPass(f.p, 4) == V::Split);
        Require(f.step->Arg(1).U32() == 64 * 4);
        ++cases;
    }
    const auto expect = [&](const Shape& shape, V verdict, u32 factor = 8) {
        Fixture f{shape};
        Require(Optimization::WorkgroupSplitPass(f.p, factor) == verdict);
        if (f.step) Require(f.step->Arg(1).U32() == shape.step); // left untouched
        Require(f.local_id->GetOpcode() == Op::CompositeExtractU32x3);
        Require(f.group_id->GetOpcode() == Op::CompositeExtractU32x3);
        ++cases;
    };
    expect({.barrier = true}, V::Barrier);
    expect({.shared_write = true}, V::SharedMemory);
    expect({.lid_in_math = true}, V::OtherLocalUse);
    expect({.leader = Shape::Leader::Alone}, V::OtherLocalUse);
    expect({.leader = Shape::Leader::OtherBound}, V::OtherLocalUse);
    expect({.write_before = true}, V::UnconfinedWrite);
    expect({.write_after = true}, V::UnconfinedWrite);
    expect({.guard_other_bound = true}, V::OtherLocalUse);
    expect({.guard = false}, V::NoGuard);
    expect({.step = 32}, V::BadStrideLoop);
    expect({.loop = false}, V::NoStrideLoop);
    expect({}, V::BadFactor, 3);
    expect({}, V::BadFactor, 1);
    {
        Fixture f{Shape{}};
        f.p.workgroup_size = {64, 2, 1};
        Require(Optimization::WorkgroupSplitPass(f.p, 8) == V::NotLinear);
        f.p.workgroup_size = {64, 1, 1};
        f.p.shared_memory_size = 256;
        Require(Optimization::WorkgroupSplitPass(f.p, 8) == V::SharedMemory);
        f.p.stage = Stage::Fragment;
        Require(Optimization::WorkgroupSplitPass(f.p, 8) == V::NotCompute);
        Require(f.step->Arg(1).U32() == 64);
        cases += 3;
    }
    for (V v{V::Split}; v <= V::UnconfinedWrite; v = static_cast<V>(static_cast<int>(v) + 1)) {
        Require(!Optimization::Name(v).empty() && Optimization::Name(v) != "unknown");
    }
    std::cout << "PASS " << cases << " workgroup split cases: rewrite and rejections\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
