// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/ui/qr_code.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>

namespace Jadyx::Ui {
namespace {

constexpr int MaxVersion = 10;
// By level (M, H) and version (index 0 unused): error correction bytes per block, and blocks.
constexpr std::array<std::array<int, MaxVersion + 1>, 2> EccPerBlock{{
    {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26},
    {0, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28},
}};
constexpr std::array<std::array<int, MaxVersion + 1>, 2> Blocks{{
    {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5},
    {0, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8},
}};
constexpr std::array<int, 2> FormatBits{0b00, 0b10}; // The format field's two level bits.

std::size_t LevelIndex(QrLevel level) {
    return level == QrLevel::H ? 1 : 0;
}

/// Modules left for data and error correction once the function patterns are drawn.
int RawDataModules(int version) {
    int result = (16 * version + 128) * version + 64;
    if (version >= 2) {
        const int align = version / 7 + 2;
        result -= (25 * align - 10) * align - 55;
        if (version >= 7) result -= 36;
    }
    return result;
}

int DataCodewords(QrLevel level, int version) {
    const auto l = LevelIndex(level);
    return RawDataModules(version) / 8 - EccPerBlock[l][version] * Blocks[l][version];
}

std::uint8_t GfMultiply(std::uint8_t x, std::uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11d);
        z ^= ((y >> i) & 1) * x;
    }
    return static_cast<std::uint8_t>(z);
}

std::vector<std::uint8_t> RsDivisor(int degree) {
    std::vector<std::uint8_t> result(static_cast<std::size_t>(degree));
    result.back() = 1;
    std::uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (std::size_t j = 0; j < result.size(); ++j) {
            result[j] = GfMultiply(result[j], root);
            if (j + 1 < result.size()) result[j] ^= result[j + 1];
        }
        root = GfMultiply(root, 0x02);
    }
    return result;
}

std::vector<std::uint8_t> RsRemainder(const std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& divisor) {
    std::vector<std::uint8_t> result(divisor.size());
    for (const std::uint8_t b : data) {
        const std::uint8_t factor = b ^ result.front();
        result.erase(result.begin());
        result.push_back(0);
        for (std::size_t i = 0; i < result.size(); ++i) result[i] ^= GfMultiply(divisor[i], factor);
    }
    return result;
}

std::vector<int> AlignmentPositions(int version) {
    if (version == 1) return {};
    const int count = version / 7 + 2;
    const int size = version * 4 + 17;
    const int step = (version * 4 + count * 2 + 1) / (count * 2 - 2) * 2;
    std::vector<int> result(static_cast<std::size_t>(count));
    result[0] = 6;
    for (int i = count - 1, pos = size - 7; i >= 1; --i, pos -= step) result[static_cast<std::size_t>(i)] = pos;
    return result;
}

bool MaskBit(int mask, int x, int y) {
    switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

class Builder {
public:
    Builder(QrLevel level_, int version_) : level{level_}, version{version_}, size{version_ * 4 + 17} {
        dark.assign(static_cast<std::size_t>(size * size), 0);
        function.assign(dark.size(), 0);
    }

    void Set(int x, int y, bool on) {
        dark[Index(x, y)] = on ? 1 : 0;
        function[Index(x, y)] = 1;
    }

    void DrawFunctionPatterns() {
        for (int i = 0; i < size; ++i) {
            Set(6, i, i % 2 == 0);
            Set(i, 6, i % 2 == 0);
        }
        Finder(3, 3);
        Finder(size - 4, 3);
        Finder(3, size - 4);
        const auto align = AlignmentPositions(version);
        const int last = static_cast<int>(align.size()) - 1;
        for (int i = 0; i <= last; ++i) {
            for (int j = 0; j <= last; ++j) {
                if ((i == 0 && j == 0) || (i == 0 && j == last) || (i == last && j == 0)) continue;
                Alignment(align[static_cast<std::size_t>(i)], align[static_cast<std::size_t>(j)]);
            }
        }
        DrawFormat(0); // Reserves the area; the real bits come with the mask.
        DrawVersion();
    }

    void DrawFormat(int mask) {
        const int data = FormatBits[LevelIndex(level)] << 3 | mask;
        int rem = data;
        for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        const int bits = (data << 10 | rem) ^ 0x5412;
        const auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };
        for (int i = 0; i <= 5; ++i) Set(8, i, bit(i));
        Set(8, 7, bit(6));
        Set(8, 8, bit(7));
        Set(7, 8, bit(8));
        for (int i = 9; i < 15; ++i) Set(14 - i, 8, bit(i));
        for (int i = 0; i < 8; ++i) Set(size - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; ++i) Set(8, size - 15 + i, bit(i));
        Set(8, size - 8, true);
    }

    void DrawCodewords(const std::vector<std::uint8_t>& data) {
        std::size_t i = 0;
        for (int right = size - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int vert = 0; vert < size; ++vert) {
                for (int j = 0; j < 2; ++j) {
                    const int x = right - j;
                    const bool upward = ((right + 1) & 2) == 0;
                    const int y = upward ? size - 1 - vert : vert;
                    if (!function[Index(x, y)] && i < data.size() * 8) {
                        dark[Index(x, y)] = (data[i >> 3] >> (7 - (i & 7))) & 1;
                        ++i;
                    }
                }
            }
        }
    }

    void ApplyMask(int mask) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                if (!function[Index(x, y)] && MaskBit(mask, x, y)) dark[Index(x, y)] ^= 1;
            }
        }
    }

    long Penalty() const {
        long result = 0;
        const auto at = [this](int x, int y) { return dark[Index(x, y)] != 0; };
        // Long runs and finder lookalikes, along rows and then columns.
        for (int pass = 0; pass < 2; ++pass) {
            for (int a = 0; a < size; ++a) {
                const auto get = [&](int b) { return pass == 0 ? at(b, a) : at(a, b); };
                int run = 1;
                for (int b = 1; b <= size; ++b) {
                    if (b < size && get(b) == get(b - 1)) {
                        ++run;
                        continue;
                    }
                    if (run >= 5) result += 3 + (run - 5);
                    run = 1;
                }
                for (int b = -4; b < size; ++b) {
                    // 1:1:3:1:1 dark-light-dark with four light modules on one side.
                    static constexpr std::array<bool, 11> Pattern{true, false, true, true, true, false, true,
                                                                  false, false, false, false};
                    const auto cell = [&](int k) { return k >= 0 && k < size && get(k); };
                    bool forward = true, backward = true;
                    for (int k = 0; k < 11; ++k) {
                        forward = forward && cell(b + k) == Pattern[static_cast<std::size_t>(k)];
                        backward = backward && cell(b + k) == Pattern[static_cast<std::size_t>(10 - k)];
                    }
                    if (forward) result += 40;
                    if (backward) result += 40;
                }
            }
        }
        // 2x2 blocks of one colour.
        for (int y = 0; y + 1 < size; ++y) {
            for (int x = 0; x + 1 < size; ++x) {
                const bool c = at(x, y);
                if (c == at(x + 1, y) && c == at(x, y + 1) && c == at(x + 1, y + 1)) result += 3;
            }
        }
        // Balance of dark and light.
        long dark_count = 0;
        for (const auto m : dark) dark_count += m;
        const long total = static_cast<long>(dark.size());
        const long k = (std::labs(dark_count * 20 - total * 10) + total - 1) / total - 1;
        result += k * 10;
        return result;
    }

    QrCode Result(int mask) const {
        return QrCode{.level = level, .version = version, .mask = mask, .size = size, .modules = dark};
    }

private:
    std::size_t Index(int x, int y) const {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x);
    }

    void Finder(int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                const int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= size || y >= size) continue;
                const int dist = std::max(std::abs(dx), std::abs(dy));
                Set(x, y, dist != 2 && dist != 4);
            }
        }
    }

    void Alignment(int cx, int cy) {
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) Set(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
        }
    }

    void DrawVersion() {
        if (version < 7) return;
        int rem = version;
        for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1f25);
        const long bits = static_cast<long>(version) << 12 | rem;
        for (int i = 0; i < 18; ++i) {
            const bool on = ((bits >> i) & 1) != 0;
            const int a = size - 11 + i % 3, b = i / 3;
            Set(a, b, on);
            Set(b, a, on);
        }
    }

    QrLevel level;
    int version;
    int size;
    std::vector<std::uint8_t> dark;
    std::vector<std::uint8_t> function;
};

/// The data codewords: byte mode, the length, the bytes, the terminator and the padding.
std::vector<std::uint8_t> DataBytes(std::string_view text, QrLevel level, int version) {
    std::vector<bool> bits;
    const auto put = [&bits](unsigned value, int count) {
        for (int i = count - 1; i >= 0; --i) bits.push_back(((value >> i) & 1) != 0);
    };
    put(0b0100, 4);
    put(static_cast<unsigned>(text.size()), version <= 9 ? 8 : 16);
    for (const char c : text) put(static_cast<unsigned char>(c), 8);
    const std::size_t capacity = static_cast<std::size_t>(DataCodewords(level, version)) * 8;
    put(0, static_cast<int>(std::min<std::size_t>(4, capacity - bits.size())));
    put(0, static_cast<int>((8 - bits.size() % 8) % 8));
    for (unsigned pad = 0xec; bits.size() < capacity; pad ^= 0xec ^ 0x11) put(pad, 8);
    std::vector<std::uint8_t> result(bits.size() / 8);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (bits[i]) result[i >> 3] |= static_cast<std::uint8_t>(1 << (7 - (i & 7)));
    }
    return result;
}

/// Splits into blocks, adds each block's error correction and interleaves them.
std::vector<std::uint8_t> Interleave(const std::vector<std::uint8_t>& data, QrLevel level, int version) {
    const int blocks = Blocks[LevelIndex(level)][version];
    const int ecc = EccPerBlock[LevelIndex(level)][version];
    const int raw = RawDataModules(version) / 8;
    const int short_blocks = blocks - raw % blocks;
    const int short_length = raw / blocks;
    const auto divisor = RsDivisor(ecc);
    std::vector<std::vector<std::uint8_t>> parts;
    std::size_t offset = 0;
    for (int i = 0; i < blocks; ++i) {
        const auto length = static_cast<std::size_t>(short_length - ecc + (i < short_blocks ? 0 : 1));
        std::vector<std::uint8_t> block(data.begin() + static_cast<std::ptrdiff_t>(offset),
                                        data.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
        const auto remainder = RsRemainder(block, divisor);
        if (i < short_blocks) block.push_back(0); // Keeps the columns aligned; skipped below.
        block.insert(block.end(), remainder.begin(), remainder.end());
        parts.push_back(std::move(block));
    }
    std::vector<std::uint8_t> result;
    for (std::size_t i = 0; i < parts[0].size(); ++i) {
        for (int j = 0; j < blocks; ++j) {
            if (i != static_cast<std::size_t>(short_length - ecc) || j >= short_blocks) {
                result.push_back(parts[static_cast<std::size_t>(j)][i]);
            }
        }
    }
    return result;
}

} // namespace

std::optional<QrCode> EncodeQr(std::string_view text, QrLevel level, int forced_mask) {
    if (text.empty() || forced_mask > 7) return std::nullopt;
    int version = 1;
    for (; version <= MaxVersion; ++version) {
        const std::size_t bits = 4 + (version <= 9 ? 8 : 16) + text.size() * 8;
        if (bits <= static_cast<std::size_t>(DataCodewords(level, version)) * 8) break;
    }
    if (version > MaxVersion) return std::nullopt;
    const auto codewords = Interleave(DataBytes(text, level, version), level, version);
    Builder builder{level, version};
    builder.DrawFunctionPatterns();
    builder.DrawCodewords(codewords);
    int mask = forced_mask;
    if (mask < 0) {
        long best = std::numeric_limits<long>::max();
        for (int candidate = 0; candidate < 8; ++candidate) {
            builder.ApplyMask(candidate);
            builder.DrawFormat(candidate);
            const long penalty = builder.Penalty();
            if (penalty < best) {
                best = penalty;
                mask = candidate;
            }
            builder.ApplyMask(candidate); // XOR again: undone.
        }
    }
    builder.ApplyMask(mask);
    builder.DrawFormat(mask);
    return builder.Result(mask);
}

int QrLogoModules(const QrCode& code) {
    // Version 1 has format bits beside its centre; from 7 an alignment pattern sits on it.
    if (code.level != QrLevel::H || code.version < 2 || code.version > 6) return 0;
    // By version: the largest odd square whose worst block keeps two thirds of what its
    // Reed-Solomon code can fix for the camera's own misreads (the gate counts them).
    static constexpr std::array<int, 7> Side{0, 0, 5, 7, 7, 9, 11};
    return Side[static_cast<std::size_t>(code.version)];
}

} // namespace Jadyx::Ui
