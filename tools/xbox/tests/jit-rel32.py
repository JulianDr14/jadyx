# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the production call-range predicate against signed displacement boundaries."""

from jit_test_support import ROOT, function, run_cpp

predicate = function(
    (ROOT / "src/common/x64/xbyak.h").read_text(), "constexpr bool IsWithin2G"
)
run_cpp(
    r"""
#include <cassert>
#include <cstdint>
#include <limits>
#include <initializer_list>

using u64 = std::uint64_t;
using s64 = std::int64_t;
using s32 = std::int32_t;
// Windows headers can expose these macros before the emitter header.
#define min(a, b) unwanted_min_macro
#define max(a, b) unwanted_max_macro
"""
    + predicate
    + r"""
int main() {
    constexpr uintptr_t ref = 0x400000000ULL;
    for (s64 distance : {-0x100000001LL, -0x100000000LL, -0x80000001LL,
                          -0x80000000LL, -1LL, 0LL, 1LL, 0x7fffffffLL,
                          0x80000000LL, 0xffffffffLL, 0x100000000LL}) {
        const auto target = static_cast<uintptr_t>(ref + 5 + distance);
        const bool expected = distance >= -0x80000000LL && distance <= 0x7fffffffLL;
        assert(IsWithin2G(ref, target) == expected);
    }
}
"""
)
print("PASS: 11 signed rel32 boundary/near/far targets, including five-byte call bias")
