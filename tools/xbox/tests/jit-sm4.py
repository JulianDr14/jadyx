# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production SM4 lookup operations with a distant synthetic JIT mapping."""

from pathlib import Path

from jit_test_support import BACKEND, function, run_cpp

source = (BACKEND / "emit_x64_sm4.cpp").read_text()
table = source[source.index("alignas(64)") : source.index("void EmitX64::EmitSM4")]
emitter = function(source, "void EmitX64::EmitSM4AccessSubstitutionBox")
fixture = Path(__file__).with_suffix(".cpp").read_text()
run_cpp(fixture.replace("// PRODUCTION", table + emitter))
print("PASS: 65536 SM4 byte lookups, high input bits ignored, table beyond rel32 reach")
