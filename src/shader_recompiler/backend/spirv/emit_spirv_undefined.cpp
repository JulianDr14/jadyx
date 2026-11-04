// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {

// A guest register or predicate read before any write is zero, not OpUndef. A host compiler may fold
// an undefined value into anything (drop a branch, keep a stale register), and the driver compilers
// downstream of DXIL do. The guest never relies on what such a read holds, so zero is a value it
// accepts, and one no compiler may change.
Id EmitUndefU1(EmitContext& ctx) {
    return ctx.false_value;
}

Id EmitUndefU8(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitUndefU16(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitUndefU32(EmitContext& ctx) {
    return ctx.u32_zero_value;
}

Id EmitUndefU64(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

} // namespace Shader::Backend::SPIRV
