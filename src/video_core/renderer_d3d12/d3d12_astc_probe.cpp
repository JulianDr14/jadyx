// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <barrier>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <d3d12sdklayers.h>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp11>

#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/host_shaders/d3d12_astc_native_comp_spv.h"
#include "video_core/host_shaders/d3d12_astc_native_frag_spv.h"
#include "video_core/host_shaders/d3d12_astc_probe_vert_spv.h"
#include "video_core/host_shaders/d3d12_astc_reference_comp_spv.h"
#include "video_core/host_shaders/d3d12_astc_reference_frag_spv.h"
#include "video_core/host_shaders/d3d12_astc_sample_comp_spv.h"
#include "video_core/host_shaders/d3d12_astc_sample_frag_spv.h"
#include "video_core/renderer_d3d12/d3d12_astc_guest.h"
#include "video_core/renderer_d3d12/d3d12_astc_probe.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/textures/astc.h"

namespace D3D12 {
namespace {
using Clock = std::chrono::steady_clock;
using Color = std::array<float, 4>;
using Block = std::array<u32, 4>;
constexpr auto ReadState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// Expand the native probe's fetch site into distinct x-neighbour fetches. Together with its two
// LOD sample sites, this reproduces the 5/13 static call counts seen in Game D, without a guest ROM.
std::vector<u32> ExpandProbeFetch(std::span<const u32> source, u32 calls) {
    if (!calls)
        return {source.begin(), source.end()};
    using Instruction = std::vector<u32>;
    std::vector<Instruction> instructions;
    std::unordered_map<u32, u32> types;
    std::unordered_map<u32, Instruction> declarations;
    for (size_t p = 5; p < source.size();) {
        const u32 size = source[p] >> 16;
        if (!size || p + size > source.size())
            throw std::runtime_error("multi-fetch SPIR-V malformed");
        Instruction i{source.begin() + p, source.begin() + p + size};
        bool result{}, type{};
        spv::HasResultAndType(static_cast<spv::Op>(i[0] & 65535), &result, &type);
        if (result) {
            const u32 id = i[type ? 2 : 1];
            declarations[id] = i;
            if (type)
                types[id] = i[1];
        }
        instructions.push_back(std::move(i));
        p += size;
    }
    std::unordered_set<u32> bulk_image_ids;
    for (const auto& i : instructions)
        if ((i[0] & 65535) == static_cast<u32>(spv::Op::OpDecorate) &&
            i[2] == static_cast<u32>(spv::Decoration::Binding) && i[3] == 5)
            bulk_image_ids.insert(i[1]);
    for (const auto& i : instructions) {
        const u32 op = i[0] & 65535;
        if ((op == 61 || op == 65 || op == 66 || op == 83 || op == 86 || op == 100) &&
            i.size() > 3 && bulk_image_ids.contains(i[3]))
            bulk_image_ids.insert(i[2]);
    }
    auto site = std::find_if(instructions.begin(), instructions.end(), [&](const auto& i) {
        return (i[0] & 65535) == static_cast<u32>(spv::Op::OpImageFetch) &&
               bulk_image_ids.contains(i[3]);
    });
    if (site == instructions.end())
        throw std::runtime_error("multi-fetch site missing");
    const u32 taps = calls - 2, coordinate_type = types.at((*site)[4]);
    const u32 integer_type = declarations.at(coordinate_type)[2];
    const u32 float_type = declarations.at((*site)[1])[2];
    u32 bound = source[3];
    auto make = [](spv::Op op, std::initializer_list<u32> args) {
        Instruction i{(static_cast<u32>(args.size() + 1) << 16) | static_cast<u32>(op)};
        i.insert(i.end(), args);
        return i;
    };
    std::vector<Instruction> constants, replacement;
    const u32 zero = bound++, divisor = bound++;
    constants.push_back(make(spv::Op::OpConstant, {integer_type, zero, 0}));
    constants.push_back(
        make(spv::Op::OpConstant, {float_type, divisor, std::bit_cast<u32>(1.f / taps)}));
    u32 accumulated{};
    for (u32 t = 0; t < taps; ++t) {
        const u32 x = bound++, offset = bound++, coordinate = bound++, sample = bound++;
        constants.push_back(make(spv::Op::OpConstant, {integer_type, x, t}));
        constants.push_back(
            make(spv::Op::OpConstantComposite, {coordinate_type, offset, x, zero, zero}));
        replacement.push_back(
            make(spv::Op::OpIAdd, {coordinate_type, coordinate, (*site)[4], offset}));
        auto fetch = *site;
        fetch[2] = sample;
        fetch[4] = coordinate;
        replacement.push_back(std::move(fetch));
        if (t) {
            const u32 sum = bound++;
            replacement.push_back(make(spv::Op::OpFAdd, {(*site)[1], sum, accumulated, sample}));
            accumulated = sum;
        } else
            accumulated = sample;
    }
    replacement.push_back(
        make(spv::Op::OpVectorTimesScalar, {(*site)[1], (*site)[2], accumulated, divisor}));
    std::vector<u32> out{source.begin(), source.begin() + 5};
    out[3] = bound;
    auto emit = [&](const auto& list) {
        for (const auto& i : list)
            out.insert(out.end(), i.begin(), i.end());
    };
    bool inserted = false;
    for (const auto& i : instructions) {
        if (!inserted && (i[0] & 65535) == static_cast<u32>(spv::Op::OpFunction)) {
            emit(constants);
            inserted = true;
        }
        if (&i == &*site)
            emit(replacement);
        else
            out.insert(out.end(), i.begin(), i.end());
    }
    return out;
}
struct Mip {
    std::array<u32, 4> dimensions; // width, height, depth/layers, image mip count
    std::array<u32, 4> offsets;    // block offset, texel offset, block width, block height
    std::array<u32, 4> flags;      // 0=2D,1=array,2=cube,3=sliced volume; sRGB
};
struct Query {
    std::array<u32, 4>
        config; // base mip, Load/point/linear/mip-linear, repeat/clamp/border, samples
    std::array<float, 4> coordinates;
};
static_assert(sizeof(Mip) == 48 && sizeof(Query) == 32 && sizeof(Color) == 16);

Color Mix(Color a, Color b, float f) {
    for (size_t c = 0; c < a.size(); ++c)
        a[c] += (b[c] - a[c]) * f;
    return a;
}
float LinearSRGB(float v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
void Bits(Block& b, u32 first, u32 count, u32 value) {
    for (u32 i = 0; i < count; ++i) {
        const u32 bit = first + i;
        b[bit / 32] = (b[bit / 32] & ~(1u << (bit % 32))) | (((value >> i) & 1u) << (bit % 32));
    }
}
Block Constant(u32 rgba) {
    // LDR void extent with reserved bits set and all-ones extent: Khronos section 23.23.
    Block b{0xFFFFFDFCu, 0xFFFFFFFFu, 0, 0};
    for (u32 c = 0; c < 4; ++c)
        Bits(b, 64 + c * 16, 16, ((rgba >> (c * 8)) & 255) * 257);
    return b;
}
Block Complex(u32 seed, u32 mode, u32 partitions, u32 cem) {
    std::mt19937 random{seed};
    Block b{random(), random(), random(), random()};
    Bits(b, 0, 11, mode);
    Bits(b, 11, 2, partitions - 1);
    if (partitions == 1) {
        Bits(b, 13, 4, cem);
    } else {
        Bits(b, 13, 10, seed % 1024);
        Bits(b, 23, 6, cem << 2); // shared endpoint mode, no extra CEM bits
        if (seed % 3 == 0) {
            // Mixed classes 0/1 and LDR modes 0/1/4/5. Include all extra-CEM tail lengths.
            const u32 weights = mode == 0x51    ? 26
                                : mode == 0x52  ? 38
                                : mode == 0x53  ? 48
                                : mode == 0x251 ? 58
                                : mode == 0x451 ? 52
                                                : 75;
            const u32 extra = 3 * partitions - 4;
            const u32 bits = 128 - weights - 29 - extra - ((mode & 0x400) ? 2 : 0);
            if (bits >= partitions * 2 * 3) {
                u32 packed = 1, values = partitions * 2;
                for (u32 p = 0; p < partitions; ++p) {
                    if ((seed >> p) & 1 && bits >= (values + 2) * 3) {
                        packed |= 1u << (2 + p);
                        values += 2;
                    }
                    packed |= ((seed >> (p + 5)) & 1u) << (2 + partitions + 2 * p);
                }
                Bits(b, 23, 6, packed & 63);
                Bits(b, 128 - weights - extra, extra, packed >> 6);
            }
        }
    }
    return b;
}

struct Corpus {
    std::vector<Block> blocks;
    std::vector<u32> rgba;
    std::vector<Mip> mips;
    std::vector<Query> queries;
    std::vector<u32> images;
    u32 bulk{}, cube_base{}, volume_base{}, srgb_base{}, constant_base{};
    u32 complex_blocks{}, dual_blocks{}, partition_blocks{}, mixed_cem_blocks{}, invalid_blocks{};

    u32 AddImage(u32 width, u32 height, u32 depth, u32 bw, u32 bh, u32 kind, bool srgb, u32 levels,
                 bool constant_only = false) {
        const u32 base = static_cast<u32>(mips.size());
        images.push_back(base);
        for (u32 l = 0; l < levels; ++l) {
            const u32 cols = (width + bw - 1) / bw;
            const u32 rows = (height + bh - 1) / bh;
            Mip m{{width, height, depth, levels},
                  {static_cast<u32>(blocks.size()), static_cast<u32>(rgba.size()), bw, bh},
                  {kind, srgb ? 1u : 0u, 0, 0}};
            const size_t count = size_t(cols) * rows * depth;
            for (size_t i = 0; i < count; ++i) {
                const u32 seed = static_cast<u32>(i + base * 991 + l * 733);
                if (constant_only || i % 9 == 0) {
                    blocks.push_back(Constant(0x01010101u * (seed & 255) ^ 0xAF738149u));
                } else {
                    constexpr std::array modes{0x51u, 0x52u, 0x53u, 0x251u, 0x451u, 0x452u};
                    const u32 mode = modes[i % modes.size()];
                    const bool dual = (mode & 0x400) != 0;
                    const u32 partitions = 1 + seed % (dual ? 3u : 4u);
                    // Valid LDR CEMs with RGB and gradual alpha. Each weight grid is 4x4.
                    constexpr std::array cems{0u, 1u, 4u, 5u, 6u, 8u, 9u, 10u, 12u, 13u};
                    u32 cem = cems[seed % cems.size()];
                    const u32 weight_bits = mode == 0x51    ? 26
                                            : mode == 0x52  ? 38
                                            : mode == 0x53  ? 48
                                            : mode == 0x251 ? 58
                                            : mode == 0x451 ? 52
                                                            : 75;
                    const u32 color_bits =
                        128 - weight_bits - (partitions == 1 ? 17 : 29) - (dual ? 2 : 0);
                    // Keep generated endpoint quantization at >=8 values (three bits each).
                    while (((cem / 4 + 1) * 2 * partitions * 3) > color_bits)
                        cem /= 4;
                    blocks.push_back(Complex(seed, mode, partitions, cem));
                    ++complex_blocks;
                    mixed_cem_blocks += partitions > 1 && ((blocks.back()[0] >> 23) & 3) != 0;
                    dual_blocks += dual;
                    partition_blocks += partitions > 1;
                }
            }
            rgba.resize(rgba.size() + size_t(width) * height * depth);
            Tegra::Texture::ASTC::DecompressForSamplingProbe(
                {reinterpret_cast<const u8*>(blocks.data() + m.offsets[0]), count * 16}, width,
                height, depth, bw, bh,
                {reinterpret_cast<u8*>(rgba.data() + m.offsets[1]),
                 size_t(width) * height * depth * 4},
                srgb);
            mips.push_back(m);
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
            if (kind == 3)
                depth = std::max(1u, depth / 2);
        }
        return base;
    }

    static std::array<float, 3> FaceDirection(u32 face, float u, float v) {
        const float x = 2 * u - 1, y = 2 * v - 1;
        switch (face) {
        case 0:
            return {1, -y, -x};
        case 1:
            return {-1, -y, x};
        case 2:
            return {x, 1, y};
        case 3:
            return {x, -1, -y};
        case 4:
            return {x, -y, 1};
        default:
            return {-x, -y, -1};
        }
    }
    static std::array<float, 3> CubeLocation(std::array<float, 3> d) {
        const float x = d[0], y = d[1], z = d[2];
        const float ax = std::abs(x), ay = std::abs(y), az = std::abs(z);
        float u, v, f;
        if (ax >= ay && ax >= az) {
            f = x >= 0 ? 0.f : 1.f;
            u = (x >= 0 ? -z : z) / ax;
            v = -y / ax;
        } else if (ay >= az) {
            f = y >= 0 ? 2.f : 3.f;
            u = x / ay;
            v = (y >= 0 ? z : -z) / ay;
        } else {
            f = z >= 0 ? 4.f : 5.f;
            u = (z >= 0 ? x : -x) / az;
            v = -y / az;
        }
        return {u * 0.5f + 0.5f, v * 0.5f + 0.5f, f};
    }
    Color Load(u32 level, int x, int y, int z, u32 address, bool alternate_corner = false) const {
        if (level >= mips.size())
            return {};
        const Mip& m = mips[level];
        const int w = m.dimensions[0], h = m.dimensions[1], d = m.dimensions[2];
        if (m.flags[0] == 2 && (x < 0 || x >= w) && (y < 0 || y >= h)) {
            const int cx = std::clamp(x, 0, w - 1), cy = std::clamp(y, 0, h - 1);
            const Color a = Load(level, cx, cy, z, address);
            const Color b = Load(level, x, cy, z, address);
            const Color c = Load(level, cx, y, z, address);
            Color result{};
            for (u32 channel = 0; channel < 4; ++channel)
                result[channel] = alternate_corner ? (b[channel] + c[channel]) / 2
                                                   : (a[channel] + b[channel] + c[channel]) / 3;
            return result;
        }
        if (m.flags[0] == 2 && (x < 0 || x >= w || y < 0 || y >= h)) {
            const auto p = CubeLocation(FaceDirection(z, (x + .5f) / w, (y + .5f) / h));
            x = std::clamp(static_cast<int>(std::floor(p[0] * w)), 0, w - 1);
            y = std::clamp(static_cast<int>(std::floor(p[1] * h)), 0, h - 1);
            z = static_cast<int>(p[2]);
        } else {
            const auto wrap = [address](int p, int s) {
                if (address == 0)
                    return (p % s + s) % s;
                if (address == 1)
                    return std::clamp(p, 0, s - 1);
                return p;
            };
            x = wrap(x, w);
            y = wrap(y, h);
            if (m.flags[0] == 3)
                z = wrap(z, d);
        }
        if (x < 0 || y < 0 || z < 0 || x >= w || y >= h || z >= d)
            return {};
        const size_t index = m.offsets[1] + (size_t(z) * h + y) * w + x;
        if (index >= rgba.size())
            return {};
        const u32 packed = rgba[index];
        Color color{};
        for (u32 c = 0; c < 4; ++c) {
            color[c] = float((packed >> (8 * c)) & 255) / 255;
            if (c < 3 && m.flags[1])
                color[c] = LinearSRGB(color[c]);
        }
        return color;
    }
    Color SampleLevel(u32 level, const Query& q, bool alternate_corner = false) const {
        const auto& m = mips[level];
        std::array<float, 3> p{q.coordinates[0], q.coordinates[1], q.coordinates[2]};
        const u32 mode = q.config[1], address = q.config[2];
        if (mode == 0)
            return Load(level, int(std::floor(p[0])), int(std::floor(p[1])), int(std::floor(p[2])),
                        address);
        if (m.flags[0] == 2)
            p = CubeLocation(p);
        p[0] *= m.dimensions[0];
        p[1] *= m.dimensions[1];
        const bool volume = m.flags[0] == 3;
        if (volume)
            p[2] *= m.dimensions[2];
        if (mode == 1)
            return Load(level, int(std::floor(p[0])), int(std::floor(p[1])), int(std::floor(p[2])),
                        address);
        p[0] -= .5f;
        p[1] -= .5f;
        if (volume)
            p[2] -= .5f;
        std::array<int, 3> b{};
        std::array<float, 3> f{};
        for (u32 c = 0; c < 3; ++c) {
            b[c] = int(std::floor(p[c]));
            f[c] = p[c] - b[c];
        }
        std::array<Color, 2> planes{};
        for (int z = 0; z < (volume ? 2 : 1); ++z) {
            const Color a = Load(level, b[0], b[1], b[2] + z, address, alternate_corner);
            const Color c = Load(level, b[0], b[1] + 1, b[2] + z, address, alternate_corner);
            const Color d = Load(level, b[0] + 1, b[1] + 1, b[2] + z, address, alternate_corner);
            const Color e = Load(level, b[0] + 1, b[1], b[2] + z, address, alternate_corner);
            planes[z] = Mix(Mix(a, e, f[0]), Mix(c, d, f[0]), f[1]);
        }
        return volume ? Mix(planes[0], planes[1], f[2]) : planes[0];
    }
    Color Sample(const Query& q, bool alternate_corner = false) const {
        if (q.config[0] >= mips.size())
            return {};
        const float lod =
            std::clamp(q.coordinates[3], 0.f, float(mips[q.config[0]].dimensions[3] - 1));
        const u32 l0 = static_cast<u32>(std::floor(lod));
        Color a = SampleLevel(q.config[0] + l0, q, alternate_corner);
        if (q.config[1] != 3)
            return a;
        Query next = q;
        next.config[1] = 2;
        const u32 l1 = std::min(l0 + 1, mips[q.config[0]].dimensions[3] - 1);
        return Mix(a, SampleLevel(q.config[0] + l1, next, alternate_corner), lod - l0);
    }
};

Corpus MakeCorpus() {
    Corpus c;
    constexpr std::array<std::array<u32, 2>, 14> footprints{{{4, 4},
                                                             {5, 4},
                                                             {5, 5},
                                                             {6, 5},
                                                             {6, 6},
                                                             {8, 5},
                                                             {8, 6},
                                                             {8, 8},
                                                             {10, 5},
                                                             {10, 6},
                                                             {10, 8},
                                                             {10, 10},
                                                             {12, 10},
                                                             {12, 12}}};
    for (auto f : footprints)
        c.AddImage(f[0] * 5 + 3, f[1] * 4 + 1, 1, f[0], f[1], 0, false, 6);
    c.AddImage(37, 29, 5, 8, 8, 1, false, 6);
    c.cube_base = c.AddImage(32, 32, 6, 8, 8, 2, false, 6);
    c.volume_base = c.AddImage(37, 29, 9, 8, 8, 3, false, 6); // depth 9,4,2,1,1,1
    c.srgb_base =
        c.AddImage(25, 17, 3, 8, 8, 1, true, 5); // independent endpoint/transfer/filter oracle
    c.bulk = c.AddImage(512, 512, 8, 8, 8, 1, false, 7);
    // Reserved modes, malformed void extents, HDR in LDR, dual-plane/partition conflicts,
    // invalid weights and insufficient endpoint bits: explicit Khronos error-color oracles.
    const u32 bad = static_cast<u32>(c.mips.size());
    std::vector<Block> invalid{{},
                               {0x1C4, 0, 0, 0},
                               {0x10D, 0, 0, 0},
                               Complex(42, 0x451, 4, 0),
                               Complex(42, 0x453, 3, 12),
                               Complex(42, 0x51, 1, 2)};
    auto void_reserved = Constant(0x12345678);
    Bits(void_reserved, 11, 1, 0);
    invalid.push_back(void_reserved);
    auto void_hdr = Constant(0x12345678);
    Bits(void_hdr, 9, 1, 1);
    invalid.push_back(void_hdr);
    auto void_extent = Constant(0x12345678);
    Bits(void_extent, 12, 13, 1);
    Bits(void_extent, 25, 13, 0);
    invalid.push_back(void_extent);
    const u32 invalid_width = static_cast<u32>(invalid.size()) * 4;
    c.mips.push_back({{invalid_width, 4, 1, 1},
                      {static_cast<u32>(c.blocks.size()), static_cast<u32>(c.rgba.size()), 4, 4},
                      {0, 0, 0, 0}});
    c.blocks.insert(c.blocks.end(), invalid.begin(), invalid.end());
    const size_t rgba_first = c.rgba.size();
    c.rgba.resize(rgba_first + invalid_width * 4);
    Tegra::Texture::ASTC::DecompressForSamplingProbe(
        {reinterpret_cast<const u8*>(invalid.data()), invalid.size() * 16}, invalid_width, 4, 1, 4,
        4, {reinterpret_cast<u8*>(c.rgba.data() + rgba_first), size_t(invalid_width) * 16}, false);
    if (!std::all_of(c.rgba.begin() + rgba_first, c.rgba.end(),
                     [](u32 p) { return p == 0xFFFF00FF; }))
        throw std::runtime_error("CPU invalid-block oracle disagrees with Khronos vectors");
    c.invalid_blocks = static_cast<u32>(invalid.size());
    c.images.push_back(bad);

    // Non-replicated 16-bit void colors verify top-byte decode_unorm8 rather than rounding.
    const u32 constant_image = static_cast<u32>(c.mips.size());
    c.constant_base = constant_image;
    Block constant = Constant(0);
    Bits(constant, 64, 16, 0x12FF);
    Bits(constant, 80, 16, 0xAB01);
    Bits(constant, 96, 16, 0x7FFF);
    Bits(constant, 112, 16, 0x0080);
    Bits(constant, 12, 13, 1);
    Bits(constant, 25, 13, 2);
    Bits(constant, 38, 13, 3);
    Bits(constant, 51, 13, 4);
    c.mips.push_back({{8, 8, 1, 1},
                      {static_cast<u32>(c.blocks.size()), static_cast<u32>(c.rgba.size()), 8, 8},
                      {0, 0, 0, 0}});
    c.blocks.push_back(constant);
    c.rgba.resize(c.rgba.size() + 64, 0x007FAB12);
    c.images.push_back(constant_image);
    // Bounded SRVs plus explicit offset checks must return black for an absent payload block.
    const u32 missing_image = static_cast<u32>(c.mips.size());
    c.mips.push_back({{8, 8, 1, 1},
                      {static_cast<u32>(c.blocks.size()), static_cast<u32>(c.rgba.size()), 8, 8},
                      {0, 0, 0, 0}});

    // Exhaustive texels for all small mips/layers; this detects block-edge/partial-mip errors.
    for (u32 level = 0; level < c.mips.size(); ++level) {
        if (level >= c.bulk && level < c.bulk + 7)
            continue;
        const auto& m = c.mips[level];
        for (u32 z = 0; z < m.dimensions[2]; ++z)
            for (u32 y = 0; y < m.dimensions[1]; ++y)
                for (u32 x = 0; x < m.dimensions[0]; ++x)
                    c.queries.push_back({{level, 0, 1, 1}, {float(x), float(y), float(z), 0}});
    }
    std::mt19937 random{0xA57C2026};
    for (u32 base : c.images) {
        const auto& m = c.mips[base];
        for (u32 i = 0; i < 384; ++i) {
            const auto unit = [&] { return float(random() % 2001) / 1000.f - .5f; };
            Query q{{base, 1 + (i / 3) % 3, i % 3, 1},
                    {unit(), unit(), float(i % m.dimensions[2]),
                     float(i % (m.dimensions[3] * 4)) / 4.f - .25f}};
            if (m.flags[0] == 3)
                q.coordinates[2] = unit();
            if (m.flags[0] == 2) {
                const auto d = Corpus::FaceDirection(i % 6, (i % 8 == 0) ? 1.f : unit(), unit());
                std::copy(d.begin(), d.end(), q.coordinates.begin());
            }
            c.queries.push_back(q);
        }
    }
    // All face edges and corners, including 1x1 mips. Point ties have implementation-defined face
    // selection; filtered corner taps follow D3D's reference three-face rule.
    for (u32 face = 0; face < 6; ++face)
        for (u32 level = 0; level < 6; ++level) {
            constexpr std::array<std::array<float, 2>, 8> edges{
                {{0, 0}, {0, 1}, {1, 0}, {1, 1}, {0, .5f}, {1, .5f}, {.5f, 0}, {.5f, 1}}};
            for (auto uv : edges)
                for (u32 mode : {2u, 3u}) {
                    const auto dir = Corpus::FaceDirection(face, uv[0], uv[1]);
                    c.queries.push_back({{c.cube_base, mode, 1, 1},
                                         {dir[0], dir[1], dir[2], float(level) + .375f}});
                }
        }
    // Invalid layer and metadata IDs must return black, with no out-of-range read.
    c.queries.push_back({{c.bulk, 0, 1, 1}, {1, 1, -1, 0}});
    c.queries.push_back({{c.bulk, 0, 1, 1}, {1, 1, 8, 0}});
    c.queries.push_back({{missing_image, 0, 1, 1}, {1, 1, 0, 0}});
    c.queries.push_back({{static_cast<u32>(c.mips.size()), 0, 1, 1}, {0, 0, 0, 0}});
    return c;
}

struct Buffer {
    ComPtr<ID3D12Resource> resource;
    u64 payload{}, allocated{};
};
Buffer MakeBuffer(Device& device, u64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                  bool uav = false) {
    const D3D12_HEAP_PROPERTIES heap{.Type = type};
    const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                                   .Width = std::max(4ull, size),
                                   .Height = 1,
                                   .DepthOrArraySize = 1,
                                   .MipLevels = 1,
                                   .SampleDesc = {1, 0},
                                   .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                   .Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                : D3D12_RESOURCE_FLAG_NONE};
    Buffer buffer{{}, size, device.Get()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes};
    ThrowIfFailed(device.Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                        nullptr, IID_PPV_ARGS(&buffer.resource)),
                  "ASTC probe buffer");
    return buffer;
}
void Write(Buffer& buffer, const void* data, size_t bytes) {
    void* mapped{};
    const D3D12_RANGE no_read{};
    ThrowIfFailed(buffer.resource->Map(0, &no_read, &mapped), "ASTC probe upload Map");
    std::memcpy(mapped, data, bytes);
    const D3D12_RANGE written{0, bytes};
    buffer.resource->Unmap(0, &written);
}
void Transition(ID3D12GraphicsCommandList* cmd, Buffer& b, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
    const D3D12_RESOURCE_BARRIER barrier{
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {b.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after}};
    cmd->ResourceBarrier(1, &barrier);
}

class Probe {
public:
    Device device;
    ShaderCompiler compiler;
    Corpus corpus;
    std::array<Buffer, 5> inputs; // blocks, queries, metadata, output, expanded
    std::vector<Buffer> uploads;
    Buffer readback, times;
    std::array<Buffer, 4> native_textures;
    Buffer target;
    std::array<Buffer, 2> guest_inputs;
    std::vector<u32> guest_comp, guest_frag;
    u32 DescriptorCount() const {
        return AstcGuestSamplingEnabled() ? 13u : 10u;
    }
    ComPtr<ID3D12DescriptorHeap> rtv;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12DescriptorHeap> guest_samplers;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, 3> pipelines;
    std::array<ComPtr<ID3D12PipelineState>, 3> graphics;
    ComPtr<ID3D12QueryHeap> timestamps;
    u64 frequency{};
    // Destroy/drain the scheduler before any resource, even when validation throws.
    Scheduler scheduler{device};

    Probe() : corpus{MakeCorpus()} {
        if (!compiler.IsAvailable())
            throw std::runtime_error("SPIR-V to DXIL unavailable");
        CreatePipeline();
        Upload(0, corpus.blocks.data(), corpus.blocks.size() * sizeof(Block));
        Upload(1, corpus.queries.data(), corpus.queries.size() * sizeof(Query));
        Upload(2, corpus.mips.data(), corpus.mips.size() * sizeof(Mip));
        Upload(4, corpus.rgba.data(), corpus.rgba.size() * sizeof(u32));
        UploadNativeTexture(0, corpus.bulk);
        UploadNativeTexture(1, corpus.cube_base);
        UploadNativeTexture(2, corpus.volume_base);
        UploadNativeTexture(3, corpus.srgb_base);
        inputs[3] =
            MakeBuffer(device, corpus.queries.size() * sizeof(Color), D3D12_HEAP_TYPE_DEFAULT,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        readback = MakeBuffer(device, inputs[3].payload, D3D12_HEAP_TYPE_READBACK,
                              D3D12_RESOURCE_STATE_COPY_DEST);
        times = MakeBuffer(device, 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        const D3D12_QUERY_HEAP_DESC td{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 2};
        ThrowIfFailed(device.Get()->CreateQueryHeap(&td, IID_PPV_ARGS(&timestamps)),
                      "ASTC probe timestamps");
        ThrowIfFailed(device.Queue()->GetTimestampFrequency(&frequency),
                      "ASTC probe timestamp frequency");
        if (frequency == 0)
            throw std::runtime_error("ASTC probe timestamp frequency is zero");
        if (AstcGuestSamplingEnabled())
            UploadGuestBacking(corpus.bulk);
        CreateDescriptors();
        CreateTarget();
        scheduler.Finish();
        u64 upload_alloc = 0;
        for (const auto& b : uploads)
            upload_alloc += b.allocated;
        const u64 resident = inputs[0].allocated + inputs[2].allocated;
        u64 total = readback.allocated + times.allocated + upload_alloc + NativeAllocation() +
                    target.allocated;
        for (const auto& b : inputs)
            total += b.allocated;
        LOG_INFO(Render,
                 "ASTC probe memory: blocks={} metadata={} resident_alloc={} resident_x={:.6f} "
                 "rgba_reference_alloc={} queries_alloc={} results_alloc={} readback_alloc={} "
                 "upload_peak_alloc={} timestamp_readback_alloc={} harness_peak_buffer_alloc={} "
                 "payload_x=1.0 (excludes PSO/driver/CPU/descriptor/query-heap memory)",
                 inputs[0].payload, inputs[2].payload, resident,
                 double(resident) / inputs[0].payload, inputs[4].allocated, inputs[1].allocated,
                 inputs[3].allocated, readback.allocated, upload_alloc, times.allocated, total);
        LOG_INFO(Render, "ASTC probe reference: rgba_native_texture_alloc={} bytes (harness only)",
                 NativeAllocation());
        LOG_INFO(Render, "ASTC probe fragment target: allocation={} bytes (harness only)",
                 target.allocated);
        uploads.clear();
        const auto memory = device.QueryCacheMemoryPressure();
        LOG_INFO(Render,
                 "ASTC probe observed memory: app_used={} app_limit={} dxgi_local_used={} "
                 "dxgi_budget={} "
                 "(all probe resources, CPU references, compiler/driver; not a texture multiplier)",
                 memory.app_used, memory.app_limit, memory.gpu_used, memory.gpu_budget);
        LOG_INFO(Render,
                 "ASTC probe corpus: images={} mips={} blocks={} complex={} dual={} "
                 "multipart={} mixed_cem={} invalid={} queries={} footprints=14 "
                 "sliced_3d_depth=9/4/2/1/1/1",
                 corpus.images.size(), corpus.mips.size(), corpus.blocks.size(),
                 corpus.complex_blocks, corpus.dual_blocks, corpus.partition_blocks,
                 corpus.mixed_cem_blocks, corpus.invalid_blocks, corpus.queries.size());
    }
    void Upload(u32 index, const void* data, size_t size) {
        inputs[index] =
            MakeBuffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        uploads.push_back(
            MakeBuffer(device, size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ));
        Write(uploads.back(), data, size);
        auto* cmd = scheduler.CommandList();
        cmd->CopyBufferRegion(inputs[index].resource.Get(), 0, uploads.back().resource.Get(), 0,
                              size);
        Transition(cmd, inputs[index], D3D12_RESOURCE_STATE_COPY_DEST, ReadState);
    }
    void ReplaceQueries(std::span<const Query> queries) {
        if (queries.size_bytes() > inputs[1].payload)
            throw std::runtime_error("ASTC probe query capacity");
        Buffer upload = MakeBuffer(device, queries.size_bytes(), D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_STATE_GENERIC_READ);
        Write(upload, queries.data(), queries.size_bytes());
        auto* cmd = scheduler.CommandList();
        Transition(cmd, inputs[1], ReadState, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(inputs[1].resource.Get(), 0, upload.resource.Get(), 0,
                              upload.payload);
        Transition(cmd, inputs[1], D3D12_RESOURCE_STATE_COPY_DEST, ReadState);
        scheduler.Finish();
    }
    void UploadGuestBacking(u32 base_index) {
        const auto& base = corpus.mips[base_index];
        const u32 levels = base.dimensions[3], layers = base.dimensions[2];
        const u32 header = 16 + levels * 48;
        std::vector<u32> words(header / 4);
        words[0] = levels;
        words[1] = header / 16;
        u32 layer_stride = 0;
        std::vector<u32> offsets(levels), blocks_x(levels), blocks_y(levels), gobs(levels),
            heights(levels);
        for (u32 l = 0; l < levels; ++l) {
            const auto& m = corpus.mips[base_index + l];
            blocks_x[l] = (m.dimensions[0] + m.offsets[2] - 1) / m.offsets[2];
            blocks_y[l] = (m.dimensions[1] + m.offsets[3] - 1) / m.offsets[3];
            gobs[l] = (blocks_x[l] * 16 + 63) / 64;
            const u32 rows = (blocks_y[l] + 7) / 8;
            heights[l] = std::min(3u, static_cast<u32>(std::bit_width(rows) - 1));
            offsets[l] = layer_stride;
            layer_stride += gobs[l] * 512 * Common::AlignUp<u32>(rows, 1u << heights[l]);
        }
        words.resize((header + layer_stride * layers) / 4);
        words[2] = layer_stride * layers / 16;
        for (u32 l = 0; l < levels; ++l) {
            const auto& m = corpus.mips[base_index + l];
            const u32 h = 4 + l * 12;
            words[h] = m.dimensions[0];
            words[h + 1] = m.dimensions[1];
            words[h + 2] = layers;
            const u32 shift = heights[l], mask = (1u << shift) - 1;
            words[h + 4] = offsets[l];
            words[h + 5] = layer_stride;
            words[h + 6] = gobs[l] * (512u << shift);
            words[h + 7] = 9 + shift;
            words[h + 8] = shift;
            words[h + 9] = mask;
            for (u32 z = 0; z < layers; ++z)
                for (u32 y = 0; y < blocks_y[l]; ++y)
                    for (u32 x = 0; x < blocks_x[l]; ++x) {
                        const u32 bx = x * 16;
                        const u32 swizzled = ((bx & 32) << 3) | ((y & 6) << 5) | ((bx & 16) << 1) |
                                             ((y & 1) << 4) | (bx & 15);
                        const u32 by = y / 8;
                        const u32 byte = header + z * layer_stride + offsets[l] +
                                         ((by >> shift) * gobs[l] + bx / 64) * (512u << shift) +
                                         (by & mask) * 512 + swizzled;
                        const auto& b =
                            corpus.blocks[m.offsets[0] + (z * blocks_y[l] + y) * blocks_x[l] + x];
                        std::memcpy(reinterpret_cast<u8*>(words.data()) + byte, b.data(), 16);
                    }
        }
        std::array<std::array<std::array<u32, 4>, 6>, 7> metadata{};
        for (u32 slot = 5; slot < 7; ++slot) {
            metadata[slot][0] = {1, 0, levels, 0};
            metadata[slot][1] = {layers, base.offsets[2], base.offsets[3],
                                 D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
            metadata[slot][2] = {3, 3, slot == 6, slot == 6};
            metadata[slot][3] = {slot == 6, 0, 0, std::bit_cast<u32>(D3D12_FLOAT32_MAX)};
        }
        for (u32 i = 0; i < 2; ++i) {
            const void* data = i ? static_cast<const void*>(metadata.data()) : words.data();
            const size_t size = i ? sizeof(metadata) : words.size() * 4;
            guest_inputs[i] =
                MakeBuffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
            uploads.push_back(MakeBuffer(device, size, D3D12_HEAP_TYPE_UPLOAD,
                                         D3D12_RESOURCE_STATE_GENERIC_READ));
            Write(uploads.back(), data, size);
            scheduler.CommandList()->CopyBufferRegion(guest_inputs[i].resource.Get(), 0,
                                                      uploads.back().resource.Get(), 0, size);
            Transition(scheduler.CommandList(), guest_inputs[i], D3D12_RESOURCE_STATE_COPY_DEST,
                       ReadState);
        }
        // Complete copies before a later corpus upload replaces these resources.
        scheduler.Finish();
        LOG_INFO(Render, "ASTC guest probe backing: original_alloc={} metadata_alloc={} bytes",
                 guest_inputs[0].allocated, guest_inputs[1].allocated);
    }
    void CreateDescriptors() {
        const D3D12_DESCRIPTOR_HEAP_DESC d{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                           .NumDescriptors = DescriptorCount(),
                                           .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        ThrowIfFailed(device.Get()->CreateDescriptorHeap(&d, IID_PPV_ARGS(&descriptors)),
                      "ASTC probe descriptors");
        const u32 stride = device.Get()->GetDescriptorHandleIncrementSize(d.Type);
        auto cpu = descriptors->GetCPUDescriptorHandleForHeapStart();
        for (u32 i = 0; i < 5; ++i, cpu.ptr += stride) {
            if (i == 3) {
                const D3D12_UNORDERED_ACCESS_VIEW_DESC view{
                    .Format = DXGI_FORMAT_R32_TYPELESS,
                    .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
                    .Buffer = {.NumElements = static_cast<UINT>(inputs[i].payload / 4),
                               .Flags = D3D12_BUFFER_UAV_FLAG_RAW}};
                device.Get()->CreateUnorderedAccessView(inputs[i].resource.Get(), nullptr, &view,
                                                        cpu);
            } else {
                const D3D12_SHADER_RESOURCE_VIEW_DESC view{
                    .Format = DXGI_FORMAT_R32_TYPELESS,
                    .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                    .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                    .Buffer = {.NumElements = static_cast<UINT>(inputs[i].payload / 4),
                               .Flags = D3D12_BUFFER_SRV_FLAG_RAW}};
                device.Get()->CreateShaderResourceView(inputs[i].resource.Get(), &view, cpu);
            }
        }
        const auto& bulk_mip = corpus.mips[corpus.bulk];
        const D3D12_SHADER_RESOURCE_VIEW_DESC native{
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2DArray = {.MipLevels = bulk_mip.dimensions[3],
                               .ArraySize = bulk_mip.dimensions[2]}};
        device.Get()->CreateShaderResourceView(
            AstcGuestSamplingEnabled() ? nullptr : native_textures[0].resource.Get(), &native, cpu);
        cpu.ptr += stride;
        device.Get()->CreateShaderResourceView(
            AstcGuestSamplingEnabled() ? nullptr : native_textures[0].resource.Get(), &native, cpu);
        for (u32 slot = 1; slot < 4; ++slot) {
            cpu.ptr += stride;
            const auto& m = corpus.mips[slot == 1   ? corpus.cube_base
                                        : slot == 2 ? corpus.volume_base
                                                    : corpus.srgb_base];
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = slot == 3 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (slot == 1) {
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                view.TextureCube.MipLevels = m.dimensions[3];
            } else if (slot == 2) {
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                view.Texture3D.MipLevels = m.dimensions[3];
            } else {
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray.MipLevels = m.dimensions[3];
                view.Texture2DArray.ArraySize = m.dimensions[2];
            }
            device.Get()->CreateShaderResourceView(native_textures[slot].resource.Get(), &view,
                                                   cpu);
        }
        if (AstcGuestSamplingEnabled())
            CreateGuestDescriptors();
    }
    void CreateGuestDescriptors() {
        const u32 stride =
            device.Get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = descriptors->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += 10 * stride;
        const D3D12_SHADER_RESOURCE_VIEW_DESC view{
            .Format = DXGI_FORMAT_R32G32B32A32_UINT,
            .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Buffer = {.NumElements = static_cast<u32>(guest_inputs[0].payload / 16)},
        };
        for (u32 i = 0; i < 2; ++i, cpu.ptr += stride)
            device.Get()->CreateShaderResourceView(guest_inputs[0].resource.Get(), &view, cpu);
        const D3D12_SHADER_RESOURCE_VIEW_DESC raw{
            .Format = DXGI_FORMAT_R32_TYPELESS,
            .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Buffer = {.NumElements = static_cast<u32>(guest_inputs[1].payload / 4),
                       .Flags = D3D12_BUFFER_SRV_FLAG_RAW},
        };
        device.Get()->CreateShaderResourceView(guest_inputs[1].resource.Get(), &raw, cpu);
    }
    u64 NativeAllocation() const {
        u64 total = 0;
        for (const auto& buffer : native_textures)
            total += buffer.allocated;
        return total;
    }
    void UploadNativeTexture(u32 slot, u32 image) {
        const auto& base = corpus.mips[image];
        const bool volume = base.flags[0] == 3;
        Buffer& native_texture = native_textures[slot];
        const D3D12_RESOURCE_DESC desc{.Dimension = volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                           : D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                       .Width = base.dimensions[0],
                                       .Height = base.dimensions[1],
                                       .DepthOrArraySize = static_cast<UINT16>(base.dimensions[2]),
                                       .MipLevels = static_cast<UINT16>(base.dimensions[3]),
                                       .Format = base.flags[1] ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                               : DXGI_FORMAT_R8G8B8A8_UNORM,
                                       .SampleDesc = {1, 0},
                                       .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN};
        const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
        native_texture.allocated = device.Get()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        ThrowIfFailed(device.Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&native_texture.resource)),
                      "ASTC native reference texture");
        const u32 layers = volume ? 1 : base.dimensions[2];
        const u32 subresources = layers * base.dimensions[3];
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
        u64 size{};
        device.Get()->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(), nullptr,
                                            nullptr, &size);
        uploads.push_back(
            MakeBuffer(device, size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ));
        void* data{};
        const D3D12_RANGE no_read{};
        ThrowIfFailed(uploads.back().resource->Map(0, &no_read, &data),
                      "ASTC native reference upload Map");
        auto* cmd = scheduler.CommandList();
        for (u32 layer = 0; layer < layers; ++layer) {
            for (u32 l = 0; l < base.dimensions[3]; ++l) {
                const u32 sub = l + layer * base.dimensions[3];
                const auto& fp = footprints[sub];
                const auto& m = corpus.mips[image + l];
                for (u32 z = 0; z < fp.Footprint.Depth; ++z) {
                    for (u32 y = 0; y < m.dimensions[1]; ++y) {
                        const auto* source =
                            corpus.rgba.data() + m.offsets[1] +
                            (size_t(volume ? z : layer) * m.dimensions[1] + y) * m.dimensions[0];
                        std::memcpy(static_cast<u8*>(data) + fp.Offset +
                                        (size_t(z) * fp.Footprint.Height + y) *
                                            fp.Footprint.RowPitch,
                                    source, size_t(m.dimensions[0]) * sizeof(u32));
                    }
                }
                const D3D12_TEXTURE_COPY_LOCATION src{.pResource = uploads.back().resource.Get(),
                                                      .Type =
                                                          D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                      .PlacedFootprint = fp};
                const D3D12_TEXTURE_COPY_LOCATION dst{.pResource = native_texture.resource.Get(),
                                                      .Type =
                                                          D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                                      .SubresourceIndex = sub};
                cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
        }
        const D3D12_RANGE written{0, static_cast<SIZE_T>(size)};
        uploads.back().resource->Unmap(0, &written);
        Transition(cmd, native_texture, D3D12_RESOURCE_STATE_COPY_DEST, ReadState);
    }
    void CreateTarget() {
        const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                       .Width = 256,
                                       .Height = 16,
                                       .DepthOrArraySize = 1,
                                       .MipLevels = 1,
                                       .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
                                       .SampleDesc = {1, 0},
                                       .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
        target.allocated = device.Get()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        ThrowIfFailed(device.Get()->CreateCommittedResource(
                          &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                          nullptr, IID_PPV_ARGS(&target.resource)),
                      "ASTC probe target");
        const D3D12_DESCRIPTOR_HEAP_DESC hd{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                            .NumDescriptors = 1};
        ThrowIfFailed(device.Get()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv)),
                      "ASTC probe RTV heap");
        device.Get()->CreateRenderTargetView(target.resource.Get(), nullptr,
                                             rtv->GetCPUDescriptorHandleForHeapStart());
    }
    void CreatePipeline(u32 expanded_calls = 0, std::optional<u32> selected_mask = std::nullopt,
                        bool guest_only = false) {
        const u32 expected_mask = selected_mask.value_or(expanded_calls == 13 ? 2u : 3u);
        u32 guest_decoder_calls{};
        if (AstcGuestSamplingEnabled()) {
            Shader::Info info{};
            for (u32 i = 0; i < 2; ++i)
                info.texture_descriptors.push_back(
                    {.type = Shader::TextureType::ColorArray2D, .count = 1});
            std::unordered_set<u32> allowed;
            for (u32 i = 0; i < 2; ++i)
                if (expected_mask & (1u << i))
                    allowed.insert(i);
            for (bool fragment : {false, true}) {
                const auto source = fragment ? std::span<const u32>{D3D12_ASTC_NATIVE_FRAG_SPV}
                                             : std::span<const u32>{D3D12_ASTC_NATIVE_COMP_SPV};
                auto patched =
                    PatchAstcGuestShader(ExpandProbeFetch(source, expanded_calls), info, 5,
                                         fragment, selected_mask ? &allowed : nullptr);
                if (patched.bindings.size() != size_t(std::popcount(expected_mask)) ||
                    patched.bindings.contains(5) != bool(expected_mask & 1) ||
                    patched.bindings.contains(6) != bool(expected_mask & 2))
                    throw std::runtime_error("ASTC guest import binding coverage");
                if (selected_mask == 0u &&
                    patched.code != std::vector<u32>(source.begin(), source.end()))
                    throw std::runtime_error("ASTC zero-mask changed native shader");
                guest_decoder_calls = patched.decoder_calls;
                (fragment ? guest_frag : guest_comp) = std::move(patched.code);
            }
            if (descriptors) {
                scheduler.Finish();
                const auto& bulk = corpus.mips[corpus.bulk];
                const D3D12_SHADER_RESOURCE_VIEW_DESC view{
                    .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                    .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
                    .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                    .Texture2DArray = {.MipLevels = bulk.dimensions[3],
                                       .ArraySize = bulk.dimensions[2]}};
                auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
                const u32 stride = device.Get()->GetDescriptorHandleIncrementSize(
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                handle.ptr += 5 * stride;
                for (u32 i = 0; i < 2; ++i, handle.ptr += stride)
                    device.Get()->CreateShaderResourceView(
                        expected_mask & (1u << i) ? nullptr : native_textures[0].resource.Get(),
                        &view, handle);
            }
        }
        std::vector<D3D12_DESCRIPTOR_RANGE> ranges(DescriptorCount());
        std::vector<D3D12_ROOT_PARAMETER> parameters(DescriptorCount() + 2 +
                                                     (AstcGuestSamplingEnabled() ? 1 : 0));
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0, PUSH_CONSTANT_SPACE, 6};
        for (u32 i = 0; i < DescriptorCount(); ++i) {
            ranges[i] = {.RangeType = i == 3 ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                                             : D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                         .NumDescriptors = 1,
                         .BaseShaderRegister = i < 10   ? i
                                               : i < 12 ? 4096 + 5 + (i - 10)
                                                        : 0,
                         .RegisterSpace = i == 12 ? 29u : 0u};
            parameters[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[1 + i].DescriptorTable = {1, &ranges[i]};
        }
        parameters[DescriptorCount() + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[DescriptorCount() + 1].Constants = {0, RUNTIME_DATA_SPACE,
                                                       sizeof(dxil_spirv_compute_runtime_data) / 4};
        std::array<D3D12_STATIC_SAMPLER_DESC, 8> samplers{};
        for (u32 i = 0; i < 2; ++i) {
            samplers[i] = {.Filter =
                               i ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT,
                           .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                           .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                           .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                           .MaxAnisotropy = 1,
                           .ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS,
                           .MinLOD = 0,
                           .MaxLOD = D3D12_FLOAT32_MAX,
                           .ShaderRegister = 5 + i,
                           .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL};
        }
        for (u32 i = 0; i < 6; ++i) {
            constexpr std::array modes{D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                       D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                       D3D12_TEXTURE_ADDRESS_MODE_BORDER};
            samplers[2 + i] = {.Filter = i >= 3 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
                                                : D3D12_FILTER_MIN_MAG_MIP_POINT,
                               .AddressU = modes[i % 3],
                               .AddressV = modes[i % 3],
                               .AddressW = modes[i % 3],
                               .MaxAnisotropy = 1,
                               .ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS,
                               .BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
                               .MinLOD = 0,
                               .MaxLOD = D3D12_FLOAT32_MAX,
                               .ShaderRegister = 40 + i,
                               .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL};
        }
        std::array<D3D12_DESCRIPTOR_RANGE, 2> sampler_ranges{{
            {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
             .NumDescriptors = 2,
             .BaseShaderRegister = 5,
             .OffsetInDescriptorsFromTableStart = 0},
            {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
             .NumDescriptors = 6,
             .BaseShaderRegister = 40,
             .OffsetInDescriptorsFromTableStart = 2},
        }};
        if (AstcGuestSamplingEnabled()) {
            parameters.back().ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters.back().DescriptorTable = {2, sampler_ranges.data()};
            const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{
                .Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                .NumDescriptors = 8,
                .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
            ThrowIfFailed(
                device.Get()->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&guest_samplers)),
                "ASTC guest probe sampler heap");
            auto cpu = guest_samplers->GetCPUDescriptorHandleForHeapStart();
            const u32 stride = device.Get()->GetDescriptorHandleIncrementSize(heap_desc.Type);
            for (const auto& s : samplers) {
                const D3D12_SAMPLER_DESC sampler{.Filter = s.Filter,
                                                 .AddressU = s.AddressU,
                                                 .AddressV = s.AddressV,
                                                 .AddressW = s.AddressW,
                                                 .MipLODBias = s.MipLODBias,
                                                 .MaxAnisotropy = s.MaxAnisotropy,
                                                 .ComparisonFunc = s.ComparisonFunc,
                                                 .MinLOD = s.MinLOD,
                                                 .MaxLOD = s.MaxLOD};
                device.Get()->CreateSampler(&sampler, cpu);
                cpu.ptr += stride;
            }
        }
        const D3D12_ROOT_SIGNATURE_DESC desc{
            .NumParameters = static_cast<UINT>(parameters.size()),
            .pParameters = parameters.data(),
            .NumStaticSamplers = AstcGuestSamplingEnabled() ? 0u : 8u,
            .pStaticSamplers = AstcGuestSamplingEnabled() ? nullptr : samplers.data(),
            .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ComPtr<ID3DBlob> blob, errors;
        ThrowIfFailed(
            D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors),
            "ASTC probe serialize root signature");
        ThrowIfFailed(device.Get()->CreateRootSignature(0, blob->GetBufferPointer(),
                                                        blob->GetBufferSize(), IID_PPV_ARGS(&root)),
                      "ASTC probe root signature");
        const std::array<std::span<const u32>, 3> sources{
            D3D12_ASTC_SAMPLE_COMP_SPV, D3D12_ASTC_REFERENCE_COMP_SPV,
            guest_comp.empty() ? std::span<const u32>{D3D12_ASTC_NATIVE_COMP_SPV}
                               : std::span<const u32>{guest_comp}};
        for (u32 i = 0; i < 3; ++i) {
            if (guest_only && i != 2)
                continue;
            const auto start = Clock::now();
            const std::array<ShaderCompiler::PipelineStage, 1> stages{
                {{sources[i], DXIL_SPIRV_SHADER_COMPUTE}}};
            const ShaderCompiler::PipelineOptions options{
                .astc_budget_device = i == 2 && guest_decoder_calls ? &device : nullptr,
                .astc_decoder_calls = guest_decoder_calls};
            const auto compiled = compiler.CompilePipeline(stages, options);
            const auto translated_at = Clock::now();
            const D3D12_COMPUTE_PIPELINE_STATE_DESC pso{
                .pRootSignature = root.Get(),
                .CS = {compiled[0].dxil.data(), compiled[0].dxil.size()}};
            ThrowIfFailed(
                device.Get()->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipelines[i])),
                "ASTC probe PSO");
            LOG_INFO(
                Render,
                "ASTC probe compile split: stage=compute route={} translate_sign_ms={:.3f} "
                "pso_ms={:.3f}",
                std::array{"direct", "rgba_raw", "rgba_native"}[i],
                std::chrono::duration<double, std::milli>(translated_at - start).count(),
                std::chrono::duration<double, std::milli>(Clock::now() - translated_at).count());
            LOG_INFO(Render,
                     "ASTC probe compile: route={} sm=6.4 spirv_bytes={} dxil_bytes={} "
                     "translate_sign_pso_ms={:.3f}",
                     i == 0   ? "direct"
                     : i == 1 ? "rgba_raw"
                              : "rgba_native",
                     sources[i].size_bytes(), compiled[0].dxil.size(),
                     std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        }
        const std::array<std::span<const u32>, 3> fragments{
            D3D12_ASTC_SAMPLE_FRAG_SPV, D3D12_ASTC_REFERENCE_FRAG_SPV,
            guest_frag.empty() ? std::span<const u32>{D3D12_ASTC_NATIVE_FRAG_SPV}
                               : std::span<const u32>{guest_frag}};
        for (u32 i = 0; i < 3; ++i) {
            if (guest_only && i != 2)
                continue;
            const auto start = Clock::now();
            const std::array<ShaderCompiler::PipelineStage, 2> stages{
                {{D3D12_ASTC_PROBE_VERT_SPV, DXIL_SPIRV_SHADER_VERTEX},
                 {fragments[i], DXIL_SPIRV_SHADER_FRAGMENT}}};
            const ShaderCompiler::PipelineOptions options{
                .astc_budget_device = i == 2 && guest_decoder_calls ? &device : nullptr,
                .astc_decoder_calls = guest_decoder_calls};
            const auto compiled = compiler.CompilePipeline(stages, options);
            const auto translated_at = Clock::now();
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = root.Get();
            pso.VS = {compiled[0].dxil.data(), compiled[0].dxil.size()};
            pso.PS = {compiled[1].dxil.data(), compiled[1].dxil.size()};
            pso.BlendState.RenderTarget[0] = {FALSE,
                                              FALSE,
                                              D3D12_BLEND_ONE,
                                              D3D12_BLEND_ZERO,
                                              D3D12_BLEND_OP_ADD,
                                              D3D12_BLEND_ONE,
                                              D3D12_BLEND_ZERO,
                                              D3D12_BLEND_OP_ADD,
                                              D3D12_LOGIC_OP_NOOP,
                                              D3D12_COLOR_WRITE_ENABLE_ALL};
            pso.SampleMask = UINT_MAX;
            pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso.RasterizerState.DepthClipEnable = TRUE;
            pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            pso.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                               D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
            pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
            pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso.NumRenderTargets = 1;
            pso.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
            pso.SampleDesc.Count = 1;
            ThrowIfFailed(
                device.Get()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&graphics[i])),
                "ASTC probe graphics PSO");
            LOG_INFO(
                Render,
                "ASTC probe compile split: stage=fragment route={} translate_sign_ms={:.3f} "
                "pso_ms={:.3f}",
                std::array{"direct", "rgba_raw", "rgba_native"}[i],
                std::chrono::duration<double, std::milli>(translated_at - start).count(),
                std::chrono::duration<double, std::milli>(Clock::now() - translated_at).count());
            LOG_INFO(Render,
                     "ASTC probe compile: route={} stage=fragment sm=6.4 spirv_bytes={} "
                     "dxil_bytes={} translate_sign_pso_ms={:.3f}",
                     i == 0   ? "direct"
                     : i == 1 ? "rgba_raw"
                              : "rgba_native",
                     fragments[i].size_bytes(), compiled[1].dxil.size(),
                     std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        }
    }
    std::pair<std::vector<Color>, double> Execute(u32 route, u32 first, u32 count, u32 repeat,
                                                  bool fragment = false) {
        if (count == 0 || repeat == 0 || size_t(first) + count > corpus.queries.size() ||
            (fragment && count > 4096))
            throw std::runtime_error("ASTC probe dispatch range");
        auto* cmd = scheduler.CommandList();
        ID3D12DescriptorHeap* heaps[]{descriptors.Get(), guest_samplers.Get()};
        cmd->SetDescriptorHeaps(guest_samplers ? 2 : 1, heaps);
        if (fragment)
            cmd->SetGraphicsRootSignature(root.Get());
        else
            cmd->SetComputeRootSignature(root.Get());
        if (guest_samplers) {
            if (fragment)
                cmd->SetGraphicsRootDescriptorTable(
                    DescriptorCount() + 2, guest_samplers->GetGPUDescriptorHandleForHeapStart());
            else
                cmd->SetComputeRootDescriptorTable(
                    DescriptorCount() + 2, guest_samplers->GetGPUDescriptorHandleForHeapStart());
        }
        scheduler.SetPipelineState(fragment ? graphics[route].Get() : pipelines[route].Get());
        const std::array<u32, 6> constants{count,
                                           static_cast<u32>(corpus.mips.size()),
                                           static_cast<u32>(corpus.blocks.size()),
                                           static_cast<u32>(corpus.rgba.size()),
                                           repeat,
                                           first};
        if (fragment)
            cmd->SetGraphicsRoot32BitConstants(0, 6, constants.data(), 0);
        else
            cmd->SetComputeRoot32BitConstants(0, 6, constants.data(), 0);
        auto gpu = descriptors->GetGPUDescriptorHandleForHeapStart();
        const u32 stride =
            device.Get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (u32 i = 0; i < DescriptorCount(); ++i, gpu.ptr += stride) {
            if (fragment)
                cmd->SetGraphicsRootDescriptorTable(i + 1, gpu);
            else
                cmd->SetComputeRootDescriptorTable(i + 1, gpu);
        }
        const u32 groups = (count + 63) / 64;
        dxil_spirv_compute_runtime_data runtime{};
        runtime.group_count_x = groups;
        runtime.group_count_y = runtime.group_count_z = 1;
        if (fragment)
            cmd->SetGraphicsRoot32BitConstants(DescriptorCount() + 1, sizeof(runtime) / 4, &runtime,
                                               0);
        else
            cmd->SetComputeRoot32BitConstants(DescriptorCount() + 1, sizeof(runtime) / 4, &runtime,
                                              0);
        cmd->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        if (fragment) {
            const D3D12_VIEWPORT viewport{0, 0, 256, float((count + 255) / 256), 0, 1};
            const D3D12_RECT scissor{0, 0, 256, static_cast<LONG>((count + 255) / 256)};
            cmd->RSSetViewports(1, &viewport);
            cmd->RSSetScissorRects(1, &scissor);
            const auto handle = rtv->GetCPUDescriptorHandleForHeapStart();
            cmd->OMSetRenderTargets(1, &handle, FALSE, nullptr);
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmd->DrawInstanced(3, 1, 0, 0);
        } else
            cmd->Dispatch(groups, 1, 1);
        cmd->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        if (fragment) {
            Transition(cmd, target, D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            const D3D12_TEXTURE_COPY_LOCATION src{.pResource = target.resource.Get(),
                                                  .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                                  .SubresourceIndex = 0};
            const D3D12_TEXTURE_COPY_LOCATION dst{
                .pResource = readback.resource.Get(),
                .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                .PlacedFootprint = {0, {DXGI_FORMAT_R32G32B32A32_FLOAT, 256, 16, 1, 4096}}};
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Transition(cmd, target, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
        } else {
            Transition(cmd, inputs[3], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(readback.resource.Get(), 0, inputs[3].resource.Get(), 0,
                                  size_t(count) * sizeof(Color));
            Transition(cmd, inputs[3], D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        cmd->ResolveQueryData(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                              times.resource.Get(), 0);
        scheduler.Finish();
        std::vector<Color> result(count);
        void* data{};
        D3D12_RANGE read{0, size_t(count) * sizeof(Color)};
        ThrowIfFailed(readback.resource->Map(0, &read, &data), "ASTC probe readback Map");
        std::memcpy(result.data(), data, read.End);
        const D3D12_RANGE no_write{};
        readback.resource->Unmap(0, &no_write);
        read = {0, 16};
        ThrowIfFailed(times.resource->Map(0, &read, &data), "ASTC probe times Map");
        const auto* ticks = static_cast<const u64*>(data);
        const double us = double(ticks[1] - ticks[0]) * 1e6 / double(frequency);
        times.resource->Unmap(0, &no_write);
        CheckDebugErrors();
        return {std::move(result), us};
    }
    void CheckDebugErrors() {
        ComPtr<ID3D12InfoQueue> info;
        bool error = false;
        if (SUCCEEDED(device.Get()->QueryInterface(IID_PPV_ARGS(&info)))) {
            for (u64 i = 0; i < info->GetNumStoredMessages(); ++i) {
                SIZE_T size{};
                ThrowIfFailed(info->GetMessage(i, nullptr, &size), "ASTC probe debug message size");
                std::vector<u8> bytes(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                ThrowIfFailed(info->GetMessage(i, message, &size), "ASTC probe debug message");
                error |= message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
            }
        }
        device.LogDebugMessages();
        if (error)
            throw std::runtime_error("ASTC probe D3D12 validation error");
    }
};

void Validate(Probe& probe) {
    // Dispatch in bands to avoid TDRs on complex decoding and to bound console command duration.
    constexpr u32 band = 4096;
    u64 mismatches = 0;
    double max_direct = 0, max_reference = 0;
    for (u32 first = 0; first < probe.corpus.queries.size(); first += band) {
        const u32 count = std::min(band, static_cast<u32>(probe.corpus.queries.size()) - first);
        const auto direct = probe.Execute(0, first, count, 1).first;
        const auto expanded = probe.Execute(1, first, count, 1).first;
        const auto pixel = probe.Execute(0, first, count, 1, true).first;
        for (u32 i = 0; i < count; ++i) {
            const auto expected = probe.corpus.Sample(probe.corpus.queries[first + i]);
            bool bad = false;
            for (u32 c = 0; c < 4; ++c) {
                const double a = std::abs(double(direct[i][c]) - expected[c]);
                const double b = std::abs(double(expanded[i][c]) - expected[c]);
                max_direct = std::max(max_direct, a);
                max_reference = std::max(max_reference, b);
                bad |= !std::isfinite(direct[i][c]) || !std::isfinite(expanded[i][c]) ||
                       !std::isfinite(pixel[i][c]) ||
                       std::abs(pixel[i][c] - expected[c]) > 0.00002 || a > 0.00002 || b > 0.00002;
            }
            if (bad && mismatches++ < 12) {
                const auto& q = probe.corpus.queries[first + i];
                LOG_ERROR(Render,
                          "ASTC probe mismatch: query={} meta={} mode={} address={} coords={} "
                          "direct={} expanded={} pixel={} cpu={}",
                          first + i, q.config[0], q.config[1], q.config[2], q.coordinates,
                          direct[i], expanded[i], pixel[i], expected);
            }
        }
    }
    LOG_INFO(Render,
             "ASTC probe validation: queries={} mismatches={} max_direct_error={:.9f} "
             "max_rgba_raw_error={:.9f} profile=LDR_UNORM8 invalid_policy=opaque_magenta",
             probe.corpus.queries.size(), mismatches, max_direct, max_reference);
    if (mismatches)
        throw std::runtime_error("ASTC sampling mismatches");
}

void ValidateNative(Probe& probe) {
    std::vector<Query> queries;
    u32 ambiguous_point_faces = 0;
    for (Query q : probe.corpus.queries) {
        for (u32 base :
             {probe.corpus.cube_base, probe.corpus.volume_base, probe.corpus.srgb_base}) {
            const u32 levels = probe.corpus.mips[base].dimensions[3];
            if (q.config[0] < base || q.config[0] >= base + levels)
                continue;
            q.coordinates[3] += float(q.config[0] - base);
            q.config[0] = base;
            if (base == probe.corpus.cube_base && q.config[1] == 1) {
                std::array<float, 3> axes{std::abs(q.coordinates[0]), std::abs(q.coordinates[1]),
                                          std::abs(q.coordinates[2])};
                std::sort(axes.begin(), axes.end());
                if (axes[2] - axes[1] < 1e-6f) {
                    ++ambiguous_point_faces;
                    break;
                }
            }
            queries.push_back(q);
            break;
        }
    }
    u64 mismatches = 0, allowed_corners = 0;
    double max_hardware_error = 0;
    for (size_t first = 0; first < queries.size(); first += 4096) {
        const u32 count = static_cast<u32>(std::min(size_t{4096}, queries.size() - first));
        probe.ReplaceQueries({queries.data() + first, count});
        const auto native = probe.Execute(2, 0, count, 1).first;
        const auto direct = probe.Execute(0, 0, count, 1).first;
        for (u32 i = 0; i < count; ++i) {
            const Query& q = queries[first + i];
            const Color expected = probe.corpus.Sample(q);
            bool bad = false, native_bad = false;
            for (u32 c = 0; c < 4; ++c) {
                const double error = std::abs(double(native[i][c]) - expected[c]);
                max_hardware_error = std::max(max_hardware_error, error);
                bad |=
                    !std::isfinite(direct[i][c]) || std::abs(direct[i][c] - expected[c]) > .00002;
                // Spatial hardware filters have finite subtexel precision; CPU/direct is checked
                // tightly, while native is bounded by two 8-bit texel units in linear space.
                native_bad |= !std::isfinite(native[i][c]) || error > 2.0 / 255.0 + .00002;
            }
            if (native_bad && q.config[0] == probe.corpus.cube_base && q.config[1] >= 2) {
                const Color alternative = probe.corpus.Sample(q, true);
                bool matches = true;
                for (u32 c = 0; c < 4; ++c)
                    matches &= std::isfinite(native[i][c]) &&
                               std::abs(native[i][c] - alternative[c]) <= 2.0 / 255.0 + .00002;
                if (matches) {
                    native_bad = false;
                    ++allowed_corners;
                }
            }
            if ((bad || native_bad) && mismatches++ < 8) {
                LOG_ERROR(Render,
                          "ASTC probe native mismatch: meta={} mode={} coords={} direct={} "
                          "native={} cpu={}",
                          q.config[0], q.config[1], q.coordinates, direct[i], native[i], expected);
            }
        }
    }
    LOG_INFO(Render,
             "ASTC probe native validation: queries={} mismatches={} max_error={:.9f} "
             "allowed_corner_alternatives={} "
             "ambiguous_point_faces_skipped={} cube_edges_corners=yes volume_mip_depth=yes "
             "srgb_before_filter=yes",
             queries.size(), mismatches, max_hardware_error, allowed_corners,
             ambiguous_point_faces);
    if (mismatches)
        throw std::runtime_error("ASTC native sampler validation mismatch");
}

void ValidateGuest(Probe& probe) {
    if (!AstcGuestSamplingEnabled())
        return;
    std::mt19937 random{0xA57C2027};
    std::vector<Query> queries;
    for (u32 level = 0; level < 7; ++level) {
        const auto& mip = probe.corpus.mips[probe.corpus.bulk + level];
        for (u32 mode = 0; mode < 4; ++mode) {
            for (u32 layer = 0; layer < 8; ++layer) {
                for (u32 sample = 0; sample < 32; ++sample) {
                    const float x = float(random() % mip.dimensions[0]);
                    const float y = float(random() % mip.dimensions[1]);
                    Query q{{probe.corpus.bulk, mode, 1, 1},
                            {mode == 0 ? x : (x + .375f) / mip.dimensions[0],
                             mode == 0 ? y : (y + .625f) / mip.dimensions[1], float(layer),
                             float(level) + (mode == 3 ? .375f : 0.f)}};
                    if (mode != 0 && sample < 4) {
                        q.coordinates[0] = sample & 1 ? -0.03125f : 1.03125f;
                        q.coordinates[1] = sample & 2 ? -0.03125f : 1.03125f;
                    }
                    queries.push_back(q);
                }
            }
        }
    }
    u64 mismatches{};
    double max_error{};
    for (size_t first = 0; first < queries.size(); first += 4096) {
        const u32 count = static_cast<u32>(std::min(size_t{4096}, queries.size() - first));
        probe.ReplaceQueries({queries.data() + first, count});
        for (bool fragment : {false, true}) {
            const auto colors = probe.Execute(2, 0, count, 1, fragment).first;
            for (u32 i = 0; i < count; ++i) {
                const Color expected = probe.corpus.Sample(queries[first + i]);
                bool bad{};
                for (u32 c = 0; c < 4; ++c) {
                    const double error = std::abs(double(colors[i][c]) - expected[c]);
                    max_error = std::max(max_error, error);
                    bad |= !std::isfinite(colors[i][c]) || error > .00002;
                }
                if (bad && mismatches++ < 8) {
                    const auto& q = queries[first + i];
                    LOG_ERROR(Render,
                              "ASTC guest mismatch: fragment={} mode={} coords={} gpu={} cpu={}",
                              fragment, q.config[1], q.coordinates, colors[i], expected);
                }
            }
        }
    }
    LOG_INFO(Render,
             "ASTC guest validation: queries={} stages=compute,fragment mismatches={} "
             "max_error={:.9f} mips=7 layers=8 original_blocklinear=yes native_bulk_srv=null",
             queries.size(), mismatches, max_error);
    if (mismatches)
        throw std::runtime_error("ASTC guest sampling mismatch");

    // Rebind the imported helper to every supported UNORM corpus image. This exercises prepared
    // blocks across footprints, partial mips, partitions, dual planes, constants and invalid modes.
    // sRGB/cube/volume branches of the native probe shader use other bindings and are not imported.
    u64 extended_queries{};
    u32 extended_images{};
    for (const u32 base : probe.corpus.images) {
        const auto& image = probe.corpus.mips[base];
        if (image.flags[0] > 1 || image.flags[1] || base == probe.corpus.bulk)
            continue;
        probe.UploadGuestBacking(base);
        probe.CreateGuestDescriptors();
        queries.clear();
        for (u32 level = 0; level < image.dimensions[3]; ++level) {
            const auto& mip = probe.corpus.mips[base + level];
            for (u32 mode = 0; mode < 4; ++mode) {
                for (u32 sample = 0; sample < 64; ++sample) {
                    const float x = float(random() % mip.dimensions[0]);
                    const float y = float(random() % mip.dimensions[1]);
                    queries.push_back({{base, mode, 1, 1},
                                       {mode == 0 ? x : (x + .375f) / mip.dimensions[0],
                                        mode == 0 ? y : (y + .625f) / mip.dimensions[1],
                                        float(random() % mip.dimensions[2]),
                                        float(level) + (mode == 3 ? .375f : 0.f)}});
                }
            }
        }
        for (size_t first = 0; first < queries.size(); first += 4096) {
            const u32 count = static_cast<u32>(std::min(size_t{4096}, queries.size() - first));
            probe.ReplaceQueries({queries.data() + first, count});
            for (bool fragment : {false, true}) {
                const auto colors = probe.Execute(2, 0, count, 1, fragment).first;
                for (u32 i = 0; i < count; ++i) {
                    const Color expected = probe.corpus.Sample(queries[first + i]);
                    for (u32 c = 0; c < 4; ++c) {
                        const double error = std::abs(double(colors[i][c]) - expected[c]);
                        max_error = std::max(max_error, error);
                        if (!std::isfinite(colors[i][c]) || error > .00002)
                            throw std::runtime_error(fmt::format(
                                "ASTC guest extended mismatch image={} mode={} component={} gpu={} "
                                "cpu={}",
                                base, queries[first + i].config[1], c, colors[i][c], expected[c]));
                    }
                }
            }
        }
        extended_queries += queries.size();
        ++extended_images;
    }
    LOG_INFO(Render,
             "ASTC guest extended validation: images={} queries_per_stage={} "
             "stages=compute,fragment mismatches=0 max_error={:.9f} profile=UNORM",
             extended_images, extended_queries, max_error);
    probe.UploadGuestBacking(probe.corpus.bulk);
    probe.CreateGuestDescriptors();
}

void ValidateParallelNativeCompiler(Probe& probe) {
    if (!AstcGuestSamplingEnabled())
        return;
    const u64 headroom = probe.device.QueryCacheMemoryPressure().AppFree();
    if (headroom < 4ULL * 1024 * 1024 * 1024) {
        LOG_INFO(Render, "ASTC native compiler parallel gate: skipped, app_free={} bytes",
                 headroom);
        return;
    }
    const std::array<ShaderCompiler::PipelineStage, 1> stages{
        {{D3D12_ASTC_REFERENCE_COMP_SPV, DXIL_SPIRV_SHADER_COMPUTE}}};
    std::vector<u8> reference;
    const auto serial_start = Clock::now();
    for (u32 i = 0; i < 4; ++i) {
        auto compiled = probe.compiler.CompilePipeline(stages, {});
        if (i && compiled[0].dxil != reference)
            throw std::runtime_error("ASTC native serial compiler output changed");
        reference = std::move(compiled[0].dxil);
    }
    const double serial_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - serial_start).count();
    const ShaderCompiler::PipelineOptions options{.parallel_native_astc_experiment = true};
    std::barrier start{4};
    std::array<std::future<std::vector<u8>>, 4> work;
    const auto parallel_start = Clock::now();
    size_t launched = 0;
    try {
        for (auto& task : work) {
            task = std::async(std::launch::async, [&] {
                start.arrive_and_wait();
                return probe.compiler.CompilePipeline(stages, options)[0].dxil;
            });
            ++launched;
        }
    } catch (...) {
        // Release waiting workers before their future destructors join them.
        for (size_t i = launched; i < work.size(); ++i)
            start.arrive_and_drop();
        throw;
    }
    for (auto& task : work) {
        if (task.get() != reference)
            throw std::runtime_error("ASTC native parallel compiler output mismatch");
    }
    const double parallel_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - parallel_start).count();
    LOG_INFO(Render,
             "ASTC native compiler parallel gate: workers=4 dxil_identical=yes "
             "serial_ms={:.3f} parallel_ms={:.3f} speedup={:.3f} app_free={} bytes; "
             "small reference shader, not guest compilation/FPS certification",
             serial_ms, parallel_ms, serial_ms / parallel_ms, headroom);
}

void BenchmarkConstantGuest(Probe& probe) {
    if (!AstcGuestSamplingEnabled())
        return;
    const u32 base = probe.corpus.constant_base;
    probe.UploadGuestBacking(base);
    probe.CreateGuestDescriptors();
    const Color expected = probe.corpus.Load(base, 0, 0, 0, 1);
    for (bool fragment : {false, true}) {
        for (u32 mode : {1u, 2u}) {
            std::vector<Query> queries(4096);
            for (u32 i = 0; i < queries.size(); ++i)
                queries[i] = {{base, mode, 1, 1},
                              {(float(i % 8) + .375f) / 8, (float(i / 8 % 8) + .625f) / 8, 0, 0}};
            probe.ReplaceQueries(queries);
            std::array<double, 5> timings;
            for (u32 pass = 0; pass < 6; ++pass) {
                auto [colors, us] =
                    probe.Execute(2, 0, static_cast<u32>(queries.size()), 1, fragment);
                if (pass)
                    timings[pass - 1] = us;
                for (const auto& color : colors)
                    for (u32 c = 0; c < 4; ++c)
                        if (!std::isfinite(color[c]) || std::abs(color[c] - expected[c]) > .00002)
                            throw std::runtime_error("ASTC guest constant benchmark mismatch");
            }
            std::sort(timings.begin(), timings.end());
            LOG_INFO(Render,
                     "ASTC guest constant bench: stage={} mode={} queries=4096 "
                     "median_us={:.3f} no_FPS_inference=yes",
                     fragment ? "fragment" : "compute", mode, timings[2]);
        }
    }
    probe.UploadGuestBacking(probe.corpus.bulk);
    probe.CreateGuestDescriptors();
}

void Benchmark(Probe& probe) {
    // Replace only the isolated query input after validation. ASTC and metadata remain resident.
    std::mt19937 random{0xBC12026};
    constexpr u32 count = 4096;
    for (bool fragment : {false, true}) {
        random.seed(0xBC12026);
        for (u32 pattern = 0; pattern < 2; ++pattern) {
            for (u32 mode = 0; mode < 4; ++mode) {
                for (u32 samples : {1u, 4u}) {
                    std::vector<Query> queries(count);
                    for (u32 i = 0; i < count; ++i) {
                        const u32 x = pattern ? random() % 512 : i % 64;
                        const u32 y = pattern ? random() % 512 : i / 64;
                        queries[i] = {{probe.corpus.bulk, mode, 1, samples},
                                      {mode == 0 ? float(x) : (x + .375f) / 512,
                                       mode == 0 ? float(y) : (y + .625f) / 512,
                                       float(pattern ? random() % 8 : 0),
                                       mode == 3 ? 1.375f : 0.f}};
                    }
                    // A dedicated small upload is retired after copying; query input size never
                    // grows.
                    Buffer upload =
                        MakeBuffer(probe.device, queries.size() * sizeof(Query),
                                   D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                    Write(upload, queries.data(), queries.size() * sizeof(Query));
                    auto* cmd = probe.scheduler.CommandList();
                    Transition(cmd, probe.inputs[1], ReadState, D3D12_RESOURCE_STATE_COPY_DEST);
                    cmd->CopyBufferRegion(probe.inputs[1].resource.Get(), 0, upload.resource.Get(),
                                          0, upload.payload);
                    Transition(cmd, probe.inputs[1], D3D12_RESOURCE_STATE_COPY_DEST, ReadState);
                    probe.scheduler.Finish();
                    std::array<std::array<double, 5>, 3> timings{};
                    std::array<std::vector<Color>, 3> results;
                    // Warm both PSOs, then alternate order so the clock/order does not always favor
                    // one.
                    for (u32 r = 0; r < 6; ++r) {
                        for (u32 j = 0; j < 3; ++j) {
                            const u32 route = (r + j) % 3;
                            auto [colors, us] = probe.Execute(route, 0, count, 1, fragment);
                            if (r)
                                timings[route][r - 1] = us;
                            results[route] = std::move(colors);
                        }
                    }
                    double checksum = 0;
                    for (u32 i = 0; i < count; ++i)
                        for (u32 c = 0; c < 4; ++c) {
                            if (!std::isfinite(results[0][i][c]) ||
                                std::abs(results[0][i][c] - results[1][i][c]) > .00002)
                                throw std::runtime_error("ASTC benchmark result mismatch");
                            // Native filtering uses the hardware's subtexel precision. Coordinates
                            // in this benchmark use exact 1/8 fractions, so UNORM8 interpolation
                            // may differ only by one stored texel unit due to hardware rounding.
                            if (!std::isfinite(results[2][i][c]) ||
                                std::abs(results[1][i][c] - results[2][i][c]) >
                                    1.0 / 255.0 + .00002)
                                throw std::runtime_error("ASTC native reference mismatch");
                            checksum += results[0][i][c];
                        }
                    for (auto& t : timings)
                        std::sort(t.begin(), t.end());
                    const double direct = timings[0][2], expanded = timings[1][2];
                    LOG_INFO(Render,
                             "ASTC probe bench: stage={} pattern={} mode={} samples={} queries={} "
                             "median_direct_us={:.3f} "
                             "median_rgba_raw_us={:.3f} median_rgba_native_us={:.3f} "
                             "ratio_native={:.3f} "
                             "ratio_raw={:.3f} direct_ns_per_query={:.3f} checksum={:.6f}",
                             fragment ? "fragment" : "compute", pattern ? "random" : "coherent",
                             mode, samples, count, direct, expanded, timings[2][2],
                             direct / timings[2][2], direct / expanded, direct * 1000 / count,
                             checksum);
                    if (AstcGuestSamplingEnabled()) {
                        LOG_INFO(Render,
                                 "ASTC guest bench scope: third route is imported decoder, not "
                                 "hardware-native RGBA; guest_us={:.3f} guest_to_rgba_raw={:.3f}",
                                 timings[2][2], timings[2][2] / expanded);
                    }
                }
            }
        }
    }
}
void ValidateManyCalls(Probe& probe) {
    if (!AstcGuestSamplingEnabled())
        return;
    for (const u32 calls : {5u, 13u}) {
        // The failed Series run had about 1.1 GiB available before its 650 MiB fallback. Keep a
        // similar amount available to the serial compiler, under the probe runner's real Job cap.
        constexpr u64 available_target = 1152ULL * 1024 * 1024;
        const auto before = probe.device.QueryCacheMemoryPressure();
        std::vector<u8> pressure;
        if (before.AppFree() > available_target)
            pressure.resize(before.AppFree() - available_target);
        const auto start_memory = probe.device.QueryCacheMemoryPressure();
        std::atomic<u64> peak{start_memory.app_used};
        std::jthread observer{[&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                const u64 used = probe.device.QueryCacheMemoryPressure().app_used;
                u64 old = peak.load();
                while (old < used && !peak.compare_exchange_weak(old, used)) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }};
        const auto begin = Clock::now();
        LOG_INFO(Render, "ASTC many-call gate START: calls={} app_free={} pressure_bytes={}", calls,
                 start_memory.AppFree(), pressure.size());
        probe.CreatePipeline(calls);
        // The thirteen-site fixture keeps the twelve-site binding native, while the independent
        // one-site binding still samples original ASTC. The five-site fixture imports both.
        probe.scheduler.Finish();
        const auto& bulk = probe.corpus.mips[probe.corpus.bulk];
        const D3D12_SHADER_RESOURCE_VIEW_DESC view{
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2DArray = {.MipLevels = bulk.dimensions[3], .ArraySize = bulk.dimensions[2]}};
        auto handle = probe.descriptors->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += 5 * probe.device.Get()->GetDescriptorHandleIncrementSize(
                              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        probe.device.Get()->CreateShaderResourceView(
            calls == 13 ? probe.native_textures[0].resource.Get() : nullptr, &view, handle);
        observer.request_stop();
        observer.join();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        LOG_INFO(Render,
                 "ASTC many-call compile: calls={} ms={:.3f} app_used_before={} "
                 "peak_app_used={} limit={} no_gameplay_inference=yes",
                 calls, ms, start_memory.app_used, peak.load(), start_memory.app_limit);
        std::vector<u8>{}.swap(pressure);
        std::vector<Query> queries(256);
        const auto& size = probe.corpus.mips[probe.corpus.bulk].dimensions;
        for (u32 i = 0; i < 128; ++i) {
            queries[i] = {{probe.corpus.bulk, 0, 1, 1},
                          {float((i * 7) % (size[0] - 16)), float((i * 11) % size[1]),
                           float(i % size[2]), 0}};
            queries[128 + i] = {
                {probe.corpus.bulk, 2, 1, 1},
                {float(i % 31) / 31.f, float((i * 7) % 29) / 29.f, float(i % size[2]), .5f}};
        }
        probe.ReplaceQueries(queries);
        for (const bool fragment : {false, true}) {
            const auto colors =
                probe.Execute(2, 0, static_cast<u32>(queries.size()), 1, fragment).first;
            for (u32 i = 0; i < queries.size(); ++i) {
                Color expected{};
                const u32 taps = i < 128 ? calls - 2 : 1;
                for (u32 t = 0; t < taps; ++t) {
                    Query q = queries[i];
                    if (i < 128)
                        q.coordinates[0] += t;
                    const auto color = probe.corpus.Sample(q);
                    for (u32 c = 0; c < 4; ++c)
                        expected[c] += color[c];
                }
                for (u32 c = 0; c < 4; ++c)
                    if (!std::isfinite(colors[i][c]) ||
                        std::abs(colors[i][c] - expected[c] / float(taps)) > .00002f)
                        throw std::runtime_error("ASTC many-call correctness mismatch");
            }
        }
        LOG_INFO(Render,
                 "ASTC many-call gate PASS: calls={} stages=compute,fragment queries=256 "
                 "mismatches=0 binding5={} binding6=direct_null_native_srv",
                 calls, calls == 13 ? "native" : "direct_null_native_srv");
    }
}

void ValidateSpecializedBindings(Probe& probe) {
    if (!AstcGuestSamplingEnabled())
        return;
    const auto& size = probe.corpus.mips[probe.corpus.bulk].dimensions;
    std::vector<Query> queries(256);
    for (u32 i = 0; i < 128; ++i) {
        queries[i] = {{probe.corpus.bulk, 0, 1, 1},
                      {float(i % size[0]), float((i * 7) % size[1]), float(i % size[2]), 0}};
        queries[i + 128] = {
            {probe.corpus.bulk, 2, 1, 1},
            {float(i % 31) / 31.f, float((i * 7) % 29) / 29.f, float(i % size[2]), .5f}};
    }
    for (u32 mask : {0u, 1u, 2u, 3u, 0u, 2u}) {
        probe.CreatePipeline(0, mask);
        probe.ReplaceQueries(queries);
        for (bool fragment : {false, true}) {
            const auto colors =
                probe.Execute(2, 0, static_cast<u32>(queries.size()), 1, fragment).first;
            for (u32 i = 0; i < queries.size(); ++i) {
                const auto expected = probe.corpus.Sample(queries[i]);
                const float tolerance = i >= 128 && !(mask & 2) ? 2.f / 255.f + .00002f : .00002f;
                for (u32 c = 0; c < 4; ++c)
                    if (!std::isfinite(colors[i][c]) ||
                        std::abs(colors[i][c] - expected[c]) > tolerance)
                        throw std::runtime_error("ASTC specialized binding mismatch");
            }
        }
        LOG_INFO(Render,
                 "ASTC specialization gate PASS: mask={} stages=compute,fragment queries=256 "
                 "mismatches=0 direct_native_srv=null zero_mask_preserves_source=yes",
                 mask);
    }
}

void ValidateCompilerPressure(Probe& probe) {
    if (!AstcGuestSamplingEnabled())
        throw std::runtime_error("ASTC compiler pressure gate needs GuestIntegration");
    const auto& size = probe.corpus.mips[probe.corpus.bulk].dimensions;
    for (u32 sites : {1u, 2u}) {
        std::vector<Query> queries(256);
        for (u32 i = 0; i < queries.size(); ++i) {
            const u32 mode = sites == 1 ? 2u : i < 128 ? 0u : 1u;
            const float x = float(i % size[0]), y = float((i * 7) % size[1]);
            queries[i] = {{probe.corpus.bulk, mode, 1, 1},
                          {mode == 0 ? x : (x + .35f) / size[0],
                           mode == 0 ? y : (y + .35f) / size[1], float(i % size[2]), 0}};
        }
        const u64 target_headroom = (sites == 1 ? 256ULL : 288ULL) * 1024 * 1024;
        const auto before = probe.device.QueryCacheMemoryPressure();
        std::vector<u8> pressure;
        if (before.AppFree() > target_headroom)
            pressure.resize(before.AppFree() - target_headroom);
        const auto pressured = probe.device.QueryCacheMemoryPressure();
        std::atomic<u64> peak{pressured.app_used};
        std::jthread observer{[&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                const auto used = probe.device.QueryCacheMemoryPressure().app_used;
                auto old = peak.load();
                while (old < used && !peak.compare_exchange_weak(old, used)) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }};
        probe.CreatePipeline(0, sites == 1 ? 2u : 1u, true);
        observer.request_stop();
        observer.join();
        std::vector<u8>{}.swap(pressure);
        probe.ReplaceQueries(queries);
        for (bool fragment : {false, true}) {
            const auto colors =
                probe.Execute(2, 0, static_cast<u32>(queries.size()), 1, fragment).first;
            for (u32 i = 0; i < queries.size(); ++i) {
                const auto expected = probe.corpus.Sample(queries[i]);
                for (u32 c = 0; c < 4; ++c)
                    if (!std::isfinite(colors[i][c]) ||
                        std::abs(colors[i][c] - expected[c]) > .00002f)
                        throw std::runtime_error("ASTC compiler pressure texel mismatch");
            }
        }
        LOG_INFO(Render,
                 "ASTC compiler pressure gate PASS: decoder_sites={} stages=compute,fragment "
                 "queries=256 "
                 "mismatches=0 app_free_before={} app_used_before={} peak_app_used={} limit={}",
                 sites, pressured.AppFree(), pressured.app_used, peak.load(), pressured.app_limit);
    }
}
} // namespace

bool RunAstcSamplingProbe(bool compiler_pressure_only) {
    try {
        LOG_INFO(Render, "ASTC probe START: isolated SM6.4 original blocks; guest import test={}",
                 AstcGuestSamplingEnabled());
        Probe probe;
        if (!compiler_pressure_only) {
            Validate(probe);
            ValidateNative(probe);
            ValidateGuest(probe);
            ValidateParallelNativeCompiler(probe);
            BenchmarkConstantGuest(probe);
            Benchmark(probe);
            ValidateManyCalls(probe);
            ValidateSpecializedBindings(probe);
        }
        ValidateCompilerPressure(probe);
        probe.scheduler.Finish();
        if (FAILED(probe.device.Get()->GetDeviceRemovedReason()))
            throw std::runtime_error("ASTC probe device removed");
        LOG_INFO(
            Render,
            "ASTC probe PASS: PC/console identity={}, payload_x=1; no gameplay/FPS certification",
            probe.device.AdapterName());
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "ASTC probe FAIL: {} (isolated gate, normal renderer unaffected)",
                  e.what());
        return false;
    }
}
} // namespace D3D12
