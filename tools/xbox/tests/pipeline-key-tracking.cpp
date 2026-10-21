// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Regression gate for the D3D12 pipeline key reuse: no register the dirty table ignores may
// change the key the real Refresh and RefreshStatic build, the table must cover every other
// register, and Reuse must only allow a draw whose inputs match the last resolved lookup.
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <vector>
#include "video_core/dirty_flags.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_key_tracking.h"

namespace {

using namespace D3D12::PipelineKeyTracking;
using Key = D3D12::GraphicsPipelineCacheKey;
using Maxwell = Maxwell3D::Regs;

int failures = 0;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: " #cond " ", __FILE__, __LINE__);                             \
            std::printf(__VA_ARGS__);                                                              \
            std::printf("\n");                                                                     \
            if (++failures > 50) std::exit(1);                                                     \
        }                                                                                          \
    } while (0)

constexpr u32 Reg(size_t index) {
    return static_cast<u32>(index);
}

std::vector<bool> IgnoredMask() {
    std::vector<bool> ignored(Maxwell::NUM_REGS);
    for (const RegisterRange& range : IgnoredRegisters()) {
        for (u32 i = 0; i < range.count; ++i) {
            ignored[range.first + i] = true;
        }
    }
    return ignored;
}

// The key exactly as LookupGraphicsPipeline builds it, with every dirty block reread.
Key BuildKey(Maxwell3D& maxwell3d) {
    Key key;
    std::memset(&key, 0, sizeof(key));
    Vulkan::DynamicFeatures features{};
    maxwell3d.dirty.flags.set();
    key.state.Refresh(maxwell3d, features);
    key.RefreshStatic(maxwell3d.regs);
    return key;
}

void Randomize(Maxwell3D& maxwell3d, std::mt19937_64& rng) {
    for (u32& word : maxwell3d.regs.reg_array) {
        // Mostly small values so enables, formats and enums land on real cases.
        switch (rng() % 4) {
        case 0: word = 0; break;
        case 1: word = 1; break;
        case 2: word = static_cast<u32>(rng() % 16); break;
        default: word = static_cast<u32>(rng()); break;
        }
    }
    maxwell3d.draw_manager.draw_state.topology =
        static_cast<Maxwell::PrimitiveTopology>(rng() % 15);
    maxwell3d.engine_state = static_cast<Maxwell3D::EngineHint>(rng() % 2);
}

void Mutate(Maxwell3D& maxwell3d, const RegisterRange& range, std::mt19937_64& rng) {
    for (u32 i = 0; i < range.count; ++i) {
        u32& word = maxwell3d.regs.reg_array[range.first + i];
        const u32 old = word;
        do {
            word = (rng() & 1) ? static_cast<u32>(rng()) : ~old;
        } while (word == old);
    }
}

void TestTableCoverage() {
    Maxwell3D::DirtyState::Table table{};
    SetupTable(table);
    const std::vector<bool> ignored = IgnoredMask();
    for (const RegisterRange& range : IgnoredRegisters()) {
        CHECK(range.count > 0 && range.first + range.count <= Maxwell::NUM_REGS, "range %u+%u",
              range.first, range.count);
    }
    u32 ignored_count = 0;
    for (u32 reg = 0; reg < Maxwell::NUM_REGS; ++reg) {
        const u8 expected = ignored[reg] ? VideoCommon::Dirty::NullEntry : KeyRegisters;
        CHECK(table[reg] == expected, "register 0x%X maps to %u", reg, table[reg]);
        ignored_count += ignored[reg] ? 1 : 0;
    }
    CHECK(KeyRegisters != VideoCommon::Dirty::NullEntry, "key flag aliases NullEntry");

    // Registers the key reads directly must never be ignored, even next to ignored neighbours.
    const u32 must_track[] = {
        Reg(MAXWELL3D_REG_INDEX(index_buffer.format)),
        Reg(MAXWELL3D_REG_INDEX(vertex_streams)),     // stride + enable
        Reg(MAXWELL3D_REG_INDEX(vertex_streams) + 3), // frequency
        Reg(MAXWELL3D_REG_INDEX(vertex_streams) + (Maxwell::NumVertexArrays - 1) * 4),
        Reg(MAXWELL3D_REG_INDEX(vertex_streams) + (Maxwell::NumVertexArrays - 1) * 4 + 3),
        Reg(MAXWELL3D_REG_INDEX(vertex_attrib_format)),
        Reg(MAXWELL3D_REG_INDEX(vertex_stream_instances)),
        Reg(MAXWELL3D_REG_INDEX(pipelines)),
        Reg(MAXWELL3D_REG_INDEX(depth_bias)),
        Reg(MAXWELL3D_REG_INDEX(stencil_front_mask)),
        Reg(MAXWELL3D_REG_INDEX(stencil_front_func_mask)),
        Reg(MAXWELL3D_REG_INDEX(primitive_restart)),
        Reg(MAXWELL3D_REG_INDEX(rt)),
        Reg(MAXWELL3D_REG_INDEX(blend)),
        Reg(MAXWELL3D_REG_INDEX(zeta)),
        Reg(MAXWELL3D_REG_INDEX(transform_feedback_enabled)),
    };
    for (const u32 reg : must_track) {
        CHECK(!ignored[reg], "key register 0x%X is ignored", reg);
    }
    static_assert(sizeof(Maxwell::VertexStream) == 4 * sizeof(u32));
    static_assert(offsetof(Maxwell::VertexStream, address_high) == sizeof(u32));
    static_assert(offsetof(Maxwell::VertexStream, address_low) == 2 * sizeof(u32));
    std::printf("table: %u of %zu registers ignored in %zu ranges\n", ignored_count,
                Maxwell::NUM_REGS, IgnoredRegisters().size());
}

// A write to an ignored register raises nothing in the key table; any other changed write does.
void TestDirtyPropagation(Maxwell3D& maxwell3d) {
    SetupTable(maxwell3d.dirty.tables[TABLE_INDEX]);
    const std::vector<bool> ignored = IgnoredMask();
    for (u32 reg = 0; reg < Maxwell::NUM_REGS; ++reg) {
        maxwell3d.dirty.flags.reset();
        // Mirrors Maxwell3D::ProcessDirtyRegisters for a changed value.
        for (const auto& table : maxwell3d.dirty.tables) {
            maxwell3d.dirty.flags[table[reg]] = true;
        }
        CHECK(maxwell3d.dirty.flags[KeyRegisters] == !ignored[reg], "register 0x%X", reg);
    }
}

void TestIgnoredRegistersKeepKey(Maxwell3D& maxwell3d) {
    constexpr int SAMPLES = 3000;
    std::mt19937_64 rng{0x4A616479785045ull};
    const auto ranges = IgnoredRegisters();
    std::vector<u32> broken(ranges.size());
    u64 compared = 0;
    for (int sample = 0; sample < SAMPLES; ++sample) {
        Randomize(maxwell3d, rng);
        const Maxwell base = maxwell3d.regs;
        const Key expected = BuildKey(maxwell3d);
        CHECK(BuildKey(maxwell3d) == expected, "key not deterministic");
        for (size_t r = 0; r < ranges.size(); ++r) {
            Mutate(maxwell3d, ranges[r], rng);
            if (BuildKey(maxwell3d) != expected) {
                ++broken[r];
            }
            ++compared;
            maxwell3d.regs = base;
        }
        // Every ignored register changed at once, as a burst of draws and uploads would.
        for (const RegisterRange& range : ranges) {
            Mutate(maxwell3d, range, rng);
        }
        CHECK(BuildKey(maxwell3d) == expected, "all ignored registers together, sample %d",
              sample);
        maxwell3d.regs = base;
    }
    for (size_t r = 0; r < ranges.size(); ++r) {
        CHECK(broken[r] == 0, "ignored range 0x%X+%u changed the key in %u of %d samples",
              ranges[r].first, ranges[r].count, broken[r], SAMPLES);
    }
    std::printf("ignored: %llu mutated keys compared\n", static_cast<unsigned long long>(compared));
}

// Sensitivity canary: the same harness must see a change in registers the key does read, or a
// passing TestIgnoredRegistersKeepKey would prove nothing.
void TestHarnessDetectsKeyRegisters(Maxwell3D& maxwell3d) {
    std::mt19937_64 rng{0x6B6579ull};
    const RegisterRange canaries[] = {
        {Reg(MAXWELL3D_REG_INDEX(index_buffer.format)), 1},
        {Reg(MAXWELL3D_REG_INDEX(depth_bias)), 1},
        {Reg(MAXWELL3D_REG_INDEX(vertex_streams)), 1},
        // MSVC's offsetof drops the array part of rt[0].format, so offset the member by hand.
        {Reg(MAXWELL3D_REG_INDEX(rt) + offsetof(Maxwell::RenderTargetConfig, format) / 4), 1},
        {Reg(MAXWELL3D_REG_INDEX(stencil_front_mask)), 1},
    };
    for (const RegisterRange& canary : canaries) {
        u32 changed = 0;
        for (int sample = 0; sample < 500; ++sample) {
            Randomize(maxwell3d, rng);
            const Key expected = BuildKey(maxwell3d);
            Mutate(maxwell3d, canary, rng);
            changed += BuildKey(maxwell3d) != expected ? 1 : 0;
        }
        CHECK(changed > 0, "canary register 0x%X never changed the key", canary.first);
    }
    // The draw state and macro hint are read outside the registers; Inputs must carry them.
    Randomize(maxwell3d, rng);
    maxwell3d.draw_manager.draw_state.topology = Maxwell::PrimitiveTopology::Points;
    maxwell3d.regs.polygon_offset_point_enable = 1;
    maxwell3d.regs.polygon_offset_fill_enable = 0;
    const Key points = BuildKey(maxwell3d);
    maxwell3d.draw_manager.draw_state.topology = Maxwell::PrimitiveTopology::Triangles;
    CHECK(BuildKey(maxwell3d) != points, "topology does not reach the key");
    maxwell3d.engine_state = Maxwell3D::EngineHint::None;
    const Key none = BuildKey(maxwell3d);
    maxwell3d.engine_state = Maxwell3D::EngineHint::OnHLEMacro;
    CHECK(BuildKey(maxwell3d) != none, "engine hint does not reach the key");
}

void TestReuse(const Maxwell3D& engine, const Maxwell3D& other_engine) {
    Inputs inputs{};
    inputs.engine = &engine;
    inputs.topology = Maxwell::PrimitiveTopology::Triangles;
    inputs.unique_hashes = {1, 2, 3, 4, 5, 6};

    Reuse reuse;
    CHECK(!reuse.Allows(inputs, false), "fresh state allows reuse");
    reuse.Remember(inputs);
    CHECK(reuse.Allows(inputs, false), "same inputs refused");
    CHECK(!reuse.Allows(inputs, true), "changed key registers allowed");

    Inputs changed = inputs;
    changed.engine = &other_engine;
    CHECK(!reuse.Allows(changed, false), "other channel's engine allowed");
    changed = inputs;
    changed.topology = Maxwell::PrimitiveTopology::Lines;
    CHECK(!reuse.Allows(changed, false), "other topology allowed");
    changed = inputs;
    changed.engine_state = Maxwell3D::EngineHint::OnHLEMacro;
    CHECK(!reuse.Allows(changed, false), "other engine hint allowed");
    for (size_t stage = 0; stage < inputs.unique_hashes.size(); ++stage) {
        changed = inputs;
        changed.unique_hashes[stage] ^= 1;
        CHECK(!reuse.Allows(changed, false), "other shader at stage %zu allowed", stage);
    }

    reuse.Forget();
    CHECK(!reuse.Allows(inputs, false), "forgotten lookup allowed");
    reuse.Remember(changed);
    CHECK(!reuse.Allows(inputs, false), "stale inputs allowed after a new lookup");
    CHECK(reuse.Allows(changed, false), "latest lookup refused");
}

void TestMode() {
    CHECK(GetMode() == Mode::Off, "default mode is not Off");
    SetMode(Mode::Check);
    CHECK(GetMode() == Mode::Check, "check mode not stored");
    SetMode(Mode::Off);
    CHECK(GetMode() == Mode::Off, "off mode not stored");
    SetMode(Mode::On);
    CHECK(GetMode() == Mode::On, "on mode not stored");
    SetMode(Mode::Off);
}

} // Anonymous namespace

int main() {
    // The engine only keeps the memory manager reference; key refreshes never touch it.
    alignas(std::max_align_t) static std::byte memory_storage[4096]{};
    auto& memory_manager = *reinterpret_cast<Tegra::MemoryManager*>(memory_storage);
    const auto engine = std::make_unique<Maxwell3D>(memory_manager);
    const auto other_engine = std::make_unique<Maxwell3D>(memory_manager);

    TestTableCoverage();
    TestDirtyPropagation(*engine);
    TestIgnoredRegistersKeepKey(*engine);
    TestHarnessDetectsKeyRegisters(*engine);
    TestReuse(*engine, *other_engine);
    TestMode();

    if (failures != 0) {
        std::printf("pipeline key tracking: %d failures\n", failures);
        return 1;
    }
    std::printf("pipeline key tracking: ok\n");
    return 0;
}
