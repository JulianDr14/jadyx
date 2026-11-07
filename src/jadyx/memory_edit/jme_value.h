// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Jadyx Memory Edit: the number the player typed, and what it matches in memory. Integers match
// their exact bits, so signed and unsigned are one search. A decimal matches a range, as the game may
// have rounded or cut what it shows: 100 finds 99.5 to 101, 12.5 finds 12.45 to 12.6.

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "common/common_types.h"

namespace Jadyx::MemoryEdit {

enum class ValueKind : u8 { I8, I16, I32, I64, F32, F64 };
inline constexpr size_t VALUE_KINDS = 6;

[[nodiscard]] constexpr u32 ValueWidth(ValueKind kind) {
    switch (kind) {
    case ValueKind::I8:
        return 1;
    case ValueKind::I16:
        return 2;
    case ValueKind::I32:
    case ValueKind::F32:
        return 4;
    case ValueKind::I64:
    case ValueKind::F64:
        return 8;
    }
    return 1;
}

[[nodiscard]] constexpr bool IsFloat(ValueKind kind) {
    return kind == ValueKind::F32 || kind == ValueKind::F64;
}

struct Number {
    bool negative{};
    /// Written with a point: integers cannot hold it.
    bool has_point{};
    /// Without the sign. Empty when it has a point or does not fit in 64 bits.
    std::optional<u64> magnitude;
    double value{};
    u32 decimals{};
};

/// `-?digits[.digits]`, at most 32 characters: all the numeric pad can write.
[[nodiscard]] inline std::optional<Number> ParseNumber(std::string_view text) {
    if (text.empty() || text.size() > 32) return std::nullopt;
    Number number;
    number.negative = text.front() == '-';
    const std::string_view digits = number.negative ? text.substr(1) : text;
    bool any_digit = false;
    u64 magnitude = 0;
    bool fits = true;
    for (const char c : digits) {
        if (c == '.') {
            if (number.has_point) return std::nullopt;
            number.has_point = true;
            continue;
        }
        if (c < '0' || c > '9') return std::nullopt;
        any_digit = true;
        if (number.has_point) {
            ++number.decimals;
            continue;
        }
        const u64 digit = static_cast<u64>(c - '0');
        if (magnitude > (std::numeric_limits<u64>::max() - digit) / 10) fits = false;
        magnitude = magnitude * 10 + digit;
    }
    if (!any_digit) return std::nullopt;
    if (fits && !number.has_point) number.magnitude = magnitude;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number.value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return number;
}

struct Query {
    ValueKind kind{};
    u64 bits{};
    float low32{}, high32{};
    double low64{}, high64{};
};

namespace Detail {

// The smallest float not below `value`: for a float v, v < value is v < this.
[[nodiscard]] inline float FloatAtLeast(double value) {
    const float rounded = static_cast<float>(value);
    return static_cast<double>(rounded) < value
               ? std::nextafter(rounded, std::numeric_limits<float>::infinity())
               : rounded;
}

} // namespace Detail

/// Empty when `kind` cannot hold the number.
[[nodiscard]] inline std::optional<Query> MakeQuery(const Number& number, ValueKind kind) {
    Query query;
    query.kind = kind;
    if (!IsFloat(kind)) {
        if (!number.magnitude) return std::nullopt;
        const u32 bits = ValueWidth(kind) * 8;
        const u64 mask = bits == 64 ? ~u64{} : (u64{1} << bits) - 1;
        const u64 limit = number.negative ? u64{1} << (bits - 1) : mask;
        if (*number.magnitude > limit) return std::nullopt;
        query.bits = (number.negative ? u64{0} - *number.magnitude : *number.magnitude) & mask;
        return query;
    }
    const double value = number.value;
    const double step = std::pow(10.0, -static_cast<double>(number.decimals));
    const double low = number.negative ? value - step : value - step / 2;
    const double high = number.negative ? value + step / 2 : value + step;
    if (kind == ValueKind::F64) {
        // Past the precision of the type the range is empty, and the number itself must still match.
        query.low64 = (std::min)(low, value);
        query.high64 = (std::max)(high, std::nextafter(value, std::numeric_limits<double>::infinity()));
        return query;
    }
    const float nearest = static_cast<float>(value);
    if (!std::isfinite(nearest)) return std::nullopt;
    query.low32 = (std::min)(Detail::FloatAtLeast(low), nearest);
    query.high32 = (std::max)(Detail::FloatAtLeast(high),
                              std::nextafter(nearest, std::numeric_limits<float>::infinity()));
    return query;
}

/// The kinds one search looks for.
struct QuerySet {
    std::array<Query, VALUE_KINDS> items{};
    u32 count{};

    void Add(const std::optional<Query>& query) {
        if (query) items[count++] = *query;
    }
};

/// What a search looks for unless the player picks a kind: 32-bit integers, which also find small
/// 64-bit ones, or 64-bit when it does not fit, and both decimals. 8 and 16 bits find too much.
[[nodiscard]] inline QuerySet AutoQueries(const Number& number) {
    QuerySet set;
    const auto wide = MakeQuery(number, ValueKind::I32);
    set.Add(wide ? wide : MakeQuery(number, ValueKind::I64));
    set.Add(MakeQuery(number, ValueKind::F32));
    set.Add(MakeQuery(number, ValueKind::F64));
    return set;
}

/// The bits that hold the number as `kind`, for writing it. Unlike a search, a decimal is exact. Empty
/// when `kind` cannot hold it.
[[nodiscard]] inline std::optional<u64> EncodeValue(const Number& number, ValueKind kind) {
    if (kind == ValueKind::F32) {
        const float value = static_cast<float>(number.value);
        if (!std::isfinite(value)) return std::nullopt;
        return std::bit_cast<u32>(value);
    }
    if (kind == ValueKind::F64) return std::bit_cast<u64>(number.value);
    const auto query = MakeQuery(number, kind);
    if (!query) return std::nullopt;
    return query->bits;
}

/// What memory holds, as text. Bits do not tell an integer's sign: `is_signed` follows what the player
/// typed, so a -5 they searched for reads as -5.
[[nodiscard]] inline std::string FormatValue(ValueKind kind, u64 bits, bool is_signed) {
    std::array<char, 64> text{};
    const auto write = [&text](auto value) {
        const auto [end, error] = std::to_chars(text.data(), text.data() + text.size(), value);
        return error == std::errc{} ? std::string(text.data(), end) : std::string{};
    };
    if (kind == ValueKind::F32) return write(std::bit_cast<float>(static_cast<u32>(bits)));
    if (kind == ValueKind::F64) return write(std::bit_cast<double>(bits));
    const u32 width = ValueWidth(kind) * 8;
    if (width < 64) bits &= (u64{1} << width) - 1;
    if (!is_signed) return write(bits);
    return write(static_cast<s64>(bits << (64 - width)) >> (64 - width));
}

} // namespace Jadyx::MemoryEdit
