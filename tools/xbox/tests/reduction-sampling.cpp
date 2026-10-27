// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Filtered MIN/MAX sampler reduction emulated in the shader (EDEN_SPIRV_TO_DXIL_EMULATE_REDUCTION):
// a test bench (sanity, functional, regression, performance, stability) on the production pieces
// (spirv_to_dxil with the flag, ShaderCompiler, RootSignatureCache and the sampler table rows
// Sampler::IntegerState writes) and a real D3D12 device with the debug layer.
//
// The expected data never comes from the emulation:
//  - functional and stability compare against the device's own MINIMUM/MAXIMUM sampler (tiled
//    resources tier 2) sampling the plain shader, at coordinates and LODs on a grid where every
//    filter weight is exact at 8-bit subtexel precision, so both sides weigh the same texels
//  - regression checks closed forms computed on the CPU (a 2:1 MIN downsample is the minimum of
//    each 2x2 block; a point sample is one texel) and that a NONE row leaves the shader's results
//    bit-identical to the plain shader's
//
// Usage: reduction-sampling.exe --spv DIR [category] [--seed N] [--iterations N]

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <d3d12sdklayers.h>
#include <eden_spirv_to_dxil.h>
#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_d3d12/d3d12_border_color.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"

using namespace D3D12;

namespace {

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            throw Failure(fmt::format("line {}: {}: {}", __LINE__, #cond,                          \
                                      fmt::format(__VA_ARGS__)));                                  \
        }                                                                                          \
    } while (0)

u32 g_seed = 0x5eed;
u32 g_iterations = 200;
std::string g_spv_dir;
u32 g_assert_failures = 0;

void CountAssert(const char*) noexcept {
    ++g_assert_failures;
}

using Vec4 = std::array<float, 4>;

enum class Dim : u32 { D1 = 1, D2 = 2, Array2D = 3, D3 = 4 };
enum class Op : u32 { Lod = 0, Grad = 1, LodOffset = 2, GradOffset = 3 };

constexpr const char* DimName(Dim dim) {
    switch (dim) {
    case Dim::D1:
        return "1D";
    case Dim::D2:
        return "2D";
    case Dim::Array2D:
        return "2D array";
    case Dim::D3:
        return "3D";
    }
    return "?";
}
constexpr const char* OpName(Op op) {
    switch (op) {
    case Op::Lod:
        return "lod";
    case Op::Grad:
        return "grad";
    case Op::LodOffset:
        return "lod+offset";
    case Op::GradOffset:
        return "grad+offset";
    }
    return "?";
}
/// The constant offsets of sample.comp, per axis.
std::array<int, 3> OffsetOf(Op op) {
    if (op == Op::LodOffset) {
        return {1, -2, 1};
    }
    if (op == Op::GradOffset) {
        return {-3, 2, -1};
    }
    return {0, 0, 0};
}

std::vector<u32> LoadSpirv(const std::string& name) {
    const std::string path = g_spv_dir + "/" + name;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("missing SPIR-V " + path);
    }
    const auto size = static_cast<size_t>(file.tellg());
    std::vector<u32> words(size / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(size));
    return words;
}

// ---------------------------------------------------------------------------------------------
// Textures and samplers

struct TextureSpec {
    Dim dim = Dim::D2;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    u32 width = 16;
    u32 height = 16;
    u32 depth = 1; ///< layers of an array, depth of a 3D texture
    u32 levels = 5;
};

u32 Extent(u32 size, u32 level) {
    return std::max(size >> level, 1u);
}

struct Texture {
    TextureSpec spec;
    ComPtr<ID3D12Resource> resource;
    /// Per subresource (level + layer * levels), each texel as the shader reads it.
    std::vector<std::vector<Vec4>> texels;

    u32 Width(u32 level) const {
        return Extent(spec.width, level);
    }
    u32 Height(u32 level) const {
        return spec.dim == Dim::D1 ? 1 : Extent(spec.height, level);
    }
    u32 Depth(u32 level) const {
        return spec.dim == Dim::D3 ? Extent(spec.depth, level) : 1;
    }
    u32 Layers() const {
        return spec.dim == Dim::Array2D ? spec.depth : 1;
    }
    const Vec4& At(u32 level, u32 layer, u32 x, u32 y, u32 z) const {
        const auto& sub = texels[level + layer * spec.levels];
        return sub[(z * Height(level) + y) * Width(level) + x];
    }
};

struct SamplerConfig {
    bool min_linear = true;
    bool mag_linear = true;
    bool mip_linear = true;
    u32 reduction = EDEN_SAMPLER_REDUCTION_MIN;
    std::array<D3D12_TEXTURE_ADDRESS_MODE, 3> wrap{D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                                   D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                                   D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
    Vec4 border{0.0f, 0.0f, 0.0f, 0.0f};
    float bias = 0.0f;
    float min_lod = 0.0f;
    float max_lod = 1000.0f;

    std::string Describe() const {
        return fmt::format("{} min {} mag {} mip {} wrap {}/{}/{} bias {} lod {}..{}",
                           reduction == EDEN_SAMPLER_REDUCTION_MIN   ? "MIN"
                           : reduction == EDEN_SAMPLER_REDUCTION_MAX ? "MAX"
                                                                     : "NONE",
                           min_linear ? "linear" : "point", mag_linear ? "linear" : "point",
                           mip_linear ? "linear" : "point", static_cast<int>(wrap[0]),
                           static_cast<int>(wrap[1]), static_cast<int>(wrap[2]), bias, min_lod,
                           max_lod);
    }
};

/// The D3D12 sampler. hardware: with the MINIMUM/MAXIMUM reduction itself, otherwise STANDARD as
/// the renderer creates it when the device cannot.
D3D12_SAMPLER_DESC MakeSampler(const SamplerConfig& config, bool hardware) {
    D3D12_FILTER_REDUCTION_TYPE reduction = D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    if (hardware && config.reduction == EDEN_SAMPLER_REDUCTION_MIN) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_MINIMUM;
    } else if (hardware && config.reduction == EDEN_SAMPLER_REDUCTION_MAX) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_MAXIMUM;
    }
    const auto type = [](bool linear) {
        return linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    };
    D3D12_SAMPLER_DESC desc{};
    desc.Filter = D3D12_ENCODE_BASIC_FILTER(type(config.min_linear), type(config.mag_linear),
                                            type(config.mip_linear), reduction);
    desc.AddressU = config.wrap[0];
    desc.AddressV = config.wrap[1];
    desc.AddressW = config.wrap[2];
    desc.MipLODBias = config.bias;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    std::copy(config.border.begin(), config.border.end(), desc.BorderColor);
    desc.MinLOD = config.min_lod;
    desc.MaxLOD = config.max_lod;
    return desc;
}

/// The sampler table row, as IntegerSamplerTable::Add writes it for a view of the texture (default
/// component mapping).
eden_integer_sampler_state MakeRow(const SamplerConfig& config, const TextureSpec& texture) {
    eden_integer_sampler_state row{};
    std::array<u32, 4> border;
    for (size_t i = 0; i < 4; ++i) {
        border[i] = std::bit_cast<u32>(config.border[i]);
    }
    border = SampledBorderColor(border, texture.format, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
    std::copy(border.begin(), border.end(), row.border_color);
    row.lod_bias = config.bias;
    row.min_lod = config.min_lod;
    row.max_lod = config.max_lod;
    row.last_level = texture.levels - 1;
    for (size_t i = 0; i < 3; ++i) {
        row.wrap[i] = static_cast<u32>(config.wrap[i]);
    }
    row.reduction = config.reduction;
    row.filter = (config.min_linear ? EDEN_SAMPLER_FILTER_MIN_LINEAR : 0) |
                 (config.mag_linear ? EDEN_SAMPLER_FILTER_MAG_LINEAR : 0) |
                 (config.mip_linear ? EDEN_SAMPLER_FILTER_MIP_LINEAR : 0);
    return row;
}

// ---------------------------------------------------------------------------------------------
// Samples

struct SampleData {
    Vec4 coord; ///< coordinates, LOD in w
    Vec4 dx;
    Vec4 dy;
    Vec4 result;
};
static_assert(sizeof(SampleData) == 64);

/// Largest power of two dividing size (the grid denominator that keeps u * size exact).
u32 PowerOfTwoPart(u32 size) {
    return size & (~size + 1);
}

/// A coordinate whose texel position u * Extent(size, l) is a multiple of 1/256 at every level
/// l <= level (POT sizes; NPOT sizes keep level 0 only), in [lo, hi].
float GridCoord(std::mt19937& rng, u32 size, u32 level, float lo, float hi) {
    const double denom = 256.0 * PowerOfTwoPart(size);
    const double step = std::exp2(static_cast<double>(level));
    const auto qlo = static_cast<s64>(std::ceil(lo * denom / step));
    const auto qhi = static_cast<s64>(std::floor(hi * denom / step));
    std::uniform_int_distribution<s64> dist(qlo, qhi);
    return static_cast<float>(static_cast<double>(dist(rng)) * step / denom);
}

/// LODs that avoid ties: no .5 (nearest mip rounding) and 8-bit-exact fractions.
constexpr std::array<float, 14> LODS{-2.0f,  -1.25f, -0.25f, 0.25f, 0.75f, 1.0f,  1.25f,
                                     1.75f, 2.0f,   2.25f,  3.0f,  3.75f, 4.25f, 9.0f};

struct SampleOptions {
    float lo = -0.25f; ///< coordinate range
    float hi = 1.25f;
    std::span<const float> lods = LODS;
};

std::vector<SampleData> MakeSamples(const Texture& texture, Op op, const SampleOptions& options,
                                    std::mt19937& rng, u32 count) {
    const TextureSpec& spec = texture.spec;
    const bool npot = !std::has_single_bit(spec.width) ||
                      (spec.dim != Dim::D1 && !std::has_single_bit(spec.height)) ||
                      (spec.dim == Dim::D3 && !std::has_single_bit(spec.depth));
    std::vector<SampleData> samples(count);
    std::uniform_int_distribution<size_t> pick_lod(0, options.lods.size() - 1);
    for (SampleData& sample : samples) {
        const float lod = options.lods[pick_lod(rng)];
        // Every level the sample may read, whatever the mip filter and the sampler's clamps.
        const u32 level = npot ? 0 : spec.levels - 1;
        sample.coord[0] = GridCoord(rng, spec.width, level, options.lo, options.hi);
        if (spec.dim == Dim::D2 || spec.dim == Dim::Array2D || spec.dim == Dim::D3) {
            sample.coord[1] = GridCoord(rng, spec.height, level, options.lo, options.hi);
        }
        if (spec.dim == Dim::D3) {
            sample.coord[2] = GridCoord(rng, spec.depth, level, options.lo, options.hi);
        } else if (spec.dim == Dim::Array2D) {
            // Layers round to nearest: stay off the .5 ties, include out-of-range layers.
            constexpr std::array<float, 3> fractions{0.0f, 0.25f, 0.75f};
            std::uniform_int_distribution<int> layer(-1, static_cast<int>(spec.depth));
            sample.coord[2] = static_cast<float>(layer(rng)) + fractions[rng() % 3];
        }
        sample.coord[3] = lod;
        if (op == Op::Grad || op == Op::GradOffset) {
            // Isotropic: every axis of the (square) texture spans 2^lod texels per derivative.
            const float s = std::exp2(lod) / static_cast<float>(spec.width);
            sample.dx = {s, 0.0f, 0.0f, 0.0f};
            sample.dy = {0.0f, s, 0.0f, 0.0f};
            if (spec.dim == Dim::D1) {
                // One axis, dy left at zero: with both derivatives non-zero the oracle (AMD)
                // returns a coarser LOD than the spec's max(|dx|, |dy|) for 1D textures.
                sample.dy = {0.0f, 0.0f, 0.0f, 0.0f};
            }
        }
    }
    return samples;
}

// ---------------------------------------------------------------------------------------------
// The bench

struct Program {
    ComPtr<ID3D12PipelineState> pso;
    const PipelineLayout* layout = nullptr;
    bool graphics = false;
    size_t dxil_size = 0;
    double compile_ms = 0.0;
};

class Bench {
public:
    Bench();

    void MessagesCheck(const std::string& where);
    bool HardwareReduction() const {
        return hardware_reduction;
    }

    Texture MakeTexture(const TextureSpec& spec, std::mt19937& rng);
    Program Compute(Dim dim, Op op, bool emulate);
    Program ComputeInteger(bool emulate);
    Program Fragment(bool bias, bool emulate);

    /// Runs the samples through program with the sampler; row: the sampler table row (programs
    /// with a table only).
    std::vector<Vec4> Run(const Program& program, const Texture& texture,
                          const D3D12_SAMPLER_DESC& sampler, const eden_integer_sampler_state& row,
                          std::span<const SampleData> samples);
    /// Runs the 16x16 fragment program: params (scale, offset u, offset v, bias).
    std::vector<Vec4> Draw(const Program& program, const Texture& texture,
                           const D3D12_SAMPLER_DESC& sampler, const eden_integer_sampler_state& row,
                           const Vec4& params);

    /// GPU time of `repeat` dispatches of `groups` groups each, median, in milliseconds.
    double TimeDispatch(const Program& program, const Texture& texture,
                        const D3D12_SAMPLER_DESC& sampler, const eden_integer_sampler_state& row,
                        ID3D12Resource* data, u64 data_size, u32 groups, u32 repeat);

    void Reset() {
        resource_slot = 0;
        sampler_slot = 0;
        row_slot = 0;
    }
    ID3D12Device* Get() const {
        return device.Get();
    }
    ComPtr<ID3D12Resource> UploadBuffer(std::span<const std::byte> bytes);
    ComPtr<ID3D12Resource> DataBuffer(u64 size);

    Device device;
    ShaderCompiler compiler;
    RootSignatureCache root_signatures{device};
    ComPtr<ID3D12InfoQueue> messages;

private:
    void Begin();
    void Submit();
    void Bind(const Program& program, const Texture& texture, const D3D12_SAMPLER_DESC& sampler,
              const eden_integer_sampler_state& row, ID3D12Resource* data, u64 data_size);
    std::vector<std::byte> Readback(ID3D12Resource* buffer, u64 size);
    Program Build(std::span<const ShaderCompiler::PipelineStage> stages,
                  std::span<const Shader::Info* const> infos, bool graphics, bool emulate);

    static constexpr u32 RESOURCE_SLOTS = 3 * 64;
    static constexpr u32 SAMPLER_SLOTS = 64;
    static constexpr u32 ROW_SLOTS = 64;
    static constexpr u64 ROW_TABLE_SIZE =
        EDEN_INTEGER_SAMPLER_MAX_BINDINGS * sizeof(eden_integer_sampler_state);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    u64 fence_value = 0;
    HANDLE event = nullptr;
    ComPtr<ID3D12DescriptorHeap> resource_heap;
    ComPtr<ID3D12DescriptorHeap> sampler_heap;
    u32 resource_increment = 0;
    u32 sampler_increment = 0;
    u32 resource_slot = 0;
    u32 sampler_slot = 0;
    ComPtr<ID3D12Resource> rows; ///< ROW_SLOTS sampler tables, persistently mapped
    std::byte* rows_mapped = nullptr;
    u32 row_slot = 0;
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timestamp_readback;
    bool hardware_reduction = false;
    std::vector<ComPtr<ID3D12Resource>> keep_alive; ///< released after each submit
};

Bench::Bench() {
    ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)), "info queue");
    ID3D12Device* d = device.Get();
    ThrowIfFailed(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&allocator)),
                  "allocator");
    ThrowIfFailed(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&list)),
                  "list");
    ThrowIfFailed(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const D3D12_DESCRIPTOR_HEAP_DESC resources{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                               .NumDescriptors = RESOURCE_SLOTS,
                                               .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ThrowIfFailed(d->CreateDescriptorHeap(&resources, IID_PPV_ARGS(&resource_heap)), "heap");
    const D3D12_DESCRIPTOR_HEAP_DESC samplers{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                              .NumDescriptors = SAMPLER_SLOTS,
                                              .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ThrowIfFailed(d->CreateDescriptorHeap(&samplers, IID_PPV_ARGS(&sampler_heap)), "heap");
    resource_increment = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    sampler_increment = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    rows = CreateCommittedBuffer(d, ROW_TABLE_SIZE * ROW_SLOTS, D3D12_HEAP_TYPE_UPLOAD,
                                 D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
                                 "sampler rows");
    void* mapped = nullptr;
    ThrowIfFailed(rows->Map(0, nullptr, &mapped), "map rows");
    rows_mapped = static_cast<std::byte*>(mapped);
    const D3D12_QUERY_HEAP_DESC query{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 256};
    ThrowIfFailed(d->CreateQueryHeap(&query, IID_PPV_ARGS(&timestamps)), "timestamps");
    timestamp_readback = CreateCommittedBuffer(d, 256 * sizeof(u64), D3D12_HEAP_TYPE_READBACK,
                                               D3D12_RESOURCE_STATE_COPY_DEST,
                                               D3D12_RESOURCE_FLAG_NONE, "timestamp readback");
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    ThrowIfFailed(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
                  "options");
    hardware_reduction = options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2;
    Begin();
}

void Bench::MessagesCheck(const std::string& where) {
    std::string errors;
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
        SIZE_T size{};
        messages->GetMessage(i, nullptr, &size);
        std::vector<std::byte> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        ThrowIfFailed(messages->GetMessage(i, message, &size), "message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            errors += fmt::format("\n    [{}] {}", static_cast<int>(message->Severity),
                                  message->pDescription);
        }
    }
    messages->ClearStoredMessages();
    const HRESULT removed = device.Get()->GetDeviceRemovedReason();
    if (removed != S_OK) {
        throw Failure(fmt::format("{}: device removed 0x{:08X}", where, static_cast<u32>(removed)));
    }
    if (!errors.empty()) {
        throw Failure(fmt::format("{}: debug layer:{}", where, errors));
    }
}

void Bench::Begin() {
    ID3D12DescriptorHeap* heaps[] = {resource_heap.Get(), sampler_heap.Get()};
    list->SetDescriptorHeaps(2, heaps);
}

void Bench::Submit() {
    ThrowIfFailed(list->Close(), "close");
    ID3D12CommandList* lists[] = {list.Get()};
    device.Queue()->ExecuteCommandLists(1, lists);
    ThrowIfFailed(device.Queue()->Signal(fence.Get(), ++fence_value), "signal");
    if (fence->GetCompletedValue() < fence_value) {
        ThrowIfFailed(fence->SetEventOnCompletion(fence_value, event), "fence event");
        WaitForSingleObject(event, 30000);
    }
    CHECK(fence->GetCompletedValue() >= fence_value, "the GPU did not finish (removed 0x{:08X})",
          static_cast<u32>(device.Get()->GetDeviceRemovedReason()));
    keep_alive.clear();
    ThrowIfFailed(allocator->Reset(), "allocator reset");
    ThrowIfFailed(list->Reset(allocator.Get(), nullptr), "list reset");
    Reset();
    Begin();
}

ComPtr<ID3D12Resource> Bench::UploadBuffer(std::span<const std::byte> bytes) {
    auto buffer = CreateCommittedBuffer(device.Get(), std::max<u64>(bytes.size(), 256),
                                        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                        D3D12_RESOURCE_FLAG_NONE, "upload");
    void* mapped = nullptr;
    ThrowIfFailed(buffer->Map(0, nullptr, &mapped), "map upload");
    std::memcpy(mapped, bytes.data(), bytes.size());
    buffer->Unmap(0, nullptr);
    keep_alive.push_back(buffer);
    return buffer;
}

ComPtr<ID3D12Resource> Bench::DataBuffer(u64 size) {
    return CreateCommittedBuffer(device.Get(), size, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "sample data");
}

std::vector<std::byte> Bench::Readback(ID3D12Resource* buffer, u64 size) {
    auto readback = CreateCommittedBuffer(device.Get(), size, D3D12_HEAP_TYPE_READBACK,
                                          D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
                                          "readback");
    list->CopyBufferRegion(readback.Get(), 0, buffer, 0, size);
    Submit();
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(size)};
    ThrowIfFailed(readback->Map(0, &range, &mapped), "map readback");
    std::vector<std::byte> out(size);
    std::memcpy(out.data(), mapped, size);
    readback->Unmap(0, nullptr);
    return out;
}

Texture Bench::MakeTexture(const TextureSpec& spec, std::mt19937& rng) {
    Texture texture{.spec = spec};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = spec.dim == Dim::D1   ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                     : spec.dim == Dim::D3 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                           : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = spec.width;
    desc.Height = spec.dim == Dim::D1 ? 1 : spec.height;
    desc.DepthOrArraySize = static_cast<UINT16>(spec.depth);
    desc.MipLevels = static_cast<UINT16>(spec.levels);
    desc.Format = spec.format;
    desc.SampleDesc = {1, 0};
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ThrowIfFailed(device.Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&texture.resource)),
                  "texture");
    const u32 subresources = spec.levels * texture.Layers();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
    std::vector<UINT> row_counts(subresources);
    std::vector<UINT64> row_sizes(subresources);
    UINT64 total = 0;
    device.Get()->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(),
                                        row_counts.data(), row_sizes.data(), &total);
    std::vector<std::byte> staging(total);
    texture.texels.resize(subresources);
    // Distinct, exactly representable values: 8-bit steps, 1/4096 steps, small integers.
    for (u32 layer = 0; layer < texture.Layers(); ++layer) {
        for (u32 level = 0; level < spec.levels; ++level) {
            const u32 sub = level + layer * spec.levels;
            const u32 w = texture.Width(level), h = texture.Height(level),
                      d = texture.Depth(level);
            auto& texels = texture.texels[sub];
            texels.resize(static_cast<size_t>(w) * h * d);
            const auto& fp = footprints[sub];
            for (u32 z = 0; z < d; ++z) {
                for (u32 y = 0; y < h; ++y) {
                    std::byte* row = staging.data() + fp.Offset +
                                     (static_cast<u64>(z) * h + y) * fp.Footprint.RowPitch;
                    for (u32 x = 0; x < w; ++x) {
                        Vec4& texel = texels[(static_cast<size_t>(z) * h + y) * w + x];
                        if (spec.format == DXGI_FORMAT_R8G8B8A8_UNORM) {
                            for (u32 c = 0; c < 4; ++c) {
                                const u8 value = static_cast<u8>(rng());
                                row[x * 4 + c] = static_cast<std::byte>(value);
                                texel[c] = static_cast<float>(value) / 255.0f;
                            }
                        } else if (spec.format == DXGI_FORMAT_R32_FLOAT) {
                            const float value = static_cast<float>(rng() % 4096) / 4096.0f;
                            std::memcpy(row + x * 4, &value, 4);
                            texel = {value, 0.0f, 0.0f, 1.0f};
                        } else if (spec.format == DXGI_FORMAT_R32_UINT) {
                            const u32 value = rng() % 1000;
                            std::memcpy(row + x * 4, &value, 4);
                            texel = {static_cast<float>(value), 0.0f, 0.0f, 1.0f};
                        } else {
                            throw std::runtime_error("unsupported bench format");
                        }
                    }
                }
            }
        }
    }
    auto upload = UploadBuffer(staging);
    for (u32 sub = 0; sub < subresources; ++sub) {
        const D3D12_TEXTURE_COPY_LOCATION dst{.pResource = texture.resource.Get(),
                                              .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                              .SubresourceIndex = sub};
        const D3D12_TEXTURE_COPY_LOCATION src{.pResource = upload.Get(),
                                              .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                              .PlacedFootprint = footprints[sub]};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    const D3D12_RESOURCE_BARRIER barrier{
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {.pResource = texture.resource.Get(),
                       .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
                       .StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}};
    list->ResourceBarrier(1, &barrier);
    Submit();
    return texture;
}

Shader::Info MakeInfo(Shader::TextureType type, bool integer) {
    Shader::Info info{};
    Shader::StorageBufferDescriptor storage{};
    storage.count = 1;
    storage.is_written = true;
    info.storage_buffers_descriptors.push_back(storage);
    Shader::TextureDescriptor texture{};
    texture.type = type;
    texture.is_integer = integer;
    texture.count = 1;
    info.texture_descriptors.push_back(texture);
    return info;
}

Shader::TextureType TypeOf(Dim dim) {
    switch (dim) {
    case Dim::D1:
        return Shader::TextureType::Color1D;
    case Dim::D2:
        return Shader::TextureType::Color2D;
    case Dim::Array2D:
        return Shader::TextureType::ColorArray2D;
    case Dim::D3:
        return Shader::TextureType::Color3D;
    }
    return Shader::TextureType::Color2D;
}

Program Bench::Build(std::span<const ShaderCompiler::PipelineStage> stages,
                     std::span<const Shader::Info* const> infos, bool graphics, bool emulate) {
    Program program{.graphics = graphics};
    ShaderCompiler::PipelineOptions options{};
    options.emulate_reduction = emulate;
    const auto start = std::chrono::steady_clock::now();
    auto compiled = compiler.CompilePipeline(stages, options);
    program.compile_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    program.layout = &root_signatures.Get(infos, !graphics, false, emulate);
    for (const auto& stage : compiled) {
        program.dxil_size += stage.dxil.size();
    }
    if (!graphics) {
        const D3D12_COMPUTE_PIPELINE_STATE_DESC desc{
            .pRootSignature = program.layout->Handle(),
            .CS = {compiled[0].dxil.data(), compiled[0].dxil.size()}};
        ThrowIfFailed(device.Get()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&program.pso)),
                      "compute pso");
        return program;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = program.layout->Handle();
    desc.VS = {compiled[0].dxil.data(), compiled[0].dxil.size()};
    desc.PS = {compiled[1].dxil.data(), compiled[1].dxil.size()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 0;
    desc.SampleDesc = {1, 0};
    ThrowIfFailed(device.Get()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&program.pso)),
                  "graphics pso");
    return program;
}

Program Bench::Compute(Dim dim, Op op, bool emulate) {
    const auto spirv =
        LoadSpirv(fmt::format("comp_d{}_o{}.spv", static_cast<u32>(dim), static_cast<u32>(op)));
    const std::array stages{ShaderCompiler::PipelineStage{spirv, DXIL_SPIRV_SHADER_COMPUTE}};
    const Shader::Info info = MakeInfo(TypeOf(dim), false);
    const std::array<const Shader::Info*, 1> infos{&info};
    return Build(stages, infos, false, emulate);
}

Program Bench::ComputeInteger(bool emulate) {
    const auto spirv = LoadSpirv("comp_int.spv");
    const std::array stages{ShaderCompiler::PipelineStage{spirv, DXIL_SPIRV_SHADER_COMPUTE}};
    const Shader::Info info = MakeInfo(Shader::TextureType::Color2D, true);
    const std::array<const Shader::Info*, 1> infos{&info};
    return Build(stages, infos, false, emulate);
}

Program Bench::Fragment(bool bias, bool emulate) {
    const auto vs = LoadSpirv("vs.spv");
    const auto fs = LoadSpirv(bias ? "fs_bias.spv" : "fs_tex.spv");
    const std::array stages{ShaderCompiler::PipelineStage{vs, DXIL_SPIRV_SHADER_VERTEX},
                            ShaderCompiler::PipelineStage{fs, DXIL_SPIRV_SHADER_FRAGMENT}};
    const Shader::Info vs_info{};
    const Shader::Info fs_info = MakeInfo(Shader::TextureType::Color2D, false);
    const std::array<const Shader::Info*, 2> infos{&vs_info, &fs_info};
    return Build(stages, infos, true, emulate);
}

void Bench::Bind(const Program& program, const Texture& texture,
                 const D3D12_SAMPLER_DESC& sampler, const eden_integer_sampler_state& row,
                 ID3D12Resource* data, u64 data_size) {
    CHECK(resource_slot + 3 <= RESOURCE_SLOTS && sampler_slot < SAMPLER_SLOTS &&
              row_slot < ROW_SLOTS,
          "too many binds in one submit");
    ID3D12Device* d = device.Get();
    const auto cpu = [&](u32 slot) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = resource_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * resource_increment;
        return handle;
    };
    // The layout's table: storage buffer UAV, its SRV alias, then the texture SRV (binding 1).
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{.Format = DXGI_FORMAT_R32_TYPELESS,
                                         .ViewDimension = D3D12_UAV_DIMENSION_BUFFER};
    uav.Buffer = {.FirstElement = 0, .NumElements = static_cast<UINT>(data_size / 4),
                  .Flags = D3D12_BUFFER_UAV_FLAG_RAW};
    d->CreateUnorderedAccessView(data, nullptr, &uav, cpu(resource_slot));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{.Format = DXGI_FORMAT_R32_TYPELESS,
                                        .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                                        .Shader4ComponentMapping =
                                            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    srv.Buffer = {.FirstElement = 0, .NumElements = static_cast<UINT>(data_size / 4),
                  .Flags = D3D12_BUFFER_SRV_FLAG_RAW};
    d->CreateShaderResourceView(data, &srv, cpu(resource_slot + 1));
    D3D12_SHADER_RESOURCE_VIEW_DESC view{.Format = texture.spec.format,
                                         .Shader4ComponentMapping =
                                             D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    const UINT levels = texture.spec.levels;
    switch (texture.spec.dim) {
    case Dim::D1:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
        view.Texture1D.MipLevels = levels;
        break;
    case Dim::D2:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = levels;
        break;
    case Dim::Array2D:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray = {.MipLevels = levels, .ArraySize = texture.spec.depth};
        break;
    case Dim::D3:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        view.Texture3D.MipLevels = levels;
        break;
    }
    d->CreateShaderResourceView(texture.resource.Get(), &view, cpu(resource_slot + 2));
    D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu = sampler_heap->GetCPUDescriptorHandleForHeapStart();
    sampler_cpu.ptr += static_cast<SIZE_T>(sampler_slot) * sampler_increment;
    d->CreateSampler(&sampler, sampler_cpu);

    D3D12_GPU_DESCRIPTOR_HANDLE resources = resource_heap->GetGPUDescriptorHandleForHeapStart();
    resources.ptr += static_cast<u64>(resource_slot) * resource_increment;
    D3D12_GPU_DESCRIPTOR_HANDLE samplers = sampler_heap->GetGPUDescriptorHandleForHeapStart();
    samplers.ptr += static_cast<u64>(sampler_slot) * sampler_increment;
    resource_slot += 3;
    ++sampler_slot;

    // The table is indexed by texture binding: row 1. The rest stays zero (reduction NONE).
    std::byte* table = rows_mapped + row_slot * ROW_TABLE_SIZE;
    std::memset(table, 0, ROW_TABLE_SIZE);
    std::memcpy(table + sizeof(row), &row, sizeof(row));
    const D3D12_GPU_VIRTUAL_ADDRESS table_address =
        rows->GetGPUVirtualAddress() + row_slot * ROW_TABLE_SIZE;
    ++row_slot;

    const PipelineLayout& layout = *program.layout;
    const std::array<u32, 32> zeros{};
    list->SetPipelineState(program.pso.Get());
    if (program.graphics) {
        list->SetGraphicsRootSignature(layout.Handle());
        list->SetGraphicsRoot32BitConstants(PipelineLayout::PUSH_CONSTANTS_INDEX,
                                            PUSH_CONSTANT_WORDS, zeros.data(), 0);
        list->SetGraphicsRoot32BitConstants(PipelineLayout::RUNTIME_DATA_INDEX,
                                            layout.RuntimeDataWords(), zeros.data(), 0);
        list->SetGraphicsRootDescriptorTable(layout.ResourceTableIndex(), resources);
        list->SetGraphicsRootDescriptorTable(layout.SamplerTableIndex(), samplers);
        if (layout.IntegerSamplerIndex() != PipelineLayout::NO_TABLE) {
            list->SetGraphicsRootConstantBufferView(layout.IntegerSamplerIndex(), table_address);
        }
    } else {
        list->SetComputeRootSignature(layout.Handle());
        list->SetComputeRoot32BitConstants(PipelineLayout::PUSH_CONSTANTS_INDEX,
                                           PUSH_CONSTANT_WORDS, zeros.data(), 0);
        list->SetComputeRoot32BitConstants(PipelineLayout::RUNTIME_DATA_INDEX,
                                           layout.RuntimeDataWords(), zeros.data(), 0);
        list->SetComputeRootDescriptorTable(layout.ResourceTableIndex(), resources);
        list->SetComputeRootDescriptorTable(layout.SamplerTableIndex(), samplers);
        if (layout.IntegerSamplerIndex() != PipelineLayout::NO_TABLE) {
            list->SetComputeRootConstantBufferView(layout.IntegerSamplerIndex(), table_address);
        }
    }
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    const D3D12_RESOURCE_BARRIER barrier{
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {.pResource = resource,
                       .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = before,
                       .StateAfter = after}};
    list->ResourceBarrier(1, &barrier);
}

std::vector<Vec4> Bench::Run(const Program& program, const Texture& texture,
                             const D3D12_SAMPLER_DESC& sampler,
                             const eden_integer_sampler_state& row,
                             std::span<const SampleData> samples) {
    CHECK(!program.graphics, "Run takes a compute program");
    std::vector<SampleData> padded(samples.begin(), samples.end());
    padded.resize((samples.size() + 63) / 64 * 64, samples.front());
    const u64 size = padded.size() * sizeof(SampleData);
    auto data = DataBuffer(size);
    auto upload = UploadBuffer(std::as_bytes(std::span{padded}));
    list->CopyBufferRegion(data.Get(), 0, upload.Get(), 0, size);
    Transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Bind(program, texture, sampler, row, data.Get(), size);
    list->Dispatch(static_cast<UINT>(padded.size() / 64), 1, 1);
    Transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    const auto bytes = Readback(data.Get(), size);
    std::vector<Vec4> out(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        std::memcpy(out[i].data(), bytes.data() + i * sizeof(SampleData) + offsetof(SampleData, result),
                    sizeof(Vec4));
    }
    return out;
}

std::vector<Vec4> Bench::Draw(const Program& program, const Texture& texture,
                              const D3D12_SAMPLER_DESC& sampler,
                              const eden_integer_sampler_state& row, const Vec4& params) {
    CHECK(program.graphics, "Draw takes a graphics program");
    constexpr u32 PIXELS = 16 * 16;
    const u64 size = (1 + PIXELS) * sizeof(Vec4);
    std::vector<Vec4> initial(1 + PIXELS, Vec4{-1.0f, -1.0f, -1.0f, -1.0f});
    initial[0] = params;
    auto data = DataBuffer(size);
    auto upload = UploadBuffer(std::as_bytes(std::span{initial}));
    list->CopyBufferRegion(data.Get(), 0, upload.Get(), 0, size);
    Transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Bind(program, texture, sampler, row, data.Get(), size);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, 16, 16};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    Transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    const auto bytes = Readback(data.Get(), size);
    std::vector<Vec4> out(PIXELS);
    std::memcpy(out.data(), bytes.data() + sizeof(Vec4), PIXELS * sizeof(Vec4));
    return out;
}

double Bench::TimeDispatch(const Program& program, const Texture& texture,
                           const D3D12_SAMPLER_DESC& sampler,
                           const eden_integer_sampler_state& row, ID3D12Resource* data,
                           u64 data_size, u32 groups, u32 repeat) {
    CHECK(repeat * 2 <= 256, "too many timestamps");
    Bind(program, texture, sampler, row, data, data_size);
    const D3D12_RESOURCE_BARRIER uav{.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
                                     .UAV = {.pResource = data}};
    for (u32 i = 0; i < repeat; ++i) {
        list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2);
        list->Dispatch(groups, 1, 1);
        list->ResourceBarrier(1, &uav);
        list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2 + 1);
    }
    list->ResolveQueryData(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, repeat * 2,
                           timestamp_readback.Get(), 0);
    Submit();
    UINT64 frequency = 0;
    ThrowIfFailed(device.Queue()->GetTimestampFrequency(&frequency), "timestamp frequency");
    void* mapped = nullptr;
    const D3D12_RANGE range{0, repeat * 2 * sizeof(u64)};
    ThrowIfFailed(timestamp_readback->Map(0, &range, &mapped), "map timestamps");
    const auto* ticks = static_cast<const u64*>(mapped);
    std::vector<double> times(repeat);
    for (u32 i = 0; i < repeat; ++i) {
        times[i] = static_cast<double>(ticks[i * 2 + 1] - ticks[i * 2]) * 1000.0 /
                   static_cast<double>(frequency);
    }
    timestamp_readback->Unmap(0, nullptr);
    std::nth_element(times.begin(), times.begin() + repeat / 2, times.end());
    return times[repeat / 2];
}

// ---------------------------------------------------------------------------------------------
// Comparisons

bool Near(const Vec4& a, const Vec4& b, float tolerance) {
    for (size_t c = 0; c < 4; ++c) {
        if (!(std::abs(a[c] - b[c]) <= tolerance)) {
            return false;
        }
    }
    return true;
}

std::string Show(const Vec4& v) {
    return fmt::format("({:.6g}, {:.6g}, {:.6g}, {:.6g})", v[0], v[1], v[2], v[3]);
}

/// Throws with the first mismatches. tolerance 0: bit-exact.
void Compare(std::span<const Vec4> got, std::span<const Vec4> want,
             std::span<const SampleData> samples, float tolerance, const std::string& what) {
    CHECK(got.size() == want.size(), "{}: {} results, {} expected", what, got.size(), want.size());
    u32 mismatches = 0;
    std::string details;
    for (size_t i = 0; i < got.size(); ++i) {
        const bool same = tolerance == 0.0f
                              ? std::memcmp(got[i].data(), want[i].data(), sizeof(Vec4)) == 0
                              : Near(got[i], want[i], tolerance);
        if (same) {
            continue;
        }
        if (++mismatches <= 4) {
            details += fmt::format("\n      #{} ", i);
            if (!samples.empty()) {
                const SampleData& s = samples[i];
                details += fmt::format("coord ({:.9g}, {:.9g}, {:.9g}) lod {} ", s.coord[0],
                                       s.coord[1], s.coord[2], s.coord[3]);
            }
            details += fmt::format("got {} want {}", Show(got[i]), Show(want[i]));
        }
    }
    CHECK(mismatches == 0, "{}: {} of {} differ{}", what, mismatches, got.size(), details);
}

// ---------------------------------------------------------------------------------------------
// Oracle runs

struct Case {
    TextureSpec texture;
    Op op = Op::Lod;
    SamplerConfig sampler;
    SampleOptions options{};
    u32 count = 256;
};

std::string Describe(const Case& c) {
    return fmt::format("{} {} {}x{}x{} L{} fmt {} | {}", DimName(c.texture.dim), OpName(c.op),
                       c.texture.width, c.texture.height, c.texture.depth, c.texture.levels,
                       static_cast<int>(c.texture.format), c.sampler.Describe());
}

/// Emulated (STANDARD sampler + row) against the device's own MIN/MAX sampler on the plain shader.
void CheckAgainstHardware(Bench& bench, const Case& c, std::mt19937& rng) {
    const Texture texture = bench.MakeTexture(c.texture, rng);
    const auto samples = MakeSamples(texture, c.op, c.options, rng, c.count);
    const Program emulated = bench.Compute(c.texture.dim, c.op, true);
    const Program plain = bench.Compute(c.texture.dim, c.op, false);
    const auto row = MakeRow(c.sampler, c.texture);
    const auto got = bench.Run(emulated, texture, MakeSampler(c.sampler, false), row, samples);
    const auto want = bench.Run(plain, texture, MakeSampler(c.sampler, true), row, samples);
    Compare(got, want, samples, 1e-6f, Describe(c));
}

/// A NONE row: the emulating shader returns exactly what the plain one does.
void CheckNoneIdentical(Bench& bench, Case c, std::mt19937& rng) {
    c.sampler.reduction = EDEN_SAMPLER_REDUCTION_NONE;
    const Texture texture = bench.MakeTexture(c.texture, rng);
    const auto samples = MakeSamples(texture, c.op, c.options, rng, c.count);
    const Program emulated = bench.Compute(c.texture.dim, c.op, true);
    const Program plain = bench.Compute(c.texture.dim, c.op, false);
    const auto desc = MakeSampler(c.sampler, false);
    const auto row = MakeRow(c.sampler, c.texture);
    const auto got = bench.Run(emulated, texture, desc, row, samples);
    const auto want = bench.Run(plain, texture, desc, row, samples);
    Compare(got, want, samples, 0.0f, Describe(c));
}

void RequireHardware(const Bench& bench) {
    if (!bench.HardwareReduction()) {
        throw Failure("SKIP: the device has no MIN/MAX samplers (tiled resources tier < 2) to "
                      "compare against");
    }
}

const std::array<D3D12_TEXTURE_ADDRESS_MODE, 5> WRAPS{
    D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
    D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
    D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE};

Vec4 RandomBorder(std::mt19937& rng) {
    // 8-bit steps: exact in every bench format.
    return {static_cast<float>(rng() % 256) / 255.0f, static_cast<float>(rng() % 256) / 255.0f,
            static_cast<float>(rng() % 256) / 255.0f, static_cast<float>(rng() % 256) / 255.0f};
}

TextureSpec SpecFor(Dim dim) {
    switch (dim) {
    case Dim::D1:
        return {.dim = Dim::D1, .width = 32, .height = 1, .depth = 1, .levels = 6};
    case Dim::D2:
        return {.dim = Dim::D2, .width = 16, .height = 16, .depth = 1, .levels = 5};
    case Dim::Array2D:
        return {.dim = Dim::Array2D, .width = 8, .height = 8, .depth = 4, .levels = 4};
    case Dim::D3:
        return {.dim = Dim::D3, .width = 8, .height = 8, .depth = 8, .levels = 4};
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// Tests

struct Test {
    const char* category;
    const char* name;
    std::function<void(Bench&)> run;
};

std::mt19937 Rng(u32 salt) {
    return std::mt19937(g_seed * 0x9E3779B9u + salt);
}

const std::vector<Test> TESTS{
    // --- sanity ------------------------------------------------------------------------------
    {"sanity", "the compiler emulates reduction",
     [](Bench& bench) {
         CHECK(bench.compiler.IsAvailable(), "no shader compiler");
         CHECK(bench.compiler.CanLinkStages(), "spirv_to_dxil without the pipeline entry point");
         CHECK(bench.compiler.EmulatesReduction(),
               "spirv_to_dxil.dll lacks EDEN_SPIRV_TO_DXIL_EMULATE_REDUCTION");
         std::cout << fmt::format("    device {}: hardware MIN/MAX samplers {}\n",
                                  bench.device.AdapterName(),
                                  bench.HardwareReduction() ? "yes (oracle)" : "no");
     }},
    {"sanity", "layouts carry the sampler table only when emulating",
     [](Bench& bench) {
         const Shader::Info info = MakeInfo(Shader::TextureType::Color2D, false);
         const std::array<const Shader::Info*, 1> infos{&info};
         const auto& plain = bench.root_signatures.Get(infos, true, false, false);
         const auto& emulated = bench.root_signatures.Get(infos, true, false, true);
         CHECK(plain.IntegerSamplerIndex() == PipelineLayout::NO_TABLE, "plain layout has a table");
         CHECK(emulated.IntegerSamplerIndex() != PipelineLayout::NO_TABLE,
               "emulating layout lacks the table");
         CHECK(&plain != &emulated, "both variants share one layout");
         Shader::Info no_texture{};
         no_texture.storage_buffers_descriptors.push_back(info.storage_buffers_descriptors[0]);
         const std::array<const Shader::Info*, 1> bare{&no_texture};
         CHECK(bench.root_signatures.Get(bare, true, false, true).IntegerSamplerIndex() ==
                   PipelineLayout::NO_TABLE,
               "a pipeline without textures got a table");
         const Shader::Info integer = MakeInfo(Shader::TextureType::Color2D, true);
         const std::array<const Shader::Info*, 1> integers{&integer};
         CHECK(bench.root_signatures.Get(integers, true, false, false).IntegerSamplerIndex() !=
                   PipelineLayout::NO_TABLE,
               "integer textures lost their table");
     }},
    {"sanity", "the flag changes the DXIL of float samples",
     [](Bench& bench) {
         for (const Dim dim : {Dim::D1, Dim::D2, Dim::Array2D, Dim::D3}) {
             const Program plain = bench.Compute(dim, Op::Lod, false);
             const Program emulated = bench.Compute(dim, Op::Lod, true);
             CHECK(emulated.dxil_size > plain.dxil_size, "{}: emulated DXIL {} B, plain {} B",
                   DimName(dim), emulated.dxil_size, plain.dxil_size);
         }
     }},

    // --- functional ----------------------------------------------------------------------------
    {"functional", "every filter combination, MIN and MAX",
     [](Bench& bench) {
         RequireHardware(bench);
         auto rng = Rng(1);
         for (u32 bits = 0; bits < 8; ++bits) {
             for (const u32 reduction : {EDEN_SAMPLER_REDUCTION_MIN, EDEN_SAMPLER_REDUCTION_MAX}) {
                 Case c{.texture = SpecFor(Dim::D2)};
                 c.sampler.min_linear = (bits & 1) != 0;
                 c.sampler.mag_linear = (bits & 2) != 0;
                 c.sampler.mip_linear = (bits & 4) != 0;
                 c.sampler.reduction = reduction;
                 CheckAgainstHardware(bench, c, rng);
             }
         }
     }},
    {"functional", "every address mode, border color included",
     [](Bench& bench) {
         RequireHardware(bench);
         auto rng = Rng(2);
         for (size_t i = 0; i < WRAPS.size(); ++i) {
             for (const u32 reduction : {EDEN_SAMPLER_REDUCTION_MIN, EDEN_SAMPLER_REDUCTION_MAX}) {
                 Case c{.texture = SpecFor(Dim::D2)};
                 c.sampler.reduction = reduction;
                 c.sampler.wrap = {WRAPS[i], WRAPS[(i + 1) % WRAPS.size()], WRAPS[i]};
                 c.sampler.border = RandomBorder(rng);
                 c.options = {.lo = -1.5f, .hi = 2.5f};
                 CheckAgainstHardware(bench, c, rng);
             }
         }
     }},
    {"functional", "1D, 2D array and 3D textures",
     [](Bench& bench) {
         RequireHardware(bench);
         auto rng = Rng(3);
         for (const Dim dim : {Dim::D1, Dim::Array2D, Dim::D3}) {
             for (u32 bits : {7u, 1u, 6u}) {
                 for (const u32 reduction :
                      {EDEN_SAMPLER_REDUCTION_MIN, EDEN_SAMPLER_REDUCTION_MAX}) {
                     Case c{.texture = SpecFor(dim)};
                     c.sampler.min_linear = (bits & 1) != 0;
                     c.sampler.mag_linear = (bits & 2) != 0;
                     c.sampler.mip_linear = (bits & 4) != 0;
                     c.sampler.reduction = reduction;
                     c.sampler.wrap = {D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                       D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                                       D3D12_TEXTURE_ADDRESS_MODE_MIRROR};
                     c.sampler.border = RandomBorder(rng);
                     CheckAgainstHardware(bench, c, rng);
                 }
             }
         }
     }},
    {"functional", "gradients and texel offsets",
     [](Bench& bench) {
         RequireHardware(bench);
         auto rng = Rng(4);
         for (const Dim dim : {Dim::D1, Dim::D2, Dim::Array2D, Dim::D3}) {
             for (const Op op : {Op::Grad, Op::LodOffset, Op::GradOffset}) {
                 Case c{.texture = SpecFor(dim), .op = op};
                 c.sampler.mip_linear = (rng() & 1) != 0;
                 c.sampler.reduction =
                     (rng() & 1) ? EDEN_SAMPLER_REDUCTION_MIN : EDEN_SAMPLER_REDUCTION_MAX;
                 c.sampler.wrap = {D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                   D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                   D3D12_TEXTURE_ADDRESS_MODE_MIRROR};
                 CheckAgainstHardware(bench, c, rng);
             }
         }
     }},
    {"functional", "R32_FLOAT, border on missing channels",
     [](Bench& bench) {
         RequireHardware(bench);
         auto rng = Rng(5);
         for (const u32 reduction : {EDEN_SAMPLER_REDUCTION_MIN, EDEN_SAMPLER_REDUCTION_MAX}) {
             for (const auto wrap :
                  {D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_BORDER}) {
                 Case c{.texture = SpecFor(Dim::D2)};
                 c.texture.format = DXGI_FORMAT_R32_FLOAT;
                 c.sampler.reduction = reduction;
                 c.sampler.wrap = {wrap, wrap, wrap};
                 c.sampler.border = RandomBorder(rng);
                 c.options = {.lo = -1.0f, .hi = 2.0f};
                 CheckAgainstHardware(bench, c, rng);
             }
         }
     }},
    {"functional", "sampler LOD bias and clamps with explicit LODs",
     [](Bench& bench) {
         RequireHardware(bench);
         auto rng = Rng(6);
         const std::array<std::array<float, 3>, 5> clamps{{
             {0.0f, 1.25f, 3.0f}, // bias, min, max
             {0.0f, 0.0f, 1.75f},
             {0.75f, 0.0f, 1000.0f},
             {-1.25f, 0.25f, 2.75f},
             {2.0f, 3.0f, 3.0f},
         }};
         for (const auto& [bias, min_lod, max_lod] : clamps) {
             for (const Op op : {Op::Lod, Op::Grad}) {
                 Case c{.texture = SpecFor(Dim::D2), .op = op};
                 c.sampler.bias = bias;
                 c.sampler.min_lod = min_lod;
                 c.sampler.max_lod = max_lod;
                 CheckAgainstHardware(bench, c, rng);
             }
         }
     }},
    {"functional", "non-power-of-two level 0",
     [](Bench& bench) {
         RequireHardware(bench);
         auto rng = Rng(7);
         static constexpr std::array<float, 4> level0{-1.25f, -0.25f, 0.25f, 0.0f};
         for (u32 bits : {7u, 3u, 1u, 2u}) {
             Case c{.texture = {.dim = Dim::D2, .width = 12, .height = 10, .depth = 1, .levels = 4}};
             c.sampler.min_linear = (bits & 1) != 0;
             c.sampler.mag_linear = (bits & 2) != 0;
             c.sampler.mip_linear = false;
             c.sampler.wrap = {D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
                               D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
             c.options = {.lo = -0.5f, .hi = 1.5f, .lods = level0};
             CheckAgainstHardware(bench, c, rng);
         }
     }},
    {"functional", "implicit derivative LOD in a pixel shader, with and without bias",
     [](Bench& bench) {
         RequireHardware(bench);
         auto rng = Rng(8);
         const Texture texture = bench.MakeTexture(SpecFor(Dim::D2), rng);
         for (const bool shader_bias : {false, true}) {
             const Program emulated = bench.Fragment(shader_bias, true);
             const Program plain = bench.Fragment(shader_bias, false);
             // scale 2^k / 16 texels per pixel: LOD k, exact; biases keep it off the ties.
             for (const int k : {-2, 0, 1, 2}) {
                 for (const float sampler_bias : {0.0f, 0.25f, -0.75f, 1.25f}) {
                     SamplerConfig config{};
                     config.bias = sampler_bias;
                     config.mip_linear = (rng() & 1) != 0;
                     config.reduction =
                         (rng() & 1) ? EDEN_SAMPLER_REDUCTION_MIN : EDEN_SAMPLER_REDUCTION_MAX;
                     const float scale = std::exp2(static_cast<float>(k)) / 16.0f;
                     const Vec4 params{scale, static_cast<float>(rng() % 64) / 4096.0f,
                                       static_cast<float>(rng() % 64) / 4096.0f,
                                       shader_bias ? 0.75f : 0.0f};
                     const auto row = MakeRow(config, texture.spec);
                     const auto got =
                         bench.Draw(emulated, texture, MakeSampler(config, false), row, params);
                     const auto want =
                         bench.Draw(plain, texture, MakeSampler(config, true), row, params);
                     Compare(got, want, {}, 1e-6f,
                             fmt::format("k {} shader bias {} | {}", k, params[3],
                                         config.Describe()));
                 }
             }
         }
     }},

    // --- regression ----------------------------------------------------------------------------
    {"regression", "a NONE row leaves results bit-identical to the plain shader",
     [](Bench& bench) {
         auto rng = Rng(20);
         for (const Dim dim : {Dim::D1, Dim::D2, Dim::Array2D, Dim::D3}) {
             for (const Op op : {Op::Lod, Op::Grad, Op::LodOffset, Op::GradOffset}) {
                 Case c{.texture = SpecFor(dim), .op = op};
                 c.sampler.mip_linear = (rng() & 1) != 0;
                 c.sampler.wrap = {WRAPS[rng() % 5], WRAPS[rng() % 5], WRAPS[rng() % 5]};
                 c.sampler.border = RandomBorder(rng);
                 CheckNoneIdentical(bench, c, rng);
             }
         }
         // The implicit LOD path too: the emulating shader computes the LOD outside its branch.
         const Texture texture = bench.MakeTexture(SpecFor(Dim::D2), rng);
         SamplerConfig config{};
         config.reduction = EDEN_SAMPLER_REDUCTION_NONE;
         const auto row = MakeRow(config, texture.spec);
         for (const bool shader_bias : {false, true}) {
             const Vec4 params{0.3f / 16.0f, 0.01f, 0.02f, 0.6f};
             const auto got = bench.Draw(bench.Fragment(shader_bias, true), texture,
                                         MakeSampler(config, false), row, params);
             const auto want = bench.Draw(bench.Fragment(shader_bias, false), texture,
                                          MakeSampler(config, false), row, params);
             Compare(got, want, {}, 0.0f, "implicit LOD");
         }
     }},
    {"regression", "Hi-Z 2:1 MIN downsample (MIN_MAG_LINEAR_MIP_POINT, clamp) is the 2x2 minimum",
     [](Bench& bench) {
         // The sampler (D3D12 filter 0x114) that removed the Series' device before emulation.
         auto rng = Rng(21);
         const TextureSpec spec{.dim = Dim::D2, .format = DXGI_FORMAT_R32_FLOAT, .width = 32,
                                .height = 16, .depth = 1, .levels = 5};
         const Texture texture = bench.MakeTexture(spec, rng);
         SamplerConfig config{};
         config.mip_linear = false;
         config.reduction = EDEN_SAMPLER_REDUCTION_MIN;
         const Program emulated = bench.Compute(Dim::D2, Op::Lod, true);
         for (u32 level = 0; level + 1 < spec.levels; ++level) {
             const u32 w = texture.Width(level) / 2, h = texture.Height(level) / 2;
             std::vector<SampleData> samples;
             std::vector<Vec4> want;
             for (u32 y = 0; y < h; ++y) {
                 for (u32 x = 0; x < w; ++x) {
                     SampleData s{};
                     s.coord = {(2.0f * x + 1.0f) / static_cast<float>(texture.Width(level)),
                                (2.0f * y + 1.0f) / static_cast<float>(texture.Height(level)), 0.0f,
                                static_cast<float>(level)};
                     samples.push_back(s);
                     float m = INFINITY;
                     for (u32 j = 0; j < 2; ++j) {
                         for (u32 i = 0; i < 2; ++i) {
                             m = std::min(m, texture.At(level, 0, 2 * x + i, 2 * y + j, 0)[0]);
                         }
                     }
                     want.push_back({m, 0.0f, 0.0f, 1.0f});
                 }
             }
             const auto got = bench.Run(emulated, texture, MakeSampler(config, false),
                                        MakeRow(config, spec), samples);
             Compare(got, want, samples, 0.0f, fmt::format("level {}", level));
         }
     }},
    {"regression", "LOD <= 0 magnifies, LOD > 0 minifies",
     [](Bench& bench) {
         auto rng = Rng(22);
         const TextureSpec spec{.dim = Dim::D2, .format = DXGI_FORMAT_R32_FLOAT, .width = 16,
                                .height = 16, .depth = 1, .levels = 5};
         const Texture texture = bench.MakeTexture(spec, rng);
         const Program emulated = bench.Compute(Dim::D2, Op::Lod, true);
         // Point on one side, linear on the other; MIN of the 2x2 footprint or a single texel.
         for (const bool linear_mag : {true, false}) {
             SamplerConfig config{};
             config.mag_linear = linear_mag;
             config.min_linear = !linear_mag;
             config.mip_linear = false;
             config.reduction = EDEN_SAMPLER_REDUCTION_MIN;
             std::vector<SampleData> samples;
             std::vector<Vec4> want;
             for (u32 i = 0; i < 64; ++i) {
                 const bool magnify = (i & 1) == 0;
                 const u32 level = magnify ? 0 : 1;
                 const u32 size = texture.Width(level);
                 // Texel position (x + 0.25, y + 0.75) at the level: a 2x2 footprint at x, y.
                 const u32 x = rng() % (size - 1), y = rng() % (size - 1);
                 SampleData s{};
                 s.coord = {(static_cast<float>(x) + 0.75f) / static_cast<float>(size),
                            (static_cast<float>(y) + 1.25f) / static_cast<float>(size), 0.0f,
                            magnify ? -1.0f : 1.0f};
                 samples.push_back(s);
                 const bool linear = magnify == linear_mag;
                 float m = texture.At(level, 0, x, y + (linear ? 0 : 1), 0)[0];
                 if (linear) {
                     m = std::min({m, texture.At(level, 0, x + 1, y, 0)[0],
                                   texture.At(level, 0, x, y + 1, 0)[0],
                                   texture.At(level, 0, x + 1, y + 1, 0)[0]});
                 }
                 want.push_back({m, 0.0f, 0.0f, 1.0f});
             }
             const auto got = bench.Run(emulated, texture, MakeSampler(config, false),
                                        MakeRow(config, spec), samples);
             Compare(got, want, samples, 0.0f, config.Describe());
         }
     }},
    {"regression", "integer samples ignore the reduction and still load texels",
     [](Bench& bench) {
         auto rng = Rng(23);
         const TextureSpec spec{.dim = Dim::D2, .format = DXGI_FORMAT_R32_UINT, .width = 8,
                                .height = 8, .depth = 1, .levels = 3};
         const Texture texture = bench.MakeTexture(spec, rng);
         SamplerConfig config{};
         config.min_linear = config.mag_linear = config.mip_linear = false;
         config.reduction = EDEN_SAMPLER_REDUCTION_MIN;
         std::vector<SampleData> samples;
         std::vector<Vec4> want;
         for (u32 i = 0; i < 128; ++i) {
             const u32 level = rng() % spec.levels;
             const u32 size = texture.Width(level);
             const u32 x = rng() % size, y = rng() % size;
             SampleData s{};
             s.coord = {(static_cast<float>(x) + 0.25f) / static_cast<float>(size),
                        (static_cast<float>(y) + 0.75f) / static_cast<float>(size), 0.0f,
                        static_cast<float>(level)};
             samples.push_back(s);
             want.push_back(texture.At(level, 0, x, y, 0));
         }
         for (const bool emulate : {false, true}) {
             const Program program = bench.ComputeInteger(emulate);
             const auto got = bench.Run(program, texture, MakeSampler(config, false),
                                        MakeRow(config, spec), samples);
             Compare(got, want, samples, 0.0f, emulate ? "emulating" : "plain");
         }
     }},
    {"regression", "integer samples wrap negative and non-power-of-two coordinates, with bias",
     [](Bench& bench) {
         // The texel wrap shared with the reduction: repeat and mirror of negative texels (an
         // unsigned imod read them past the edge), 12-wide levels (fmod returned 12 mod 12 = 12)
         // and the sampler bias on explicit LODs.
         auto rng = Rng(24);
         const TextureSpec spec{.dim = Dim::D2, .format = DXGI_FORMAT_R32_UINT, .width = 12,
                                .height = 10, .depth = 1, .levels = 3};
         const Texture texture = bench.MakeTexture(spec, rng);
         SamplerConfig config{};
         config.min_linear = config.mag_linear = config.mip_linear = false;
         config.reduction = EDEN_SAMPLER_REDUCTION_NONE;
         config.wrap = {D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
         config.bias = 1.0f;
         const auto wrap = [](s32 k, s32 size) { return ((k % size) + size) % size; };
         const auto mirror = [](s32 k, s32 size) {
             const s32 m = ((k % (2 * size)) + 2 * size) % (2 * size);
             return m < size ? m : 2 * size - 1 - m;
         };
         std::vector<SampleData> samples;
         std::vector<Vec4> want;
         for (u32 i = 0; i < 256; ++i) {
             const u32 level = rng() % spec.levels;
             const auto width = static_cast<s32>(texture.Width(level));
             const auto height = static_cast<s32>(texture.Height(level));
             const s32 x = static_cast<s32>(rng() % (5 * width)) - 2 * width;
             const s32 y = static_cast<s32>(rng() % (5 * height)) - 2 * height;
             SampleData s{};
             s.coord = {(static_cast<float>(x) + 0.25f) / static_cast<float>(width),
                        (static_cast<float>(y) + 0.75f) / static_cast<float>(height), 0.0f,
                        static_cast<float>(level) - config.bias};
             samples.push_back(s);
             want.push_back(texture.At(level, 0, static_cast<u32>(wrap(x, width)),
                                       static_cast<u32>(mirror(y, height)), 0));
         }
         const Program program = bench.ComputeInteger(true);
         const auto got = bench.Run(program, texture, MakeSampler(config, false),
                                    MakeRow(config, spec), samples);
         Compare(got, want, samples, 0.0f, "emulating");
     }},

    // --- performance ---------------------------------------------------------------------------
    {"performance", "sampling throughput and compile cost",
     [](Bench& bench) {
         auto rng = Rng(40);
         const Texture texture = bench.MakeTexture(
             {.dim = Dim::D2, .width = 256, .height = 256, .depth = 1, .levels = 9}, rng);
         constexpr u32 GROUPS = 4096; // 262144 samples
         constexpr u32 REPEAT = 16;
         std::vector<SampleData> samples(GROUPS * 64);
         std::uniform_real_distribution<float> coord(0.0f, 1.0f);
         std::uniform_real_distribution<float> lod(-1.0f, 6.0f);
         for (SampleData& s : samples) {
             s.coord = {coord(rng), coord(rng), 0.0f, lod(rng)};
         }
         const u64 size = samples.size() * sizeof(SampleData);
         auto data = bench.DataBuffer(size);
         {
             auto upload = bench.UploadBuffer(std::as_bytes(std::span{samples}));
             const auto readback = bench.Run(bench.Compute(Dim::D2, Op::Lod, false), texture,
                                             MakeSampler({}, false),
                                             MakeRow({}, texture.spec),
                                             std::span{samples}.first(64));
             (void)readback;
             (void)upload;
         }
         const Program plain = bench.Compute(Dim::D2, Op::Lod, false);
         const Program emulated = bench.Compute(Dim::D2, Op::Lod, true);
         SamplerConfig none{};
         none.reduction = EDEN_SAMPLER_REDUCTION_NONE;
         SamplerConfig min{};
         const auto row_none = MakeRow(none, texture.spec);
         const auto row_min = MakeRow(min, texture.spec);
         // The data buffer starts in COMMON: promoted to UNORDERED_ACCESS by the first dispatch.
         // GPU clocks change between submissions and inflated whichever variant met a slow one:
         // the variants take turns over several rounds and each keeps its fastest median.
         constexpr u32 ROUNDS = 5;
         const auto inf = std::numeric_limits<double>::infinity();
         double t_plain = inf, t_none = inf, t_min = inf;
         std::optional<double> t_hw;
         for (u32 round = 0; round < ROUNDS; ++round) {
             t_plain = std::min(t_plain, bench.TimeDispatch(plain, texture, MakeSampler(none, false),
                                                            row_none, data.Get(), size, GROUPS,
                                                            REPEAT));
             t_none = std::min(t_none, bench.TimeDispatch(emulated, texture,
                                                          MakeSampler(none, false), row_none,
                                                          data.Get(), size, GROUPS, REPEAT));
             t_min = std::min(t_min, bench.TimeDispatch(emulated, texture, MakeSampler(min, false),
                                                        row_min, data.Get(), size, GROUPS, REPEAT));
             if (bench.HardwareReduction()) {
                 t_hw = std::min(t_hw.value_or(inf),
                                 bench.TimeDispatch(plain, texture, MakeSampler(min, true), row_min,
                                                    data.Get(), size, GROUPS, REPEAT));
             }
         }
         const double samples_m = static_cast<double>(samples.size()) / 1e6;
         std::cout << fmt::format(
             "    {} samples, best of {} medians of {}: plain {:.3f} ms | emulating, NONE row {:.3f} ms "
             "({:.2f}x) | emulating, MIN row {:.3f} ms ({:.2f}x){}\n",
             samples.size(), ROUNDS, REPEAT, t_plain, t_none, t_none / t_plain, t_min, t_min / t_plain,
             t_hw ? fmt::format(" | hardware MIN {:.3f} ms ({:.2f}x)", *t_hw, *t_hw / t_plain)
                  : std::string{});
         std::cout << fmt::format("    {:.1f} / {:.1f} / {:.1f} ns per sample\n",
                                  t_plain * 1e6 / (samples_m * 1e6), t_none * 1e6 / (samples_m * 1e6),
                                  t_min * 1e6 / (samples_m * 1e6));
         std::cout << fmt::format(
             "    compile: plain {:.1f} ms {} B DXIL | emulating {:.1f} ms {} B DXIL ({:.2f}x)\n",
             plain.compile_ms, plain.dxil_size, emulated.compile_ms, emulated.dxil_size,
             static_cast<double>(emulated.dxil_size) / static_cast<double>(plain.dxil_size));
         // Loose bounds: shaders that never meet a MIN/MAX sampler only pay a row load and a
         // uniform branch; emulated samples cost up to 16 fetches.
         CHECK(t_none <= t_plain * 2.0 + 0.05, "NONE row costs {:.2f}x", t_none / t_plain);
         CHECK(t_min <= t_plain * 25.0 + 0.5, "MIN row costs {:.2f}x", t_min / t_plain);
         CHECK(emulated.dxil_size <= plain.dxil_size * 12, "emulating DXIL grew {:.1f}x",
               static_cast<double>(emulated.dxil_size) / static_cast<double>(plain.dxil_size));
     }},

    // --- stability -----------------------------------------------------------------------------
    {"stability", "random configurations against the hardware (or NONE identity)",
     [](Bench& bench) {
         auto rng = Rng(60);
         const bool hardware = bench.HardwareReduction();
         for (u32 iteration = 0; iteration < g_iterations; ++iteration) {
             const Dim dim = static_cast<Dim>(1 + rng() % 4);
             Case c{.texture = SpecFor(dim), .op = static_cast<Op>(rng() % 4), .count = 64};
             if ((rng() & 3) == 0) {
                 c.texture.format = DXGI_FORMAT_R32_FLOAT;
             }
             c.sampler.min_linear = (rng() & 1) != 0;
             c.sampler.mag_linear = (rng() & 1) != 0;
             c.sampler.mip_linear = (rng() & 1) != 0;
             c.sampler.reduction = 1 + rng() % 2;
             c.sampler.wrap = {WRAPS[rng() % 5], WRAPS[rng() % 5], WRAPS[rng() % 5]};
             c.sampler.border = RandomBorder(rng);
             if ((rng() & 3) == 0) {
                 // Whole biases and .25/.75 clamps keep LODS off the .5 nearest-mip ties, which
                 // round either way by implementation.
                 c.sampler.bias = static_cast<float>(static_cast<int>(rng() % 5) - 2);
                 c.sampler.min_lod = std::array{0.0f, 0.75f, 1.25f}[rng() % 3];
                 c.sampler.max_lod = c.sampler.min_lod + static_cast<float>(rng() % 5);
             }
             c.options = {.lo = -1.5f, .hi = 2.5f};
             try {
                 if (hardware) {
                     CheckAgainstHardware(bench, c, rng);
                 } else {
                     CheckNoneIdentical(bench, c, rng);
                 }
                 if (iteration % 25 == 24) {
                     bench.MessagesCheck("iteration");
                 }
             } catch (const Failure& failure) {
                 throw Failure(fmt::format("iteration {}: {}", iteration, failure.what()));
             }
         }
     }},
    {"stability", "repeated dispatches return the same results",
     [](Bench& bench) {
         auto rng = Rng(61);
         const Texture texture = bench.MakeTexture(SpecFor(Dim::D3), rng);
         const auto samples = MakeSamples(texture, Op::GradOffset, {.lo = -1.0f, .hi = 2.0f}, rng,
                                          1024);
         const Program emulated = bench.Compute(Dim::D3, Op::GradOffset, true);
         SamplerConfig config{};
         config.reduction = EDEN_SAMPLER_REDUCTION_MAX;
         config.wrap = {WRAPS[0], WRAPS[3], WRAPS[4]};
         config.border = RandomBorder(rng);
         const auto row = MakeRow(config, texture.spec);
         const auto first = bench.Run(emulated, texture, MakeSampler(config, false), row, samples);
         for (u32 i = 0; i < 50; ++i) {
             const auto again =
                 bench.Run(emulated, texture, MakeSampler(config, false), row, samples);
             Compare(again, first, samples, 0.0f, fmt::format("run {}", i + 1));
         }
     }},
    {"stability", "rebuilding the emulating pipelines",
     [](Bench& bench) {
         // Every variant compiles, validates and creates a PSO again and again.
         for (u32 round = 0; round < 3; ++round) {
             for (const Dim dim : {Dim::D1, Dim::D2, Dim::Array2D, Dim::D3}) {
                 for (const Op op : {Op::Lod, Op::Grad, Op::LodOffset, Op::GradOffset}) {
                     const Program program = bench.Compute(dim, op, true);
                     CHECK(program.pso, "no PSO");
                 }
             }
             for (const bool bias : {false, true}) {
                 CHECK(bench.Fragment(bias, true).pso, "no PSO");
             }
             CHECK(bench.ComputeInteger(true).pso, "no PSO");
             bench.MessagesCheck("round");
         }
     }},
};

} // Anonymous namespace

int main(int argc, char** argv) try {
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seed" && i + 1 < argc) {
            g_seed = static_cast<u32>(std::stoul(argv[++i], nullptr, 0));
        } else if (arg == "--iterations" && i + 1 < argc) {
            g_iterations = static_cast<u32>(std::stoul(argv[++i], nullptr, 0));
        } else if (arg == "--spv" && i + 1 < argc) {
            g_spv_dir = argv[++i];
        } else {
            filter = arg;
        }
    }
    if (g_spv_dir.empty()) {
        std::cerr << "usage: reduction-sampling.exe --spv DIR [category] [--seed N] "
                     "[--iterations N]\n";
        return 2;
    }
    Common::Log::Initialize();
    Settings::values.renderer_debug = true;
    SetAssertHook(CountAssert);
    Bench bench;
    bench.messages->ClearStoredMessages();
    u32 passed = 0, failed = 0, skipped = 0;
    for (const Test& test : TESTS) {
        if (!filter.empty() && filter != test.category) continue;
        bench.messages->ClearStoredMessages();
        const u32 asserts = g_assert_failures;
        const auto start = std::chrono::steady_clock::now();
        std::string error;
        try {
            test.run(bench);
            bench.MessagesCheck("after the test");
            if (g_assert_failures != asserts) {
                error = fmt::format("{} assertion(s) failed in the emulator code (see the log)",
                                    g_assert_failures - asserts);
            }
        } catch (const std::exception& e) {
            error = e.what();
            try {
                bench.messages->ClearStoredMessages();
            } catch (...) {
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start).count();
        if (error.empty()) {
            ++passed;
            std::cout << fmt::format("PASS [{}] {} ({:.0f} ms)\n", test.category, test.name, ms);
        } else if (error.starts_with("SKIP")) {
            ++skipped;
            std::cout << fmt::format("SKIP [{}] {}: {}\n", test.category, test.name, error);
        } else {
            ++failed;
            std::cout << fmt::format("FAIL [{}] {} ({:.0f} ms)\n    {}\n", test.category,
                                     test.name, ms, error);
        }
        std::cout.flush();
        if (bench.Get()->GetDeviceRemovedReason() != S_OK) {
            std::cout << "device removed: stopping\n";
            ++failed;
            break;
        }
    }
    std::cout << fmt::format("{} passed, {} failed, {} skipped (seed {:#x})\n", passed, failed,
                             skipped, g_seed);
    return failed == 0 ? 0 : 1;
} catch (const std::exception& e) {
    std::cerr << "bench setup: " << e.what() << '\n';
    return 2;
}
