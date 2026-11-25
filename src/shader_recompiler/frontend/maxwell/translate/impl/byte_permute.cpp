// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/bit_field.h"
#include "common/common_types.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {
namespace {
// PRMT d, a, selector, c: each byte of d is picked from the eight bytes of {c, a}, where a holds
// bytes 0-3 and c bytes 4-7. The default mode reads one selector nibble per byte (low three bits
// pick the byte, bit 3 replicates its sign bit). The other modes derive all four picks from the
// selector's low two bits, as PTX prmt.b32 does.
enum class Mode : u64 {
    Default = 0,
    F4E = 1,  // forward four extract
    B4E = 2,  // backward four extract
    RC8 = 3,  // replicate one byte
    ECL = 4,  // edge clamp left
    ECR = 5,  // edge clamp right
    RC16 = 6, // replicate one half
};

struct Pick {
    u32 index;
    bool sign;
};

Pick StaticPick(Mode mode, u32 selector, u32 i) {
    const u32 c = selector & 3;
    switch (mode) {
    case Mode::Default: {
        const u32 nibble = (selector >> (4 * i)) & 0xf;
        return {nibble & 7, (nibble & 8) != 0};
    }
    case Mode::F4E:
        return {(c + i) & 7, false};
    case Mode::B4E:
        return {(c + 8 - i) & 7, false};
    case Mode::RC8:
        return {c, false};
    case Mode::ECL:
        return {i > c ? i : c, false};
    case Mode::ECR:
        return {i < c ? i : c, false};
    case Mode::RC16:
        return {(selector & 1) * 2 + (i & 1), false};
    }
    throw NotImplementedException("PRMT mode {}", static_cast<u64>(mode));
}

IR::U32 StaticByte(TranslatorVisitor& v, const IR::U32& a, const IR::U32& c, Pick pick) {
    const IR::U32 src{pick.index < 4 ? a : c};
    const u32 bit_offset{(pick.index & 3) * 8};
    if (pick.sign) {
        // 0 or all ones, from the picked byte's top bit.
        return v.ir.BitFieldExtract(src, v.ir.Imm32(bit_offset + 7), v.ir.Imm32(1), true);
    }
    return v.ir.BitFieldExtract(src, v.ir.Imm32(bit_offset), v.ir.Imm32(8), false);
}

IR::U32 DynamicByte(TranslatorVisitor& v, Mode mode, const IR::U32& a, const IR::U32& sel,
                    const IR::U32& c, u32 i) {
    const IR::U32 c2{v.ir.BitwiseAnd(sel, v.ir.Imm32(3))};
    const IR::U32 seven{v.ir.Imm32(7)};
    IR::U32 index;
    IR::U1 sign{v.ir.Imm1(false)};
    switch (mode) {
    case Mode::Default: {
        const IR::U32 nibble{v.ir.BitFieldExtract(sel, v.ir.Imm32(4 * i), v.ir.Imm32(4), false)};
        index = v.ir.BitwiseAnd(nibble, seven);
        sign = v.ir.INotEqual(v.ir.BitwiseAnd(nibble, v.ir.Imm32(8)), v.ir.Imm32(0));
        break;
    }
    case Mode::F4E:
        index = v.ir.BitwiseAnd(IR::U32{v.ir.IAdd(c2, v.ir.Imm32(i))}, seven);
        break;
    case Mode::B4E:
        index = v.ir.BitwiseAnd(IR::U32{v.ir.IAdd(c2, v.ir.Imm32(8 - i))}, seven);
        break;
    case Mode::RC8:
        index = c2;
        break;
    case Mode::ECL:
        index = v.ir.UMax(c2, v.ir.Imm32(i));
        break;
    case Mode::ECR:
        index = v.ir.UMin(c2, v.ir.Imm32(i));
        break;
    case Mode::RC16:
        index = IR::U32{v.ir.IAdd(
            IR::U32{v.ir.ShiftLeftLogical(v.ir.BitwiseAnd(sel, v.ir.Imm32(1)), v.ir.Imm32(1))},
            v.ir.Imm32(i & 1))};
        break;
    default:
        throw NotImplementedException("PRMT mode {}", static_cast<u64>(mode));
    }
    const IR::U1 from_c{v.ir.IGreaterThanEqual(index, v.ir.Imm32(4), false)};
    const IR::U32 src{v.ir.Select(from_c, c, a)};
    const IR::U32 bit_offset{
        v.ir.ShiftLeftLogical(v.ir.BitwiseAnd(index, v.ir.Imm32(3)), v.ir.Imm32(3))};
    const IR::U32 byte{v.ir.BitFieldExtract(src, bit_offset, v.ir.Imm32(8), false)};
    if (mode != Mode::Default) {
        return byte;
    }
    const IR::U32 replicated{v.ir.BitFieldExtract(
        src, IR::U32{v.ir.IAdd(bit_offset, v.ir.Imm32(7))}, v.ir.Imm32(1), true)};
    return IR::U32{v.ir.Select(sign, replicated, byte)};
}

void PRMT(TranslatorVisitor& v, u64 insn, const IR::U32& selector, const IR::U32& src_c) {
    union {
        u64 insn;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> src_a_reg;
        BitField<48, 3, Mode> mode;
    } const prmt{insn};

    const Mode mode{prmt.mode};
    const IR::U32 src_a{v.X(prmt.src_a_reg)};
    IR::U32 result{v.ir.Imm32(0)};
    for (u32 i = 0; i < 4; ++i) {
        const IR::U32 byte{selector.IsImmediate()
                               ? StaticByte(v, src_a, src_c, StaticPick(mode, selector.U32(), i))
                               : DynamicByte(v, mode, src_a, selector, src_c, i)};
        result = v.ir.BitFieldInsert(result, byte, v.ir.Imm32(8 * i), v.ir.Imm32(8));
    }
    v.X(prmt.dest_reg, result);
}
} // Anonymous namespace

void TranslatorVisitor::PRMT_reg(u64 insn) {
    PRMT(*this, insn, GetReg20(insn), GetReg39(insn));
}

void TranslatorVisitor::PRMT_rc(u64 insn) {
    PRMT(*this, insn, GetReg39(insn), GetCbuf(insn));
}

void TranslatorVisitor::PRMT_cr(u64 insn) {
    PRMT(*this, insn, GetCbuf(insn), GetReg39(insn));
}

void TranslatorVisitor::PRMT_imm(u64 insn) {
    PRMT(*this, insn, GetImm20(insn), GetReg39(insn));
}

} // namespace Shader::Maxwell
