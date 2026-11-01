// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// texture_replay and prewarm pacing: the replay key misses on every input that can change what a
// draw binds, a replay never binds what full resolution would not, the pacer rests 3x each build,
// and comparing the key costs a small fraction of resolving the textures again.

// The checks must run in any configuration: an NDEBUG build would otherwise pass vacuously.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_prewarm_pacer.h"
#include "video_core/renderer_d3d12/d3d12_texture_replay.h"

namespace {
using D3D12::LoadingDetector;
using D3D12::PrewarmPacer;
using D3D12::TextureReplayKey;
using VideoCommon::GraphicsBindingStamp;
using VideoCommon::ImageId;
using VideoCommon::ImageViewId;
using VideoCommon::ImageViewInOut;
using VideoCommon::SamplerId;

std::vector<ImageViewInOut> Views(std::initializer_list<u32> ids) {
    std::vector<ImageViewInOut> views;
    for (const u32 id : ids) {
        views.push_back({.index = 0, .blacklist = false, .id = ImageViewId{id}});
    }
    return views;
}

std::vector<SamplerId> Samplers(std::initializer_list<u32> ids) {
    std::vector<SamplerId> samplers;
    for (const u32 id : ids) {
        samplers.push_back(SamplerId{id});
    }
    return samplers;
}

void TestKeyMissesOnEveryInput() {
    int channel = 0;
    const GraphicsBindingStamp stamp{1, 2, 3, 4, &channel};
    const ImageId depth{7};
    const auto views = Views({10, 11, 12});
    const auto samplers = Samplers({20, 21, 22});

    TextureReplayKey key;
    assert(!key.Matches(stamp, depth, true, views, samplers)); // nothing recorded yet
    key.Store(stamp, depth, true, views, samplers);
    assert(key.Matches(stamp, depth, true, views, samplers));

    auto stamp_with = [&](auto&& change) {
        GraphicsBindingStamp other = stamp;
        change(other);
        return other;
    };
    assert(!key.Matches(stamp_with([](auto& s) { ++s.epoch; }), depth, true, views, samplers));
    assert(!key.Matches(stamp_with([](auto& s) { ++s.frame; }), depth, true, views, samplers));
    assert(!key.Matches(stamp_with([](auto& s) { ++s.bindings; }), depth, true, views, samplers));
    assert(!key.Matches(stamp_with([](auto& s) { ++s.tables; }), depth, true, views, samplers));
    int other_channel = 0;
    assert(!key.Matches(stamp_with([&](auto& s) { s.channel = &other_channel; }), depth, true,
                        views, samplers));

    assert(!key.Matches(stamp, ImageId{8}, true, views, samplers));
    assert(!key.Matches(stamp, ImageId{}, true, views, samplers));
    assert(!key.Matches(stamp, depth, false, views, samplers));

    for (size_t i = 0; i < views.size(); ++i) {
        auto changed = views;
        changed[i].id = ImageViewId{99};
        assert(!key.Matches(stamp, depth, true, changed, samplers));
        auto changed_samplers = samplers;
        changed_samplers[i] = SamplerId{99};
        assert(!key.Matches(stamp, depth, true, views, changed_samplers));
    }
    // The table index and blacklist flag are inputs of the resolution, not of what it bound.
    auto same_ids = views;
    same_ids[0].index = 5;
    assert(key.Matches(stamp, depth, true, same_ids, samplers));

    assert(!key.Matches(stamp, depth, true, Views({10, 11}), samplers));
    assert(!key.Matches(stamp, depth, true, Views({10, 11, 12, 13}), samplers));
    assert(!key.Matches(stamp, depth, true, views, Samplers({20, 21})));

    key.valid = false; // check mode found a mismatch
    assert(!key.Matches(stamp, depth, true, views, samplers));
    key.Store(stamp, depth, true, views, samplers);
    assert(key.Matches(stamp, depth, true, views, samplers));

    TextureReplayKey empty;
    empty.Store(stamp, ImageId{}, false, {}, {});
    assert(empty.Matches(stamp, ImageId{}, false, {}, {}));
    assert(!empty.Matches(stamp, ImageId{}, false, views, samplers));
    std::puts("PASS texture replay key misses on stamp, depth, views, samplers and counts");
}

/// A texture cache model: a view's descriptor depends on the view and on the image changes the
/// binding epoch counts (a deleted view's slot reused, an image reinterpreted), never on anything
/// else. Draws follow GraphicsPipeline::Configure: replay on a key match, else resolve in full
/// and record only when no image changed while resolving and no texture aliased the depth target.
struct ModelCache {
    u64 epoch = 1;
    u64 frame = 1;
    u64 Handle(ImageViewId id) const {
        return (epoch << 32) | id.index;
    }
};

void TestReplayNeverBindsStaleDescriptors() {
    std::mt19937 rng{0x5eed};
    ModelCache cache;
    TextureReplayKey key;
    std::vector<u64> recorded;
    const ImageId depth{3};
    u64 draws = 0, replays = 0, stores = 0;
    std::vector<ImageViewInOut> views = Views({1, 2, 3, 4});
    std::vector<SamplerId> samplers = Samplers({1, 2, 3, 4});
    for (int i = 0; i < 200'000; ++i) {
        const u32 event = rng() % 100;
        if (event < 3) {
            ++cache.epoch; // upload, alias or deletion
        } else if (event < 4) {
            ++cache.frame;
        } else if (event < 10) {
            views[rng() % views.size()].id = ImageViewId{rng() % 8};
        } else if (event < 13) {
            samplers[rng() % samplers.size()] = SamplerId{rng() % 8};
        } else if (event < 14) {
            views.push_back({.index = 0, .blacklist = false, .id = ImageViewId{rng() % 8}});
            samplers.push_back(SamplerId{rng() % 8});
        } else if (event < 15 && views.size() > 1) {
            views.pop_back();
            samplers.pop_back();
        }
        const bool aliases_depth = rng() % 50 == 0;
        const bool image_changed_while_resolving = rng() % 40 == 0;

        ++draws;
        const GraphicsBindingStamp stamp{cache.epoch, cache.frame, 0, 0, nullptr};
        std::vector<u64> full;
        for (const ImageViewInOut& view : views) {
            full.push_back(cache.Handle(view.id));
        }
        if (key.Matches(stamp, depth, true, views, samplers)) {
            ++replays;
            assert(recorded == full);
            continue;
        }
        if (image_changed_while_resolving) {
            ++cache.epoch;
        }
        const GraphicsBindingStamp after{cache.epoch, cache.frame, 0, 0, nullptr};
        if (aliases_depth || after != stamp) {
            key.valid = false;
        } else {
            key.Store(stamp, depth, true, views, samplers);
            recorded = full;
            ++stores;
        }
    }
    assert(replays > draws / 4); // the model repeats draws as the long same-target runs do
    std::printf("PASS %llu draws, %llu replays, %llu recorded: every replay bound the full path's "
                "descriptors\n",
                static_cast<unsigned long long>(draws), static_cast<unsigned long long>(replays),
                static_cast<unsigned long long>(stores));
}

void TestPrewarmPacer() {
    using namespace std::chrono_literals;
    const PrewarmPacer::Clock::time_point t0{1s};
    PrewarmPacer pacer;
    pacer.Queued(t0);
    // First seen built 100 ms later: rests 300 ms from then.
    assert(!pacer.Rested(t0 + 100ms));
    assert(!pacer.Rested(t0 + 250ms));
    assert(!pacer.Rested(t0 + 399ms));
    assert(pacer.Rested(t0 + 400ms));
    assert(pacer.Rested(t0 + 900ms));

    // The next PSO: queued at 900 ms, seen built at 920 ms, rests 60 ms.
    pacer.Queued(t0 + 900ms);
    assert(!pacer.Rested(t0 + 920ms));
    assert(!pacer.Rested(t0 + 979ms));
    assert(pacer.Rested(t0 + 980ms));

    // Built by the next frame boundary at once: no rest beyond it.
    pacer.Queued(t0 + 2s);
    assert(pacer.Rested(t0 + 2s));

    // Duty cycle of a worker over back-to-back prewarms: build / (build + rest) = 25%.
    PrewarmPacer duty;
    auto now = t0;
    std::chrono::nanoseconds busy{}, total{};
    for (const auto build : {80ms, 650ms, 200ms, 120ms, 400ms}) {
        const auto start = now;
        duty.Queued(now);
        now += build;
        busy += build;
        while (!duty.Rested(now)) {
            now += 16ms; // one frame
        }
        total += now - start;
    }
    const double share = static_cast<double>(busy.count()) / static_cast<double>(total.count());
    assert(share > 0.20 && share <= 0.25);
    std::printf("PASS prewarm pacer rests 3x each build (worker busy %.1f%% of the time)\n",
                share * 100.0);
}

void TestLoadingDetector() {
    using namespace std::chrono_literals;
    const LoadingDetector::Clock::time_point t0{1s};

    // Gameplay at 30 fps: never a loading screen.
    LoadingDetector gameplay;
    for (int i = 0; i < 100; ++i) {
        assert(!gameplay.Tick(t0 + i * 33ms, 4000));
    }

    // Few draws: a loading screen once they last 500 ms, over as soon as a frame draws a lot.
    LoadingDetector few;
    auto now = t0;
    assert(!few.Tick(now, 30));
    while (now < t0 + 450ms) {
        now += 33ms;
        assert(!few.Tick(now, 30));
    }
    now += 66ms;
    assert(few.Tick(now, 30));
    now += 33ms;
    assert(few.Tick(now, 199));
    now += 33ms;
    assert(!few.Tick(now, 200));
    now += 33ms;
    assert(!few.Tick(now, 30));

    // A gap with little drawn after it is one at once; a long stall then a full frame is not.
    LoadingDetector gap;
    assert(!gap.Tick(t0, 4000));
    assert(gap.Tick(t0 + 300ms, 10));
    LoadingDetector stall;
    assert(!stall.Tick(t0, 4000));
    assert(!stall.Tick(t0 + 900ms, 4000));
    assert(!stall.Tick(t0 + 933ms, 4000));

    // A loading screen does not rest between prewarm builds.
    PrewarmPacer pacer;
    pacer.rest = 0;
    pacer.Queued(t0);
    assert(pacer.Rested(t0 + 200ms));
    std::puts("PASS loading screens detected from draws per composite; prewarm builds back to "
              "back on them");
}

void BenchKeyCompare() {
    int channel = 0;
    const GraphicsBindingStamp stamp{1, 2, 3, 4, &channel};
    for (const u32 count : {4u, 8u, 16u, 32u}) {
        std::vector<ImageViewInOut> views;
        std::vector<SamplerId> samplers;
        for (u32 i = 0; i < count; ++i) {
            views.push_back({.index = i, .blacklist = false, .id = ImageViewId{100 + i}});
            samplers.push_back(SamplerId{200 + i});
        }
        TextureReplayKey key;
        key.Store(stamp, ImageId{1}, true, views, samplers);
        constexpr int ITERATIONS = 2'000'000;
        volatile u32 sink = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < ITERATIONS; ++i) {
            sink = sink + (key.Matches(stamp, ImageId{1}, true, views, samplers) ? 1u : 0u);
        }
        const double ns = std::chrono::duration<double, std::nano>(
                              std::chrono::steady_clock::now() - start)
                              .count() /
                          ITERATIONS;
        assert(sink == ITERATIONS);
        // The console spent ~1.07 us per draw on textures and samplers (run C); a hit must cost
        // a small fraction of it even on a slower core.
        assert(ns < 150.0);
        std::printf("BENCH key compare, %2u textures: %.1f ns per draw (%.1f%% of the 1070 ns "
                    "measured for textures+samplers)\n",
                    count, ns, ns / 10.7);
    }
}
} // namespace

int main() {
    TestKeyMissesOnEveryInput();
    TestReplayNeverBindsStaleDescriptors();
    TestPrewarmPacer();
    TestLoadingDetector();
    BenchKeyCompare();
    std::puts("texture replay gate passed");
    return 0;
}
