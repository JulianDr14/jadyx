// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/textures/etc2.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace Tegra::Texture::ETC2 {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

/// ETC1 intensity modifiers {a, b}; pixel index 0..3 picks +a, +b, -a, -b.
constexpr std::array<std::array<s32, 2>, 8> INTENSITY{{
    {2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183},
}};

/// T and H mode distances.
constexpr std::array<s32, 8> DISTANCE{3, 6, 11, 16, 23, 32, 41, 64};

/// EAC modifiers, shared by the ETC2 alpha block and the R11/RG11 channels.
constexpr std::array<std::array<s32, 8>, 16> EAC_MODIFIERS{{
    {-3, -6, -9, -15, 2, 5, 8, 14},
    {-3, -7, -10, -13, 2, 6, 9, 12},
    {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12},
    {-3, -6, -8, -12, 2, 5, 7, 11},
    {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10},
    {-3, -5, -8, -11, 2, 4, 7, 10},
    {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},
    {-2, -4, -8, -10, 1, 3, 7, 9},
    {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},
    {-1, -2, -3, -10, 0, 1, 2, 9},
    {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8},
}};

u64 ReadBigEndian(const u8* bytes) {
    u64 value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

constexpr u32 Bits(u64 value, int high, int low) {
    return static_cast<u32>((value >> low) & ((u64{1} << (high - low + 1)) - 1));
}

constexpr s32 Clamp255(s32 value) {
    return std::clamp(value, 0, 255);
}

constexpr s32 Extend4(u32 value) {
    return static_cast<s32>(value * 17);
}
constexpr s32 Extend5(u32 value) {
    return static_cast<s32>((value << 3) | (value >> 2));
}
constexpr s32 Extend6(u32 value) {
    return static_cast<s32>((value << 2) | (value >> 4));
}
constexpr s32 Extend7(u32 value) {
    return static_cast<s32>((value << 1) | (value >> 6));
}

struct Rgb {
    s32 r, g, b;
};

constexpr u32 Pack(Rgb c, u32 alpha = 255) {
    return static_cast<u32>(Clamp255(c.r)) | (static_cast<u32>(Clamp255(c.g)) << 8) |
           (static_cast<u32>(Clamp255(c.b)) << 16) | (alpha << 24);
}

constexpr Rgb Offset(Rgb c, s32 d) {
    return {c.r + d, c.g + d, c.b + d};
}

/// The RGB part of an ETC2 block into rgba[y * 4 + x]. Alpha comes out 255, or 0 for the
/// transparent texels of a punchthrough block (whose bit 33 says "opaque" instead of "diff").
void DecodeColor(u64 block, bool punchthrough, u32* rgba) {
    const u32 indices_msb = Bits(block, 31, 16);
    const u32 indices_lsb = Bits(block, 15, 0);
    // Texel (x, y) is bit x * 4 + y of each index plane.
    const auto index_at = [&](u32 x, u32 y) {
        const u32 bit = x * 4 + y;
        return (((indices_msb >> bit) & 1) << 1) | ((indices_lsb >> bit) & 1);
    };
    const bool flag33 = (block >> 33) & 1;
    const bool differential = punchthrough || flag33;
    const bool opaque = !punchthrough || flag33;

    // Four paint colors picked by the index: T, H and the punchthrough transparent index.
    const auto paint = [&](const std::array<u32, 4>& colors) {
        for (u32 y = 0; y < 4; ++y) {
            for (u32 x = 0; x < 4; ++x) {
                const u32 index = index_at(x, y);
                rgba[y * 4 + x] = !opaque && index == 2 ? 0 : colors[index];
            }
        }
    };

    if (differential) {
        const s32 r = static_cast<s32>(Bits(block, 63, 59));
        const s32 g = static_cast<s32>(Bits(block, 55, 51));
        const s32 b = static_cast<s32>(Bits(block, 47, 43));
        const auto delta = [&](int high) {
            const s32 d = static_cast<s32>(Bits(block, high, high - 2));
            return d >= 4 ? d - 8 : d;
        };
        const s32 r2 = r + delta(58);
        const s32 g2 = g + delta(50);
        const s32 b2 = b + delta(42);
        if (r2 < 0 || r2 > 31) {
            // T mode.
            const Rgb c1{Extend4((Bits(block, 60, 59) << 2) | Bits(block, 57, 56)),
                         Extend4(Bits(block, 55, 52)), Extend4(Bits(block, 51, 48))};
            const Rgb c2{Extend4(Bits(block, 47, 44)), Extend4(Bits(block, 43, 40)),
                         Extend4(Bits(block, 39, 36))};
            const s32 d = DISTANCE[(Bits(block, 35, 34) << 1) | Bits(block, 32, 32)];
            paint({Pack(c1), Pack(Offset(c2, d)), Pack(c2), Pack(Offset(c2, -d))});
            return;
        }
        if (g2 < 0 || g2 > 31) {
            // H mode. The order of the two base colors carries the distance's lowest bit.
            const u32 r1 = Bits(block, 62, 59);
            const u32 g1 = (Bits(block, 58, 56) << 1) | Bits(block, 52, 52);
            const u32 b1 = (Bits(block, 51, 51) << 3) | Bits(block, 49, 47);
            const u32 r2h = Bits(block, 46, 43);
            const u32 g2h = Bits(block, 42, 39);
            const u32 b2h = Bits(block, 38, 35);
            const u32 order = ((r1 << 8) | (g1 << 4) | b1) >= ((r2h << 8) | (g2h << 4) | b2h);
            const s32 d =
                DISTANCE[(Bits(block, 34, 34) << 2) | (Bits(block, 32, 32) << 1) | order];
            const Rgb c1{Extend4(r1), Extend4(g1), Extend4(b1)};
            const Rgb c2{Extend4(r2h), Extend4(g2h), Extend4(b2h)};
            paint({Pack(Offset(c1, d)), Pack(Offset(c1, -d)), Pack(Offset(c2, d)),
                   Pack(Offset(c2, -d))});
            return;
        }
        if (b2 < 0 || b2 > 31) {
            // Planar mode: always opaque, a gradient from three colors.
            const Rgb o{Extend6(Bits(block, 62, 57)),
                        Extend7((Bits(block, 56, 56) << 6) | Bits(block, 54, 49)),
                        Extend6((Bits(block, 48, 48) << 5) | (Bits(block, 44, 43) << 3) |
                                Bits(block, 41, 39))};
            const Rgb h{Extend6((Bits(block, 38, 34) << 1) | Bits(block, 32, 32)),
                        Extend7(Bits(block, 31, 25)), Extend6(Bits(block, 24, 19))};
            const Rgb v{Extend6(Bits(block, 18, 13)), Extend7(Bits(block, 12, 6)),
                        Extend6(Bits(block, 5, 0))};
            for (s32 y = 0; y < 4; ++y) {
                for (s32 x = 0; x < 4; ++x) {
                    const auto lerp = [&](s32 oc, s32 hc, s32 vc) {
                        return (x * (hc - oc) + y * (vc - oc) + 4 * oc + 2) >> 2;
                    };
                    rgba[y * 4 + x] =
                        Pack({lerp(o.r, h.r, v.r), lerp(o.g, h.g, v.g), lerp(o.b, h.b, v.b)});
                }
            }
            return;
        }
    }

    // Individual or differential: two subblocks, side by side or (flipped) stacked.
    std::array<Rgb, 2> base;
    if (differential) {
        base[0] = {Extend5(Bits(block, 63, 59)), Extend5(Bits(block, 55, 51)),
                   Extend5(Bits(block, 47, 43))};
        const auto second = [&](int high, int delta_high) {
            const s32 d = static_cast<s32>(Bits(block, delta_high, delta_high - 2));
            return Extend5(static_cast<u32>(static_cast<s32>(Bits(block, high, high - 4)) +
                                            (d >= 4 ? d - 8 : d)));
        };
        base[1] = {second(63, 58), second(55, 50), second(47, 42)};
    } else {
        base[0] = {Extend4(Bits(block, 63, 60)), Extend4(Bits(block, 55, 52)),
                   Extend4(Bits(block, 47, 44))};
        base[1] = {Extend4(Bits(block, 59, 56)), Extend4(Bits(block, 51, 48)),
                   Extend4(Bits(block, 43, 40))};
    }
    const std::array<u32, 2> tables{Bits(block, 39, 37), Bits(block, 36, 34)};
    const bool flip = (block >> 32) & 1;
    // Per subblock, the four colors the index picks; a non-opaque punchthrough block has no
    // +-a step (index 0 is the base color) and index 2 is transparent.
    std::array<std::array<u32, 4>, 2> colors;
    for (u32 sub = 0; sub < 2; ++sub) {
        const auto [a, b] = INTENSITY[tables[sub]];
        const s32 step = opaque ? a : 0;
        colors[sub] = {Pack(Offset(base[sub], step)), Pack(Offset(base[sub], b)),
                       opaque ? Pack(Offset(base[sub], -a)) : 0, Pack(Offset(base[sub], -b))};
    }
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u32 sub = flip ? (y >= 2) : (x >= 2);
            rgba[y * 4 + x] = colors[sub][index_at(x, y)];
        }
    }
}

/// The 16 EAC 3-bit indices of a block; texel (x, y) is index x * 4 + y, from the top bits down.
template <typename Emit>
void ForEachEacTexel(u64 block, Emit&& emit) {
    const auto& modifiers = EAC_MODIFIERS[Bits(block, 51, 48)];
    for (u32 x = 0; x < 4; ++x) {
        for (u32 y = 0; y < 4; ++y) {
            const int high = 47 - static_cast<int>(x * 4 + y) * 3;
            emit(y * 4 + x, modifiers[Bits(block, high, high - 2)]);
        }
    }
}

/// ETC2 RGBA8's alpha block: 8-bit base plus modifier times multiplier.
void DecodeAlpha(u64 block, u32* rgba) {
    const s32 base = static_cast<s32>(Bits(block, 63, 56));
    const s32 multiplier = static_cast<s32>(Bits(block, 55, 52));
    ForEachEacTexel(block, [&](u32 texel, s32 modifier) {
        const u32 alpha = static_cast<u32>(Clamp255(base + modifier * multiplier));
        rgba[texel] = (rgba[texel] & 0x00FFFFFF) | (alpha << 24);
    });
}

/// One EAC R11 channel at full precision: 11 bits widened to 16 by bit replication.
void DecodeEac(u64 block, bool is_signed, u16* out, u32 stride) {
    const s32 multiplier = static_cast<s32>(Bits(block, 55, 52));
    if (!is_signed) {
        const s32 base = static_cast<s32>(Bits(block, 63, 56)) * 8 + 4;
        ForEachEacTexel(block, [&](u32 texel, s32 modifier) {
            const s32 step = multiplier != 0 ? modifier * multiplier * 8 : modifier;
            const u32 value = static_cast<u32>(std::clamp(base + step, 0, 2047));
            out[texel * stride] = static_cast<u16>((value << 5) | (value >> 6));
        });
        return;
    }
    // The signed base is two's complement with -128 read as -127, so the range is symmetric.
    const s32 base = std::max(static_cast<s32>(static_cast<std::int8_t>(Bits(block, 63, 56))),
                              -127) * 8;
    ForEachEacTexel(block, [&](u32 texel, s32 modifier) {
        const s32 step = multiplier != 0 ? modifier * multiplier * 8 : modifier;
        const s32 value = std::clamp(base + step, -1023, 1023);
        const s32 magnitude = value < 0 ? -value : value;
        const s32 widened = (magnitude << 5) | (magnitude >> 5);
        out[texel * stride] = static_cast<u16>(static_cast<std::int16_t>(value < 0 ? -widened
                                                                                  : widened));
    });
}

} // Anonymous namespace

void DecodeBlock(Kind kind, const std::uint8_t* block, std::uint8_t* texels) {
    switch (kind) {
    case Kind::Rgb:
    case Kind::RgbPunchthrough:
    case Kind::Rgba: {
        std::array<u32, 16> rgba;
        const bool with_alpha = kind == Kind::Rgba;
        DecodeColor(ReadBigEndian(block + (with_alpha ? 8 : 0)), kind == Kind::RgbPunchthrough,
                    rgba.data());
        if (with_alpha) {
            DecodeAlpha(ReadBigEndian(block), rgba.data());
        }
        std::memcpy(texels, rgba.data(), sizeof(rgba));
        return;
    }
    case Kind::R11:
    case Kind::R11Signed: {
        std::array<u16, 16> red;
        DecodeEac(ReadBigEndian(block), kind == Kind::R11Signed, red.data(), 1);
        std::memcpy(texels, red.data(), sizeof(red));
        return;
    }
    case Kind::Rg11:
    case Kind::Rg11Signed: {
        std::array<u16, 32> rg;
        const bool is_signed = kind == Kind::Rg11Signed;
        DecodeEac(ReadBigEndian(block), is_signed, rg.data(), 2);
        DecodeEac(ReadBigEndian(block + 8), is_signed, rg.data() + 1, 2);
        std::memcpy(texels, rg.data(), sizeof(rg));
        return;
    }
    }
}

void Decompress(Kind kind, std::span<const std::uint8_t> input, std::uint32_t width,
                std::uint32_t height, std::uint32_t slices, std::span<std::uint8_t> output) {
    const std::size_t block_bytes = BlockBytes(kind);
    const std::size_t texel_bytes = TexelBytes(kind);
    const u32 blocks_wide = (width + 3) / 4;
    const u32 blocks_high = (height + 3) / 4;
    const std::size_t row_pitch = std::size_t{width} * texel_bytes;
    const std::size_t slice_pitch = row_pitch * height;
    const std::size_t slice_blocks = std::size_t{blocks_wide} * blocks_high;
    std::array<u8, 16 * 4> texels;
    for (u32 slice = 0; slice < slices; ++slice) {
        for (u32 by = 0; by < blocks_high; ++by) {
            const u32 rows = std::min(4U, height - by * 4);
            for (u32 bx = 0; bx < blocks_wide; ++bx) {
                const std::size_t block_index =
                    slice * slice_blocks + std::size_t{by} * blocks_wide + bx;
                if ((block_index + 1) * block_bytes > input.size()) {
                    return;
                }
                DecodeBlock(kind, input.data() + block_index * block_bytes, texels.data());
                const std::size_t columns = std::min(4U, width - bx * 4) * texel_bytes;
                for (u32 row = 0; row < rows; ++row) {
                    const std::size_t at = slice * slice_pitch + (by * 4 + row) * row_pitch +
                                           std::size_t{bx} * 4 * texel_bytes;
                    if (at + columns > output.size()) {
                        return;
                    }
                    std::memcpy(output.data() + at, texels.data() + row * 4 * texel_bytes,
                                columns);
                }
            }
        }
    }
}

} // namespace Tegra::Texture::ETC2
