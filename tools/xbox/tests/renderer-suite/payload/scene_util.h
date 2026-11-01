// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

inline std::string Named(const char* prefix, const char* name) {
    return std::string{prefix} + name;
}

/// sRGB transfer function, for what a scene expects to read from an sRGB target as bytes.
double LinearToSrgb(double value);
double SrgbToLinear(double value);

/// A cell of a grid over the target: the left/top of cell `index` and its center.
struct Cell {
    uint32_t x, y, size;
    uint32_t CenterX() const { return x + size / 2; }
    uint32_t CenterY() const { return y + size / 2; }
};

inline Cell CellOf(uint32_t index, uint32_t columns = 4, uint32_t size = 8) {
    return {(index % columns) * size, (index / columns) * size, size};
}
