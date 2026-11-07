// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cassert>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include "video_core/renderer_d3d12/d3d12_live_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_pso_key.h"

using namespace D3D12;

// Mirrors the original lookup hash, including its scan of every shader byte.
__declspec(noinline) static size_t OldHash(const GraphicsPsoKey& key) {
    std::uint64_t hash = reinterpret_cast<std::uintptr_t>(key.root);
    const auto add = [&](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
    for (auto word : key.state) add(word);
    for (auto shader : key.shaders) {
        add(shader.size());
        for (auto byte : shader) add(byte);
    }
    return static_cast<size_t>(hash);
}

__declspec(noinline) static size_t NewHash(const GraphicsPsoKey& key) {
    return GraphicsPsoKeyHash{}(key);
}

static void Identity() {
    std::array<std::vector<std::uint8_t>, 5> code, copies;
    PsoShaderSpans spans;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::mt19937 random{207208};
    for (size_t i = 0; i < code.size(); ++i) {
        code[i].resize(257 + i * 113);
        for (auto& byte : code[i]) byte = static_cast<std::uint8_t>(random());
        spans[i] = code[i];
    }
    const PsoShaderHashCache cached{spans};
    const auto set = [&](size_t stage, std::span<const std::uint8_t> bytes) {
        D3D12_SHADER_BYTECODE value{bytes.data(), bytes.size()};
        switch (stage) {
        case 0: desc.VS = value; break;
        case 1: desc.HS = value; break;
        case 2: desc.DS = value; break;
        case 3: desc.GS = value; break;
        case 4: desc.PS = value; break;
        }
    };
    size_t cases{};
    // Stage omission includes transform feedback's disabled PS. Relocated, truncated and
    // replaced stages must fall back to their own bytes, including an empty cache source.
    for (unsigned mask = 0; mask < 32; ++mask) {
        for (size_t i = 0; i < code.size(); ++i) set(i, mask & (1U << i) ? spans[i] : std::span<const std::uint8_t>{});
        for (unsigned state = 0; state < 128; ++state) {
            desc.SampleMask = random();
            desc.RasterizerState.DepthBias = static_cast<INT>(random());
            auto a = MakeGraphicsPsoKey(desc, {}, &cached);
            auto b = MakeGraphicsPsoKey(desc, {});
            a.state.push_back(state & 1U); b.state.push_back(state & 1U);
            assert(a == b && NewHash(a) == NewHash(b));
            ++cases;
        }
    }
    for (size_t stage = 0; stage < code.size(); ++stage) {
        for (size_t i = 0; i < code.size(); ++i) set(i, spans[i]);
        const auto original = MakeGraphicsPsoKey(desc, {}, &cached);
        copies[stage] = code[stage];
        set(stage, copies[stage]);
        auto relocated = MakeGraphicsPsoKey(desc, {}, &cached);
        assert(original == relocated && NewHash(original) == NewHash(relocated));
        copies[stage].back() ^= 1;
        auto changed = MakeGraphicsPsoKey(desc, {}, &cached);
        assert(!(original == changed));
        // Equal hashes must never authorize sharing different bytes.
        changed.shader_hashes = original.shader_hashes;
        assert(NewHash(original) == NewHash(changed) && !(original == changed));
        struct Collision { size_t operator()(const GraphicsPsoKey&) const { return 0; } };
        LivePipelineCache<GraphicsPsoKey, int, Collision> cache;
        auto first = cache.Get(original, [] { return 11; }).entry;
        auto second = cache.Get(changed, [] { return 22; }).entry;
        assert(first != second && *first->value == 11 && *second->value == 22);
        set(stage, std::span<const std::uint8_t>{copies[stage]}.first(13));
        auto truncated = MakeGraphicsPsoKey(desc, {}, &cached);
        auto plain = MakeGraphicsPsoKey(desc, {});
        assert(truncated == plain && NewHash(truncated) == NewHash(plain));
        assert(!(truncated == original));
        cases += 5;
    }
    auto empty = GraphicsPsoKey{};
    const auto empty_desc = MakeGraphicsPsoKey({}, {});
    empty.state = empty_desc.state;
    assert(empty == empty_desc && NewHash(empty) == NewHash(empty_desc));
    // Cache publication is immutable: readers require no locks and cannot change the digest.
    for (size_t i = 0; i < code.size(); ++i) set(i, spans[i]);
    const auto expected = NewHash(MakeGraphicsPsoKey(desc, {}, &cached));
    std::vector<std::jthread> readers;
    for (int i = 0; i < 16; ++i) readers.emplace_back([&] {
        for (int n = 0; n < 1000; ++n) assert(NewHash(MakeGraphicsPsoKey(desc, {}, &cached)) == expected);
    });
    readers.clear();
    std::cout << "Identity: " << cases << " omission/state/relocation/replacement/collision cases; 16000 concurrent reads PASS\n";
}

static void Benchmark() {
    std::vector<std::uint8_t> shader(512 * 1024);
    std::mt19937 random{208};
    for (auto& byte : shader) byte = static_cast<std::uint8_t>(random());
    PsoShaderSpans spans{}; spans[0] = shader;
    const auto begin = std::chrono::steady_clock::now();
    const PsoShaderHashCache cached{spans};
    const auto prepared = std::chrono::steady_clock::now();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.VS = {shader.data(), shader.size()};
    auto key = MakeGraphicsPsoKey(desc, {}, &cached);
    volatile size_t sink{};
    const auto bench = [&](auto hash) {
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 512; ++i) {
            key.state.back() = i;
            sink = hash(key);
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    const auto old_ms = bench(OldHash), new_ms = bench(NewHash);
    const double prepare_ms = std::chrono::duration<double, std::milli>(prepared - begin).count();
    std::cout << "Synthetic lookup only: 512 variants / 512 KiB shader, old=" << old_ms
              << " ms new=" << new_ms << " ms prepare=" << prepare_ms
              << " ms, byte scans 256 MiB -> 0 after preparation (not Xbox FPS), sink=" << sink << '\n';
}

int main() { Identity(); Benchmark(); }
