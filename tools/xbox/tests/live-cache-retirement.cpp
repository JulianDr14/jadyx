// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#undef NDEBUG
#include <array>
#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_live_pipeline_cache.h"

using namespace D3D12;
struct Key {
    size_t id{};
    size_t version{};
    bool operator==(const Key&) const = default;
};
struct Hash {
    size_t operator()(const Key& key) const noexcept { return key.id; }
};
struct Collision {
    size_t operator()(const Key&) const noexcept { return 0; }
};
using Cache = LivePipelineCache<Key, int, Hash>;

static void CapacityAndLifetime() {
    Cache cache{2};
    auto a = cache.Get({1, 0}, [] { return 10; }).entry;
    auto b = cache.Get({2, 0}, [] { return 20; }).entry;
    auto hit = cache.Get({1, 0}, [] { assert(false); return 0; });
    assert(hit.outcome == Cache::Outcome::Hit && hit.entry == a);
    auto bypass = cache.Get({3, 0}, [] { return 30; });
    assert(bypass.outcome == Cache::Outcome::Bypassed && *bypass.entry->value == 30);
    b.reset();
    // Reclaim immediately, without waiting for the 128-request periodic cleanup.
    auto replacement = cache.Get({4, 0}, [] { return 40; });
    assert(replacement.outcome == Cache::Outcome::Created);
    assert(cache.Get({1, 0}, [] { assert(false); return 0; }).entry == a);
    std::weak_ptr<const Cache::Entry> released = a;
    hit.entry.reset(); a.reset();
    assert(released.expired());
    Cache disabled{0};
    auto x = disabled.Get({1, 0}, [] { return 1; });
    auto y = disabled.Get({1, 0}, [] { return 2; });
    assert(x.outcome == Cache::Outcome::Bypassed && y.outcome == Cache::Outcome::Bypassed);
    assert(x.entry != y.entry);
    Cache failure{2};
    for (int i = 0; i < 3; ++i) {
        try {
            failure.Get({1, 0}, []() -> int { throw std::runtime_error("synthetic build failure"); });
            assert(false);
        } catch (const std::runtime_error&) {
        }
    }
    assert(failure.Get({1, 0}, [] { return 99; }).outcome == Cache::Outcome::Created);
    LivePipelineCache<Key, int, Collision> collisions{2};
    auto first = collisions.Get({1, 0}, [] { return 1; }).entry;
    auto changed = collisions.Get({1, 1}, [] { return 2; }).entry;
    assert(first != changed);
    assert(collisions.Get({1, 0}, [] { assert(false); return 0; }).entry == first);
}

static void ConcurrentFullHits() {
    Cache cache{64};
    std::array<std::shared_ptr<const Cache::Entry>, 64> kept;
    for (size_t i = 0; i < kept.size(); ++i) {
        kept[i] = cache.Get({i, 0}, [i] { return static_cast<int>(i); }).entry;
    }
    std::barrier start{17};
    std::vector<std::jthread> threads;
    for (size_t t = 0; t < 16; ++t) {
        threads.emplace_back([&, t] {
            start.arrive_and_wait();
            for (size_t n = 0; n < 512; ++n) {
                const size_t i = (t + n) % kept.size();
                const auto hit = cache.Get({i, 0}, [] { assert(false); return 0; });
                assert(hit.outcome == Cache::Outcome::Hit && hit.entry == kept[i]);
            }
        });
    }
    start.arrive_and_wait();
    threads.clear();
    assert(cache.Get({65, 0}, [] { return 65; }).outcome == Cache::Outcome::Bypassed);
}

void ConcurrentRetirement() {
    for (size_t repeat = 0; repeat < 12; ++repeat) {
        Cache cache{128};
        std::array<std::shared_ptr<const Cache::Entry>, 128> owners;
        for (size_t i = 0; i < owners.size(); ++i)
            owners[i] = cache.Get({i, 0}, [] { return 1; }).entry;
        auto survivor = owners[127];
        std::barrier start{3};
        std::jthread retire{[&] {
            start.arrive_and_wait();
            for (size_t i = 0; i < 127; ++i) {
                owners[i].reset();
                std::this_thread::yield();
            }
        }};
        std::jthread hits{[&] {
            start.arrive_and_wait();
            for (size_t i = 0; i < 4096; ++i)
                assert(cache.Get({127, 0}, [] { assert(false); return 0; }).entry == survivor);
        }};
        start.arrive_and_wait();
        retire.join(); hits.join();
        for (int i = 0; i < 128; ++i) cache.Get({127, 0}, [] { assert(false); return 0; });
        std::vector<std::shared_ptr<const Cache::Entry>> replacements;
        for (size_t i = 0; i < 127; ++i) {
            auto result = cache.Get({i + 128, 0}, [] { return 2; });
            assert(result.outcome == Cache::Outcome::Created);
            replacements.push_back(result.entry);
        }
    }
}

int main() {
    CapacityAndLifetime();
    ConcurrentFullHits();
    ConcurrentRetirement();
    std::shared_ptr<const Cache::Entry> survivor;
    { Cache short_lived{1}; survivor = short_lived.Get({1, 0}, [] { return 1; }).entry; }
    assert(*survivor->value == 1);
    survivor.reset(); // token outlives cache safely

    std::cout << "PASS: exact full hits, concurrent hits, immediate reclaim, bypass, "
                 "collision identity, failure retry and weak lifetime\n";
}
