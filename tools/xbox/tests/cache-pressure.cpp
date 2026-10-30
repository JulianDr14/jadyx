// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// The checks must run in any configuration: an NDEBUG build would otherwise pass vacuously.
#undef NDEBUG
#include <cassert>
#include <limits>
#include <iostream>
#include "video_core/renderer_d3d12/d3d12_cache_policy.h"
using namespace D3D12;
constexpr u64 MiB = 1024*1024;
int main() {
    CachePressureController c;
    auto sample = [](u64 free) { return CacheMemorySnapshot{5120*MiB-free,5120*MiB,530*MiB,3447*MiB}; };
    // A large CPU/JIT allocation is not GPU usage: 800 MiB actual headroom is normal.
    assert(c.Update(sample(800*MiB)) == CachePressure::Normal);
    assert(c.Update(sample(768*MiB)) == CachePressure::Pressure);
    assert(c.Update(sample(850*MiB)) == CachePressure::Pressure);
    assert(c.Update(sample(896*MiB)) == CachePressure::Normal);
    assert(c.Update(sample(384*MiB)) == CachePressure::Critical);
    assert(c.Update(sample(480*MiB)) == CachePressure::Critical);
    assert(c.Update(sample(512*MiB)) == CachePressure::Pressure);
    assert(c.Update(sample(192*MiB)) == CachePressure::Emergency);
    assert(c.Update(sample(240*MiB)) == CachePressure::Emergency);
    assert(c.Update(sample(256*MiB)) == CachePressure::Critical);
    auto exhausted = sample(0); exhausted.app_used += MiB;
    assert(exhausted.AppFree() == 0);
    assert(c.Update(exhausted) == CachePressure::Emergency);
    assert(c.Update({}) == CachePressure::Emergency); // failed queries do not relax pressure
    CachePressureController gpu;
    assert(gpu.Update({0,0,970*MiB,1000*MiB}) == CachePressure::Emergency);
    assert(gpu.Update({0,0,950*MiB,1000*MiB}) == CachePressure::Emergency);
    assert(gpu.Update({0,0,920*MiB,1000*MiB}) == CachePressure::Critical);
    assert(gpu.Update({0,0,850*MiB,1000*MiB}) == CachePressure::Pressure);
    assert(gpu.Update({0,0,750*MiB,1000*MiB}) == CachePressure::Normal);
    // OS budget shrink must trigger even when the app has ample CPU RAM.
    assert(gpu.Update({1000*MiB,5120*MiB,530*MiB,540*MiB}) == CachePressure::Emergency);
    CachePressureController large;
    const auto maximum = std::numeric_limits<u64>::max();
    assert(large.Update({0,0,maximum,maximum}) == CachePressure::Emergency);
    auto critical = c.Policy(CachePressure::Critical,true);
    auto emergency = c.Policy(CachePressure::Emergency,true);
    assert(critical.min_age == 60 && critical.max_downloads == 1 && critical.time_budget_us == 1000);
    assert(emergency.min_age == 10 && emergency.max_downloads == 1 && emergency.time_budget_us == 1000);
    const auto recovery = c.Policy(CachePressure::Emergency, true, true);
    assert(recovery.max_downloads == 40 && recovery.time_budget_us == 0);
    assert(!c.RequiresImmediateRecovery(sample(64*MiB)));
    assert(c.RequiresImmediateRecovery(sample(64*MiB-1)));
    assert(!c.RequiresImmediateRecovery({0, 0, 999*MiB, 1000*MiB}));
    assert(c.RequiresImmediateRecovery({0, 0, 1000*MiB, 1000*MiB}));
    assert(!c.RequiresImmediateRecovery({}));
    assert(!c.Policy(CachePressure::Critical,false).aggressive);
    assert(c.Policy(CachePressure::Normal,false).min_age >= critical.min_age);
    // A coordinator yield evicts clean idle textures sooner, never downloads a render target.
    const auto yield = CachePressureController::YieldPolicy();
    assert(yield.high_priority && !yield.aggressive && yield.max_downloads == 0);
    assert(yield.min_age < c.Policy(CachePressure::Pressure,false).min_age);
    assert(yield.iterations > c.Policy(CachePressure::Pressure,false).iterations);
    // Texture cap: the heaps may hold the app headroom above the line, never less than the floor.
    constexpr u64 floor = 768*MiB, line = 2192*MiB;
    const auto room = [](u64 used) { return CacheMemorySnapshot{used*MiB,5120*MiB,0,0}; };
    assert(TextureCapForHeadroom(floor, line, 500*MiB, room(2000)) == 1428*MiB); // light game
    assert(TextureCapForHeadroom(floor, line, 1428*MiB, room(2928)) == 1428*MiB); // grown into it
    assert(TextureCapForHeadroom(floor, line, 300*MiB, room(4000)) == floor); // heavy game
    assert(TextureCapForHeadroom(floor, line, 900*MiB, room(5120)) == floor); // exhausted
    assert(TextureCapForHeadroom(floor, line, 900*MiB, room(5200)) == floor); // over the limit
    assert(TextureCapForHeadroom(floor, line, 900*MiB, {}) == floor); // unknown limit
    assert(TextureCapForHeadroom(floor, 0, 0, room(0)) == floor); // growth off
    assert(TextureCapForHeadroom(0, line, 0, room(0)) == 0); // cap off
    // The GPU budget bounds it too, at the Normal exit: a quarter of the budget left.
    assert(TextureCapForHeadroom(floor, line, 1000*MiB, {0,5120*MiB,1000*MiB,4000*MiB}) ==
           3000*MiB);
    assert(TextureCapForHeadroom(floor, line, 1000*MiB, {0,5120*MiB,3500*MiB,4000*MiB}) == floor);
    std::cout << "PASS: independent domains, app/GPU budget exhaustion, hysteresis, query failure, overflow, emergency recovery policy and headroom texture cap\n";
}
