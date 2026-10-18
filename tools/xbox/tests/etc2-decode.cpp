// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
#include "video_core/textures/etc2.h"

using namespace Tegra::Texture::ETC2;

namespace {
void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

/// A 64-bit block built field by field, stored big-endian as the format does.
struct Block {
    std::uint64_t bits = 0;
    Block& Set(int high, int low, std::uint64_t value) {
        const std::uint64_t mask = ((std::uint64_t{1} << (high - low + 1)) - 1) << low;
        bits = (bits & ~mask) | ((value << low) & mask);
        return *this;
    }
    /// ETC color block: texel (x, y) takes index 0..3 (MSB plane at 31-16, LSB at 15-0).
    Block& Index(int x, int y, int index) {
        const int bit = x * 4 + y;
        Set(16 + bit, 16 + bit, (index >> 1) & 1);
        return Set(bit, bit, index & 1);
    }
    /// EAC block: texel (x, y) takes a 3-bit index.
    Block& EacIndex(int x, int y, int index) {
        const int high = 47 - (x * 4 + y) * 3;
        return Set(high, high - 2, index);
    }
    void Store(std::uint8_t* out) const {
        for (int i = 0; i < 8; ++i) {
            out[i] = static_cast<std::uint8_t>(bits >> (56 - i * 8));
        }
    }
};

struct Rgba {
    int r, g, b, a;
};

std::array<std::uint32_t, 16> DecodeColor(Kind kind, const Block& color, const Block* alpha = nullptr) {
    std::array<std::uint8_t, 16> bytes{};
    if (alpha) {
        alpha->Store(bytes.data());
        color.Store(bytes.data() + 8);
    } else {
        color.Store(bytes.data());
    }
    std::array<std::uint32_t, 16> texels;
    DecodeBlock(kind, bytes.data(), reinterpret_cast<std::uint8_t*>(texels.data()));
    return texels;
}

bool Is(std::uint32_t texel, Rgba c) {
    return (texel & 0xFF) == static_cast<std::uint32_t>(c.r) &&
           ((texel >> 8) & 0xFF) == static_cast<std::uint32_t>(c.g) &&
           ((texel >> 16) & 0xFF) == static_cast<std::uint32_t>(c.b) &&
           (texel >> 24) == static_cast<std::uint32_t>(c.a);
}

std::array<std::uint16_t, 16> DecodeR11(const Block& block, bool is_signed) {
    std::array<std::uint8_t, 8> bytes;
    block.Store(bytes.data());
    std::array<std::uint16_t, 16> texels;
    DecodeBlock(is_signed ? Kind::R11Signed : Kind::R11, bytes.data(),
                reinterpret_cast<std::uint8_t*>(texels.data()));
    return texels;
}
} // namespace

int main() {
    // Individual mode: two 4-bit colors side by side, table 0 (+-2, +-8).
    {
        Block b;
        b.Set(63, 60, 0xF).Set(59, 56, 0x0).Set(55, 52, 0x8).Set(51, 48, 0x1)
            .Set(47, 44, 0x2).Set(43, 40, 0x3).Set(39, 37, 0).Set(36, 34, 0).Set(33, 33, 0)
            .Set(32, 32, 0);
        b.Index(3, 3, 3);
        const auto t = DecodeColor(Kind::Rgb, b);
        Check(Is(t[0], {255, 138, 36, 255}), "individual left subblock, +a clamps");
        Check(Is(t[2], {2, 19, 53, 255}), "individual right subblock");
        Check(Is(t[15], {0, 9, 43, 255}), "individual -b");
    }
    // Differential mode, flipped (stacked subblocks), negative delta, table 7.
    {
        Block b;
        b.Set(63, 59, 16).Set(58, 56, 7).Set(55, 51, 16).Set(50, 48, 7).Set(47, 43, 16)
            .Set(42, 40, 7).Set(39, 37, 7).Set(36, 34, 7).Set(33, 33, 1).Set(32, 32, 1);
        b.Index(0, 3, 3).Index(1, 0, 1);
        const auto t = DecodeColor(Kind::Rgb, b);
        Check(Is(t[0], {132 + 47, 132 + 47, 132 + 47, 255}), "differential top subblock");
        Check(Is(t[1], {255, 255, 255, 255}), "differential +b clamps");
        Check(Is(t[3 * 4 + 0], {0, 0, 0, 255}), "differential bottom subblock -b");
        Check(Is(t[2 * 4 + 2], {123 + 47, 123 + 47, 123 + 47, 255}), "differential delta -1");
    }
    // T mode: R + dR overflows.
    {
        Block b;
        b.Set(63, 59, 0b00011).Set(58, 56, 0b100).Set(55, 52, 0x8).Set(51, 48, 0x4)
            .Set(47, 44, 0xA).Set(43, 40, 0x5).Set(39, 36, 0x0).Set(35, 34, 0b01).Set(33, 33, 1)
            .Set(32, 32, 1);
        for (int x = 0; x < 4; ++x) {
            for (int y = 0; y < 4; ++y) {
                b.Index(x, y, 1);
            }
        }
        b.Index(0, 0, 0).Index(3, 3, 3);
        const auto t = DecodeColor(Kind::Rgb, b);
        Check(Is(t[0], {204, 136, 68, 255}), "T mode first color");
        Check(Is(t[1], {186, 101, 16, 255}), "T mode second color plus distance");
        Check(Is(t[15], {154, 69, 0, 255}), "T mode second color minus distance");
    }
    // H mode: G + dG overflows; the base color order adds the distance's low bit.
    {
        Block b;
        b.Set(63, 63, 0).Set(62, 59, 0b1000).Set(58, 56, 0b000).Set(55, 53, 0b111)
            .Set(52, 52, 1).Set(51, 51, 1).Set(50, 50, 0).Set(49, 47, 0b010).Set(46, 43, 0x2)
            .Set(42, 39, 0x3).Set(38, 35, 0x4).Set(34, 34, 1).Set(33, 33, 1).Set(32, 32, 0);
        b.Index(0, 0, 0).Index(1, 0, 1).Index(2, 0, 2).Index(3, 0, 3);
        const auto t = DecodeColor(Kind::Rgb, b);
        Check(Is(t[0], {168, 49, 202, 255}), "H mode first color plus distance");
        Check(Is(t[1], {104, 0, 138, 255}), "H mode first color minus distance");
        Check(Is(t[2], {66, 83, 100, 255}), "H mode second color plus distance");
        Check(Is(t[3], {2, 19, 36, 255}), "H mode second color minus distance");
    }
    // Planar mode: B + dB overflows; origin, horizontal and vertical colors.
    {
        Block b;
        b.Set(47, 45, 0b111).Set(44, 43, 0b11).Set(42, 42, 0).Set(41, 39, 0b011)
            .Set(38, 34, 0b11111).Set(33, 33, 1).Set(32, 32, 1).Set(12, 6, 127);
        const auto t = DecodeColor(Kind::Rgb, b);
        Check(Is(t[0], {0, 0, 109, 255}), "planar origin");
        Check(Is(t[3], {191, 0, 27, 255}), "planar horizontal");
        Check(Is(t[12], {0, 191, 27, 255}), "planar vertical");
        Check(Is(t[15], {191, 191, 0, 255}), "planar corner clamps");
    }
    // Punchthrough: without the opaque bit, index 2 is transparent and index 0 has no step.
    {
        Block b;
        b.Set(63, 59, 16).Set(55, 51, 16).Set(47, 43, 16).Set(33, 33, 0);
        b.Index(1, 0, 1).Index(2, 0, 2).Index(3, 0, 3);
        auto t = DecodeColor(Kind::RgbPunchthrough, b);
        Check(Is(t[0], {132, 132, 132, 255}), "punchthrough index 0 is the base");
        Check(Is(t[1], {140, 140, 140, 255}), "punchthrough +b");
        Check(t[2] == 0, "punchthrough transparent");
        Check(Is(t[3], {124, 124, 124, 255}), "punchthrough -b");
        b.Set(33, 33, 1);
        t = DecodeColor(Kind::RgbPunchthrough, b);
        Check(Is(t[0], {134, 134, 134, 255}) && Is(t[2], {130, 130, 130, 255}),
              "punchthrough opaque block decodes as differential");
    }
    // RGBA8: the EAC alpha block in front of the color block.
    {
        Block alpha;
        alpha.Set(63, 56, 200).Set(55, 52, 3).Set(51, 48, 0);
        for (int x = 0; x < 4; ++x) {
            for (int y = 0; y < 4; ++y) {
                alpha.EacIndex(x, y, 7);
            }
        }
        alpha.EacIndex(0, 1, 3);
        Block color;
        color.Set(63, 59, 16).Set(55, 51, 16).Set(47, 43, 16).Set(33, 33, 1);
        const auto t = DecodeColor(Kind::Rgba, color, &alpha);
        Check(Is(t[0], {134, 134, 134, 242}), "alpha base plus modifier");
        Check((t[4] >> 24) == 155, "alpha index order is column major");
    }
    // EAC R11 at full precision.
    {
        Block b;
        b.Set(63, 56, 128).Set(55, 52, 2).Set(51, 48, 0).EacIndex(0, 0, 3).EacIndex(1, 0, 4);
        auto t = DecodeR11(b, false);
        Check(t[0] == ((788 << 5) | (788 >> 6)) && t[1] == ((1060 << 5) | (1060 >> 6)),
              "R11 unsigned");
        b.Set(55, 52, 0).EacIndex(0, 0, 7);
        t = DecodeR11(b, false);
        Check(t[0] == ((1042 << 5) | (1042 >> 6)), "R11 multiplier 0 steps by one");
        b.Set(63, 56, 255).Set(55, 52, 15);
        t = DecodeR11(b, false);
        Check(t[0] == 0xFFFF, "R11 clamps at 2047 and widens to 0xFFFF");
        Block s;
        s.Set(63, 56, 0x80).Set(55, 52, 1).Set(51, 48, 0).EacIndex(0, 0, 3);
        t = DecodeR11(s, true);
        Check(static_cast<std::int16_t>(t[0]) == -32767, "R11 signed -128 reads as -127, clamps");
        s.Set(63, 56, 0).EacIndex(0, 0, 4);
        t = DecodeR11(s, true);
        Check(static_cast<std::int16_t>(t[0]) == 512, "R11 signed positive");
    }
    // RG11: two R11 blocks, interleaved.
    {
        std::array<std::uint8_t, 16> bytes;
        Block r, g;
        r.Set(63, 56, 255).Set(55, 52, 15).EacIndex(0, 0, 7);
        g.Set(63, 56, 0).Set(55, 52, 15).EacIndex(0, 0, 3);
        r.Store(bytes.data());
        g.Store(bytes.data() + 8);
        std::array<std::uint16_t, 32> t;
        DecodeBlock(Kind::Rg11, bytes.data(), reinterpret_cast<std::uint8_t*>(t.data()));
        Check(t[0] == 0xFFFF && t[1] == 0, "RG11 interleaves red and green");
    }
    // Edges: a 5x3 image of 2x1 blocks keeps only the texels inside it.
    {
        Block left, right;
        left.Set(63, 60, 0x1).Set(59, 56, 0x1).Set(33, 33, 0);
        right.Set(63, 60, 0x2).Set(59, 56, 0x2).Set(33, 33, 0);
        std::vector<std::uint8_t> input(16);
        left.Store(input.data());
        right.Store(input.data() + 8);
        std::vector<std::uint32_t> output(5 * 3, 0xDEADBEEF);
        Decompress(Kind::Rgb, input, 5, 3, 1,
                   {reinterpret_cast<std::uint8_t*>(output.data()), output.size() * 4});
        Check((output[3] & 0xFF) == 17 + 2 && (output[4] & 0xFF) == 34 + 2 &&
                  (output[2 * 5 + 4] & 0xFF) == 34 + 2,
              "edge blocks clipped into the row pitch");
    }
    // Every random block decodes; punchthrough alpha is 0 or 255 and transparent texels are 0.
    std::mt19937_64 random{12345};
    for (int i = 0; i < 200000; ++i) {
        Block b;
        b.bits = random();
        const auto t = DecodeColor(Kind::RgbPunchthrough, b);
        for (const std::uint32_t texel : t) {
            Check((texel >> 24) == 255 || texel == 0, "punchthrough alpha");
        }
    }

    // Throughput: a 2048x2048 RGBA8 texture (the biggest kind) and an RG11 one.
    {
        constexpr std::uint32_t SIZE = 2048;
        std::vector<std::uint8_t> input(SIZE / 4 * SIZE / 4 * 16);
        for (auto& byte : input) {
            byte = static_cast<std::uint8_t>(random());
        }
        std::vector<std::uint8_t> output(SIZE * SIZE * 4);
        for (const Kind kind : {Kind::Rgba, Kind::Rg11, Kind::Rgb}) {
            constexpr int RUNS = 5;
            const auto start = std::chrono::steady_clock::now();
            for (int run = 0; run < RUNS; ++run) {
                Decompress(kind, input, SIZE, SIZE, 1, output);
            }
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count() /
                              RUNS;
            const char* name = kind == Kind::Rgba ? "RGBA8" : kind == Kind::Rg11 ? "RG11" : "RGB8";
            std::printf("%s %ux%u: %.2f ms (%.0f Mtexels/s)\n", name, SIZE, SIZE, ms,
                        SIZE * SIZE / ms / 1000.0);
        }
    }
    std::puts("ETC2/EAC decode: individual, differential, T, H, planar, punchthrough, alpha, "
              "R11/RG11, edges PASS");
}
