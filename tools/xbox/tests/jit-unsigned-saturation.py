# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare the production saturating shift with a wider integer oracle."""

from jit_test_support import BACKEND, function, run_cpp

source = (BACKEND / "emit_x64_vector.cpp").read_text()
shift = function(source, "static bool VectorSignedSaturatedShiftLeftUnsigned(")
run_cpp(
    r"""
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <type_traits>
using u8 = std::uint8_t;
using size_t = std::size_t;
template<class T> using VectorArray = std::array<T, 16 / sizeof(T)>;
template<class T> requires std::is_signed_v<T>
"""
    + shift
    + r"""
std::uint64_t cases = 0;
template<class T> void check(std::uint64_t bits, u8 amount) {
    using U = std::make_unsigned_t<T>;
    VectorArray<T> data{};
    data.fill(std::bit_cast<T>(static_cast<U>(bits)));
    VectorArray<T> output{};
    const bool qc = VectorSignedSaturatedShiftLeftUnsigned(output, data, amount);
    const auto element = data[0];
    const auto maximum = static_cast<unsigned __int128>(std::numeric_limits<U>::max());
    const auto wide = element < 0 ? 0 : static_cast<unsigned __int128>(element) << amount;
    const U expected = static_cast<U>(wide > maximum ? maximum : wide);
    assert(qc == (element < 0 || wide > maximum));
    for (T result : output) assert(std::bit_cast<U>(result) == expected);
    ++cases;
}
template<class T> void random_cases() {
    std::mt19937_64 random{0x1951};
    for (u8 amount = 0; amount < sizeof(T) * 8; ++amount) {
        for (std::uint64_t bits : {0ULL, 1ULL, 2ULL, 0x7fffffffULL, 0xffffffffULL,
                                   0x7fffffffffffffffULL, 0xffffffffffffffffULL})
            check<T>(bits, amount);
        for (unsigned trial = 0; trial < 4096; ++trial) check<T>(random(), amount);
    }
}
int main() {
    for (u8 amount = 0; amount < 8; ++amount)
        for (unsigned bits = 0; bits < 256; ++bits) check<std::int8_t>(bits, amount);
    for (u8 amount = 0; amount < 16; ++amount)
        for (unsigned bits = 0; bits < 65536; ++bits) check<std::int16_t>(bits, amount);
    random_cases<std::int32_t>();
    random_cases<std::int64_t>();
    std::printf("PASS: %llu saturation vectors, all 8/16-bit inputs, 32/64-bit boundaries/random\n",
                static_cast<unsigned long long>(cases));
}
"""
)
