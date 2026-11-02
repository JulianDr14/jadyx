// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/bit_field.h"
#include "common/common_types.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {
namespace {
// High 32 bits of the 64-bit product, built from 16-bit halves because the IR has no wide
// multiply. A signed operand that is negative takes the other operand off the unsigned result.
IR::U32 MulHigh(TranslatorVisitor& v, const IR::U32& a, const IR::U32& b, bool a_signed,
                bool b_signed) {
    IR::IREmitter& ir{v.ir};
    const IR::U32 mask{ir.Imm32(0xffff)};
    const IR::U32 sixteen{ir.Imm32(16)};
    const IR::U32 a_lo{ir.BitwiseAnd(a, mask)};
    const IR::U32 a_hi{ir.ShiftRightLogical(a, sixteen)};
    const IR::U32 b_lo{ir.BitwiseAnd(b, mask)};
    const IR::U32 b_hi{ir.ShiftRightLogical(b, sixteen)};
    const IR::U32 lo_lo{ir.IMul(a_lo, b_lo)};
    const IR::U32 lo_hi{ir.IMul(a_lo, b_hi)};
    const IR::U32 hi_lo{ir.IMul(a_hi, b_lo)};
    const IR::U32 hi_hi{ir.IMul(a_hi, b_hi)};
    const IR::U32 mid{ir.IAdd(ir.IAdd(ir.ShiftRightLogical(lo_lo, sixteen), ir.BitwiseAnd(lo_hi, mask)),
                              ir.BitwiseAnd(hi_lo, mask))};
    IR::U32 high{ir.IAdd(ir.IAdd(hi_hi, ir.ShiftRightLogical(lo_hi, sixteen)),
                         ir.IAdd(ir.ShiftRightLogical(hi_lo, sixteen),
                                 ir.ShiftRightLogical(mid, sixteen)))};
    const IR::U32 zero{ir.Imm32(0)};
    if (a_signed) {
        high = ir.ISub(high, IR::U32{ir.Select(ir.ILessThan(a, zero, true), b, zero)});
    }
    if (b_signed) {
        high = ir.ISub(high, IR::U32{ir.Select(ir.ILessThan(b, zero, true), a, zero)});
    }
    return high;
}

void SetFlags(TranslatorVisitor& v, const IR::U32& result) {
    v.SetZFlag(v.ir.IEqual(result, v.ir.Imm32(0)));
    v.SetSFlag(v.ir.ILessThan(result, v.ir.Imm32(0), true));
    v.ResetCFlag();
    v.ResetOFlag();
}

void IMUL(TranslatorVisitor& v, u64 insn, const IR::U32& src_b, bool high, bool a_signed,
          bool b_signed, bool cc) {
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> src_reg_a;
    } const imul{insn};

    const IR::U32 src_a{v.X(imul.src_reg_a)};
    const IR::U32 result{high ? MulHigh(v, src_a, src_b, a_signed, b_signed)
                              : v.ir.IMul(src_a, src_b)};
    if (cc) {
        SetFlags(v, result);
    }
    v.X(imul.dest_reg, result);
}

void IMUL(TranslatorVisitor& v, u64 insn, const IR::U32& src_b) {
    union {
        u64 raw;
        BitField<39, 1, u64> high;
        BitField<40, 1, u64> a_signed;
        BitField<41, 1, u64> b_signed;
        BitField<47, 1, u64> cc;
    } const imul{insn};

    IMUL(v, insn, src_b, imul.high != 0, imul.a_signed != 0, imul.b_signed != 0, imul.cc != 0);
}

void IMAD(TranslatorVisitor& v, u64 insn, const IR::U32& src_b, const IR::U32& src_c) {
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> src_reg_a;
        BitField<47, 1, u64> cc;
        BitField<48, 1, u64> a_signed;
        BitField<49, 1, u64> x;
        BitField<50, 1, u64> sat;
        BitField<51, 1, u64> neg_product;
        BitField<52, 1, u64> neg_c;
        BitField<53, 1, u64> b_signed;
        BitField<54, 1, u64> high;
    } const imad{insn};

    if (imad.x != 0) {
        throw NotImplementedException("IMAD X");
    }
    if (imad.sat != 0) {
        throw NotImplementedException("IMAD SAT");
    }
    const IR::U32 src_a{v.X(imad.src_reg_a)};
    IR::U32 product{imad.high != 0
                        ? MulHigh(v, src_a, src_b, imad.a_signed != 0, imad.b_signed != 0)
                        : v.ir.IMul(src_a, src_b)};
    if (imad.neg_product != 0) {
        product = v.ir.INeg(product);
    }
    const IR::U32 op_c{imad.neg_c != 0 ? IR::U32{v.ir.INeg(src_c)} : src_c};
    const IR::U32 result{v.ir.IAdd(product, op_c)};
    if (imad.cc != 0) {
        SetFlags(v, result);
    }
    v.X(imad.dest_reg, result);
}
} // Anonymous namespace

void TranslatorVisitor::IMUL_reg(u64 insn) {
    IMUL(*this, insn, GetReg20(insn));
}

void TranslatorVisitor::IMUL_cbuf(u64 insn) {
    IMUL(*this, insn, GetCbuf(insn));
}

void TranslatorVisitor::IMUL_imm(u64 insn) {
    IMUL(*this, insn, GetImm20(insn));
}

void TranslatorVisitor::IMUL32I(u64 insn) {
    union {
        u64 raw;
        BitField<52, 1, u64> cc;
        BitField<53, 1, u64> high;
        BitField<54, 1, u64> a_signed;
        BitField<55, 1, u64> b_signed;
    } const imul{insn};

    IMUL(*this, insn, GetImm32(insn), imul.high != 0, imul.a_signed != 0, imul.b_signed != 0,
         imul.cc != 0);
}

void TranslatorVisitor::IMAD_reg(u64 insn) {
    IMAD(*this, insn, GetReg20(insn), GetReg39(insn));
}

void TranslatorVisitor::IMAD_rc(u64 insn) {
    IMAD(*this, insn, GetReg39(insn), GetCbuf(insn));
}

void TranslatorVisitor::IMAD_cr(u64 insn) {
    IMAD(*this, insn, GetCbuf(insn), GetReg39(insn));
}

void TranslatorVisitor::IMAD_imm(u64 insn) {
    IMAD(*this, insn, GetImm20(insn), GetReg39(insn));
}

} // namespace Shader::Maxwell
