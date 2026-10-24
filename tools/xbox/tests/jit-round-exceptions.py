# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production scalar/vector ROUND emitter bodies against an MXCSR adapter."""

import sys

from jit_test_support import BACKEND, function, run_cpp

scalar = function(
    (BACKEND / "emit_x64_floating_point.cpp").read_text(), "static void EmitFPRound("
)
vector = function(
    (BACKEND / "emit_x64_vector_floating_point.cpp").read_text(),
    "void EmitFPVectorRoundInt(",
)
# Exact operations must keep using the software utility, which updates guest IXC.
assert "round_imm && !exact" in scalar
assert "TieAwayFromZero && !exact" in vector
scalar_body = scalar[scalar.index("        fsize == 64") : scalar.index("    } else {")]
vector_body = vector[
    vector.index("            EmitTwoOpVectorOperation<") : vector.index(
        "            return;"
    )
]
run_cpp(
    r"""
#include <cassert>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
using u8 = std::uint8_t;
using size_t = std::size_t;
namespace Xbyak { struct Xmm {}; }
struct Context {};
struct DefaultIndexer {};
struct Code {
    double input{};
    double result{};
    bool precision{}, invalid{}, signaling{};
    void round(Xbyak::Xmm, Xbyak::Xmm, u8 immediate) {
        // Intel ROUND immediate bit 3 suppresses PE, but not invalid (sNaN).
        if (signaling) invalid = true;
        result = input;
        if (!std::isfinite(input)) return;
        switch (immediate & 3) {
        case 0: result = std::nearbyint(input); break;
        case 1: result = std::floor(input); break;
        case 2: result = std::ceil(input); break;
        case 3: result = std::trunc(input); break;
        }
        if (input != result && (immediate & 8) == 0) precision = true;
    }
    void roundss(Xbyak::Xmm a, Xbyak::Xmm b, u8 imm) { round(a, b, imm); }
    void roundsd(Xbyak::Xmm a, Xbyak::Xmm b, u8 imm) { round(a, b, imm); }
    void roundp(Xbyak::Xmm a, Xbyak::Xmm b, u8 imm) { round(a, b, imm); }
};
template<unsigned Bits, class Callback>
void FPTwoOp(Code&, Context&, void*, Callback callback) { callback(Xbyak::Xmm{}); }
template<size_t Bits, class Indexer, int Argument, class Callback>
void EmitTwoOpVectorOperation(Code&, Context&, void*, Callback callback) {
    callback(Xbyak::Xmm{}, Xbyak::Xmm{});
}
void scalar_round(Code& code, Context& ctx, void* inst, size_t fsize,
                  std::optional<u8> round_imm) {
SCALAR
}
#define FCODE(name) code.name
template<size_t fsize>
void vector_round(Code& code, Context& ctx, void* inst, std::optional<u8> round_imm) {
VECTOR
}
int main() {
    for (bool vector : FORMS) {
        for (unsigned width : {32U, 64U}) {
            for (u8 mode = 0; mode < 4; ++mode) {
                for (bool initial : {false, true}) {
                    for (double value : {-2.5, -1.25, -0.0, 0.0, 1.25, 2.5,
                                         double(INFINITY), double(NAN)}) {
                        Code code;
                        Context ctx;
                        code.input = value;
                        code.precision = initial;
                        code.invalid = initial;
                        if (!vector) scalar_round(code, ctx, nullptr, width, mode);
                        else if (width == 64) vector_round<64>(code, ctx, nullptr, mode);
                        else vector_round<32>(code, ctx, nullptr, mode);
                        assert(code.precision == initial); // Non-exact FRINT cannot add IXC.
                        assert(code.invalid == initial);
                        double expected = value;
                        if (mode == 0) expected = std::nearbyint(value);
                        if (mode == 1) expected = std::floor(value);
                        if (mode == 2) expected = std::ceil(value);
                        if (mode == 3) expected = std::trunc(value);
                        assert(code.result == expected || (std::isnan(value) && std::isnan(code.result)));
                    }
                }
                Code code;
                Context ctx;
                code.signaling = true;
                code.input = NAN;
                if (!vector) scalar_round(code, ctx, nullptr, width, mode);
                else if (width == 64) vector_round<64>(code, ctx, nullptr, mode);
                else vector_round<32>(code, ctx, nullptr, mode);
                assert(code.invalid && !code.precision); // Suppress only precision.
            }
        }
    }
}
""".replace("SCALAR", scalar_body)
    .replace("VECTOR", vector_body)
    .replace("FORMS", "{true}" if sys.argv[1:] == ["vector"] else "{false, true}")
)
print("PASS: 256 non-exact result/sticky-flag cases, 16 invalid-preservation cases")
