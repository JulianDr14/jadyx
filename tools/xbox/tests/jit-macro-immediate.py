# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare production macro emitters with the interpreter's address expression."""

from pathlib import Path

from jit_test_support import ROOT, function, run_cpp

source = (ROOT / "src/video_core/macro.cpp").read_text()
production = "\n".join(
    function(source, signature)
    for signature in (
        "Xbyak::Reg32 MacroJITx64Impl::Compile_GetRegister(",
        "void MacroJITx64Impl::Compile_AddImmediate(",
        "void MacroJITx64Impl::Compile_Read(",
    )
)
interpreter = function(source, "bool MacroInterpreterImpl::Step(")
address = "GetRegister(opcode.src_a) + opcode.immediate"
assert (
    address in interpreter[interpreter.index("case Macro::Operation::AddImmediate:") :]
)
assert address in interpreter[interpreter.index("case Macro::Operation::Read:") :]
fixture = Path(__file__).with_suffix(".cpp").read_text()
run_cpp(
    fixture.replace("// PRODUCTION", production).replace("INTERPRETER_ADDRESS", address)
)
