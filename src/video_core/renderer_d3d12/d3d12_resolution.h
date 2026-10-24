// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace D3D12 {
// "Up" scaling also includes fractional settings (1/2 and 3/4), as in the shared cache.
struct ResolutionScale {
    std::uint32_t numerator{1};
    std::uint32_t shift{};

    float Factor() const {
        return static_cast<float>(numerator) / static_cast<float>(std::uint32_t{1} << shift);
    }
    std::uint32_t Size(std::uint32_t value) const {
        return value == 0 ? 0
                          : static_cast<std::uint32_t>(std::max(
                                std::uint64_t{1}, (std::uint64_t{value} * numerator) >> shift));
    }
    float Coordinate(float value) const { return value * Factor(); }
    struct Rectangle {
        std::int32_t left, top, right, bottom;
    };
    Rectangle Rect(std::int32_t left, std::int32_t top, std::int32_t right,
                   std::int32_t bottom) const {
        const auto x = static_cast<std::int32_t>(std::floor(Coordinate(static_cast<float>(left))));
        const auto y = static_cast<std::int32_t>(std::floor(Coordinate(static_cast<float>(top))));
        return {.left = x,
                .top = y,
                .right = right <= left ? x
                                       : static_cast<std::int32_t>(
                                             std::ceil(Coordinate(static_cast<float>(right)))),
                .bottom = bottom <= top ? y
                                        : static_cast<std::int32_t>(
                                              std::ceil(Coordinate(static_cast<float>(bottom))))};
    }
    Rectangle BlitRect(std::int32_t left, std::int32_t top, std::int32_t right,
                       std::int32_t bottom) const {
        const auto bounds = Rect(std::min(left, right), std::min(top, bottom),
                                 std::max(left, right), std::max(top, bottom));
        return {.left = left <= right ? bounds.left : bounds.right,
                .top = top <= bottom ? bounds.top : bounds.bottom,
                .right = left <= right ? bounds.right : bounds.left,
                .bottom = top <= bottom ? bounds.bottom : bounds.top};
    }
};
// Mirrors SPIR-V's four texture words and two image words. Kept independent of D3D12
// headers so descriptor ordering and the word boundary are covered by host tests.
struct ResolutionFlags {
    std::array<std::uint32_t, 6> words{};
    std::uint32_t texture_index{};
    std::uint32_t image_index{};

    void PushTexture(bool scaled) { Push(scaled, texture_index, 0, 4); }
    void PushImage(bool scaled) { Push(scaled, image_index, 4, 2); }

private:
    void Push(bool scaled, std::uint32_t& index, std::uint32_t offset, std::uint32_t count) {
        if (scaled && index < count * 32)
            words[offset + index / 32] |= 1U << (index % 32);
        ++index;
    }
};
} // namespace D3D12
