# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the production thunk's argument contract at every supported FP width."""

import re

from jit_test_support import BACKEND, function, run_cpp

source = (BACKEND / "emit_x64_floating_point.cpp").read_text()
thunk = function(source, "static u64 EmitFPRoundThunk")
packing = re.search(
    r"auto const extra_args = .*;", function(source, "static void EmitFPRound(")
)[0]
run_cpp(
    r"""
#include <cassert>
#include <cstdint>
#include <initializer_list>
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace FP {
enum class RoundingMode { Nearest, Up, Down, Zero, Away, Odd };
struct FPSR { unsigned observed_mode{}; bool observed_exact{}; u32 observed_fpcr{}; };
using FPCR = u32;
template<class FPT>
u64 FPRoundInt(FPT input, FPCR fpcr, RoundingMode mode, bool exact, FPSR& fpsr) {
    // Observe the utility-call boundary, without replacing the bitfield decoder.
    fpsr.observed_mode = static_cast<unsigned>(mode);
    fpsr.observed_exact = exact;
    fpsr.observed_fpcr = fpcr;
    return input;
}
}
template<typename FPT>
"""
    + thunk
    + "\nu32 pack(FP::RoundingMode rounding_mode, bool exact) {"
    + packing
    + "return extra_args;}\n"
    + r"""
template<class FPT> void check() {
    for (u32 mode = 0; mode < 6; ++mode) {
        for (u32 exact = 0; exact < 2; ++exact) {
            for (u32 unused : {0U, 0xa50000U}) {
                FP::FPSR fpsr{};
                constexpr u64 input = 0x7ff81234fedc5678ULL;
                const u32 packed = pack(static_cast<FP::RoundingMode>(mode), exact != 0) | unused;
                const u64 result = EmitFPRoundThunk<FPT>(input, fpsr, 0x03480000, packed);
                assert(fpsr.observed_mode == mode);
                assert(fpsr.observed_exact == (exact != 0));
                assert(fpsr.observed_fpcr == 0x03480000);
                assert(result == static_cast<FPT>(input));
            }
        }
    }
}
int main() { check<u16>(); check<u32>(); check<u64>(); }
"""
)
print(
    "PASS: 72 thunk dispatch contracts (3 widths, 6 modes, exactness, ignored high bits)"
)
