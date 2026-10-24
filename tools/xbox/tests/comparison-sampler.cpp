// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Recording shim for comparison-sampler.py; the Sampler implementation is injected unchanged.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

using u32 = uint32_t;
using u64 = uint64_t;
using f32 = float;
using D3D12_FILTER = u32;
using D3D12_FILTER_REDUCTION_TYPE = u32;
constexpr u32 D3D12_FILTER_REDUCTION_TYPE_STANDARD = 0;
constexpr u32 D3D12_FILTER_REDUCTION_TYPE_COMPARISON = 1;
constexpr u32 D3D12_FILTER_REDUCTION_TYPE_MINIMUM = 2;
constexpr u32 D3D12_FILTER_REDUCTION_TYPE_MAXIMUM = 3;
constexpr u32 D3D12_FILTER_TYPE_POINT = 0;
constexpr u32 D3D12_FILTER_TYPE_LINEAR = 1;
constexpr f32 D3D12_MIP_LOD_BIAS_MIN = -16.0f;
constexpr f32 D3D12_MIP_LOD_BIAS_MAX = 15.99f;
constexpr u32 D3D12_ENCODE_BASIC_FILTER(u32 min, u32 mag, u32 mip, u32 reduction) {
    return (min << 4) | (mag << 2) | mip | (reduction << 7);
}
constexpr u32 D3D12_ENCODE_ANISOTROPIC_FILTER(u32 reduction) {
    return 0x55 | (reduction << 7);
}
struct D3D12_CPU_DESCRIPTOR_HANDLE {
    size_t ptr{};
};
struct D3D12_GPU_DESCRIPTOR_HANDLE {
    u64 ptr{};
};
struct D3D12_SAMPLER_DESC {
    u32 Filter{}, AddressU{}, AddressV{}, AddressW{};
    f32 MipLODBias{};
    u32 MaxAnisotropy{}, ComparisonFunc{};
    f32 BorderColor[4]{};
    f32 MinLOD{}, MaxLOD{};
};
struct eden_integer_sampler_state {
    u32 border_color[4]{};
    f32 lod_bias{}, min_lod{}, max_lod{};
    u32 last_level{}, wrap[3]{};
};
namespace fmt {
template <typename... Args>
std::string format(const char*, Args&&...) {
    return {};
}
} // namespace fmt
#define LOG_INFO(...) ((void)0)
namespace Tegra::Texture {
enum class TextureFilter { Point, Linear };
enum class TextureMipmapFilter { None, Point, Linear };
enum class SamplerReduction { WeightedAverage, Min, Max };
struct TSCEntry {
    std::array<u64, 4> raw{};
    TextureFilter min_filter{}, mag_filter{};
    TextureMipmapFilter mipmap_filter{TextureMipmapFilter::Point};
    SamplerReduction reduction_filter{};
    bool depth_compare_enabled{true};
    u32 depth_compare_func{4}, wrap_u{1}, wrap_v{2}, wrap_p{3};
    f32 anisotropy{1}, min_lod{2}, max_lod{1}, bias{99};
    std::array<f32, 4> BorderColor() const {
        return {0.25f, 0.5f, 0.75f, 1};
    }
    f32 MaxAnisotropy() const {
        return anisotropy;
    }
    f32 MinLod() const {
        return min_lod;
    }
    f32 MaxLod() const {
        return max_lod;
    }
    f32 LodBias() const {
        return bias;
    }
};
} // namespace Tegra::Texture
namespace VideoCommon {
using SamplerId = u32;
}
namespace boost::container {
template <typename T, size_t>
using small_vector = std::vector<T>;
}
namespace D3D12 {
struct Allocator {
    size_t next{1};
    std::set<size_t> live;
    D3D12_CPU_DESCRIPTOR_HANDLE Allocate() {
        live.insert(next);
        return {.ptr = next++};
    }
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        assert(live.erase(handle.ptr) == 1);
    }
};
struct Device {
    std::map<size_t, D3D12_SAMPLER_DESC> descriptors;
    Device* Get() {
        return this;
    }
    void CreateSampler(const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        descriptors[handle.ptr] = *desc;
    }
};
struct TextureCacheRuntime {
    Allocator sampler_descriptors;
    Device device;
    bool supports_min_max_filter{true};
};
namespace TextureDetail {
inline std::atomic<u64> next_sampler_key{1};
inline bool logged_point_reduction{}, logged_min_max_filter{};
inline u32 AddressMode(u32 value) {
    return value;
}
inline u32 Compare(u32 value) {
    return value;
}
inline void WarnOnce(bool&, const char*) {}
template <typename Func>
void CheckRemovedAfter(Device*, Func&&) {}
} // namespace TextureDetail
// PRODUCTION_SAMPLER_CLASS
struct TextureCache {
    std::vector<Sampler> samplers;
    const Sampler& GetSampler(u32 id) const {
        return samplers.at(id);
    }
};
struct SamplerHeap {
    std::vector<u64> keys;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> handles;
    D3D12_GPU_DESCRIPTOR_HANDLE GetTable(
        std::span<const u64> input_keys,
        std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> input_handles) {
        keys.assign(input_keys.begin(), input_keys.end());
        handles.assign(input_handles.begin(), input_handles.end());
        return {.ptr = 1};
    }
};
// PRODUCTION_SAMPLER_TABLE
} // namespace D3D12
// PRODUCTION_SAMPLER_SOURCE

// Keep the pre-fix API executable, so the red gate fails on behavior, not compilation.
template <typename T>
auto Handle(const T& sampler, bool compares) {
    if constexpr (requires { sampler.Handle(compares); })
        return sampler.Handle(compares);
    else
        return sampler.Handle();
}
template <typename T>
auto Key(const T& sampler, bool compares) {
    if constexpr (requires { sampler.Key(compares); })
        return sampler.Key(compares);
    else
        return sampler.Key();
}

struct Stage {
    struct Descriptor {
        u32 count{};
        bool is_depth{};
    };
    std::vector<Descriptor> texture_descriptors;
};
template <typename T>
void AddStage(T& table, const Stage& stage, D3D12::TextureCache& cache, const u32*& ids) {
    if constexpr (requires { table.AddStage(stage, cache, ids); })
        table.AddStage(stage, cache, ids);
    else {
        for (const auto& descriptor : stage.texture_descriptors) {
            for (u32 index = 0; index < descriptor.count; ++index)
                table.Add(cache.GetSampler(*ids++));
        }
    }
}

int main() {
    D3D12::TextureCacheRuntime runtime;
    Tegra::Texture::TSCEntry config;
    {
        D3D12::Sampler sampler{runtime, config};
        const auto normal = runtime.device.descriptors.at(Handle(sampler, false).ptr);
        assert(normal.Filter == 0 && "plain ambient cube must not use a comparison sampler");
        assert(sampler.Handle().ptr == Handle(sampler, false).ptr);
        assert(sampler.HandleWithDefaultAnisotropy().ptr == Handle(sampler, false).ptr);
        const auto comparison = runtime.device.descriptors.at(Handle(sampler, true).ptr);
        assert(comparison.Filter == 0x80 && comparison.ComparisonFunc == 4);
        assert(Key(sampler, false) != Key(sampler, true));
        assert(normal.MinLOD == 2 && normal.MaxLOD == 2 && normal.MipLODBias == 15.99f);
        assert(normal.BorderColor[2] == 0.75f && normal.AddressW == 3);
        const auto integer = sampler.IntegerState(7);
        assert(integer.last_level == 7 && integer.border_color[2] == std::bit_cast<u32>(0.75f));
        const auto normal_key = Key(sampler, false), compare_key = Key(sampler, true);
        D3D12::Sampler moved{std::move(sampler)};
        assert(Key(moved, false) == normal_key && Key(moved, true) == compare_key);
        D3D12::Sampler assigned{runtime, config};
        assigned = std::move(moved);
        assert(runtime.sampler_descriptors.live.size() == 2);
        assert(Key(assigned, true) == compare_key);
    }
    // Plain MIN/MAX policy remains exact for point filters, supported for linear filters,
    // and safely falls back on devices without reduction support. Comparison is independent.
    for (bool supported : {false, true}) {
        for (bool linear : {false, true}) {
            for (auto reduction :
                 {Tegra::Texture::SamplerReduction::Min, Tegra::Texture::SamplerReduction::Max}) {
                config = {};
                config.reduction_filter = reduction;
                config.min_filter = linear ? Tegra::Texture::TextureFilter::Linear
                                           : Tegra::Texture::TextureFilter::Point;
                config.mipmap_filter = Tegra::Texture::TextureMipmapFilter::None;
                runtime.supports_min_max_filter = supported;
                D3D12::Sampler sampler{runtime, config};
                const auto plain = runtime.device.descriptors.at(Handle(sampler, false).ptr);
                const auto comparison = runtime.device.descriptors.at(Handle(sampler, true).ptr);
                const u32 expected =
                    supported && linear
                        ? (reduction == Tegra::Texture::SamplerReduction::Min ? 2U : 3U)
                        : 0U;
                assert(((plain.Filter >> 7) & 3) == expected);
                assert(((comparison.Filter >> 7) & 3) == 1);
                assert(plain.MinLOD == 0 && plain.MaxLOD == 0.25f);
                assert(comparison.MinLOD == 0 && comparison.MaxLOD == 0.25f);
            }
        }
    }
    assert(runtime.sampler_descriptors.live.empty());
    for (bool enabled : {false, true}) {
        for (u32 reduction = 0; reduction < 3; ++reduction) {
            for (f32 anisotropy : {1.0f, 8.0f}) {
                config.depth_compare_enabled = enabled;
                config.reduction_filter = static_cast<Tegra::Texture::SamplerReduction>(reduction);
                config.anisotropy = anisotropy;
                D3D12::Sampler sampler{runtime, config};
                const u32 normal_filter =
                    runtime.device.descriptors.at(Handle(sampler, false).ptr).Filter;
                const u32 compare_filter =
                    runtime.device.descriptors.at(Handle(sampler, true).ptr).Filter;
                assert(((normal_filter >> 7) & 3) != 1);
                assert(enabled ? ((compare_filter >> 7) & 3) == 1
                               : compare_filter == normal_filter);
                assert(enabled || Key(sampler, false) == Key(sampler, true));
            }
        }
    }
    {
        config = {};
        D3D12::TextureCache cache;
        cache.samplers.emplace_back(runtime, config);
        const Stage stage{.texture_descriptors = {{.count = 2, .is_depth = false},
                                                  {.count = 1, .is_depth = true},
                                                  {.count = 1, .is_depth = false}}};
        const std::array<u32, 4> ids{};
        const u32* cursor = ids.data();
        D3D12::SamplerTableBuilder table;
        AddStage(table, stage, cache, cursor);
        assert(cursor == ids.data() + 4);
        D3D12::SamplerHeap heap;
        (void)table.Table(heap);
        assert(heap.keys.size() == 4 && heap.keys[0] == heap.keys[1]);
        assert(heap.keys[0] != heap.keys[2] && heap.keys[0] == heap.keys[3]);
        for (size_t index = 0; index < 4; ++index) {
            const u32 filter = runtime.device.descriptors.at(heap.handles[index].ptr).Filter;
            assert(filter == (index == 2 ? 0x80U : 0U));
        }
        // Separate stage/dispatch, opposite table order, and a reset cannot alias modes.
        const Stage reverse{.texture_descriptors = {{.count = 1, .is_depth = true},
                                                    {.count = 1, .is_depth = false}}};
        cursor = ids.data();
        D3D12::SamplerTableBuilder other_table;
        AddStage(other_table, reverse, cache, cursor);
        const auto plain_key = heap.keys[0], comparison_key = heap.keys[2];
        heap = {};
        (void)other_table.Table(heap);
        assert(heap.keys[0] == comparison_key && heap.keys[1] == plain_key);
        // A changed TSC gets fresh identities even if the old CPU slots are recycled.
        cache.samplers.emplace_back(runtime, config);
        assert(Key(cache.samplers[1], false) != plain_key);
        assert(Key(cache.samplers[1], true) != comparison_key);
    }
    assert(runtime.sampler_descriptors.live.empty());
    std::cout << "PASS production sampler: mixed plain/compare, reduction/aniso, LOD, integer, "
                 "moves and frees\n";
}
