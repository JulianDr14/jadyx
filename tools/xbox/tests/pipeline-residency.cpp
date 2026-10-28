// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Pipeline residency gate: idle limits and the background prewarm against the app's headroom, the
// hot pipeline set's sessions, decay, prewarm choice and file round trip, and the stage latency
// histogram's buckets and percentiles. Run with
// build-uwp/diagnostics/pipeline-residency/run.bat.

// The checks must run in any configuration: an NDEBUG build would otherwise pass vacuously.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_pipeline_residency.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_stats.h"

namespace {
using namespace D3D12;
constexpr std::uint64_t M = MemoryGuard::MiB;
constexpr std::uint64_t limit = 5120 * M;

void TestIdleFrames() {
    using P = PipelineResidencyPolicy;
    assert(P::IdleFrames(0, 0) == P::IDLE_FRAMES);
    // Plenty free: an idle PSO is cheaper to keep than to compile again.
    assert(P::IdleFrames(0, limit) == P::KEEP_IDLE_FRAMES);
    assert(P::IdleFrames(limit - 1537 * M, limit) == P::KEEP_IDLE_FRAMES);
    assert(P::IdleFrames(limit - 1536 * M, limit) == P::IDLE_FRAMES);
    assert(P::IdleFrames(limit - 1024 * M, limit) == P::IDLE_FRAMES);
    assert(P::IdleFrames(limit - 129 * M, limit) == P::IDLE_FRAMES);
    assert(P::IdleFrames(limit - 128 * M, limit) == P::LOW_IDLE_FRAMES);
    assert(P::IdleFrames(limit - 33 * M, limit) == P::LOW_IDLE_FRAMES);
    assert(P::IdleFrames(limit - 32 * M, limit) == P::EMERGENCY_IDLE_FRAMES);
    assert(P::IdleFrames(limit + M, limit) == P::EMERGENCY_IDLE_FRAMES);
}

void TestPrewarmGate() {
    using P = PipelineResidencyPolicy;
    // Prewarm stops exactly where idle eviction starts, so neither undoes the other.
    assert(P::MayPrewarm(0, limit));
    assert(P::MayPrewarm(limit - 1537 * M, limit));
    assert(!P::MayPrewarm(limit - 1536 * M, limit));
    assert(!P::MayPrewarm(limit, limit) && !P::MayPrewarm(limit + M, limit));
    for (std::uint64_t used = 0; used <= limit; used += 16 * M) {
        assert(P::MayPrewarm(used, limit) == (P::IdleFrames(used, limit) == P::KEEP_IDLE_FRAMES));
    }
    // An unknown limit is no promise of room.
    assert(!P::MayPrewarm(0, 0));
}

void TestReleaseProbe() {
    using Probe = PipelineReleaseProbe;
    using V = Probe::Verdict;
    constexpr std::uint64_t K = 1024;

    // The Series: 256 PSOs released, the untracked commit unchanged a few frames later.
    Probe series;
    std::uint64_t frame = 1;
    series.Tick(frame++, 0, 900 * M, true); // baseline, nothing released yet
    assert(series.MayRelease());
    for (std::uint64_t released = 32; released < Probe::PROBE_PSOS; released += 32) {
        series.Tick(frame++, released, 900 * M, true);
        assert(series.MayRelease() && series.GetVerdict() == V::Unknown);
    }
    series.Tick(frame++, Probe::PROBE_PSOS, 900 * M, true);
    // Paused while the released PSOs' fences retire.
    assert(!series.MayRelease() && series.GetVerdict() == V::Unknown);
    const std::uint64_t settle = frame - 1;
    for (; frame - settle < Probe::SETTLE_FRAMES; ++frame) {
        series.Tick(frame, Probe::PROBE_PSOS, 901 * M, true);
        assert(!series.MayRelease());
    }
    series.Tick(frame, Probe::PROBE_PSOS, 901 * M, true);
    assert(series.GetVerdict() == V::DoesNotPay && !series.MayRelease());
    assert(series.Freed() == 0 && series.Probed() == Probe::PROBE_PSOS);
    // Decided once: later frames change nothing.
    series.Tick(frame + 1000, 5000, 100 * M, true);
    assert(series.GetVerdict() == V::DoesNotPay);

    // 16 KiB a PSO pays, a byte less does not.
    const auto run = [](std::uint64_t freed) {
        Probe probe;
        probe.Tick(1, 10, 900 * M, true);
        probe.Tick(2, 10 + Probe::PROBE_PSOS, 900 * M, true);
        probe.Tick(2 + Probe::SETTLE_FRAMES, 10 + Probe::PROBE_PSOS, 900 * M - freed, true);
        return probe.GetVerdict();
    };
    assert(run(Probe::PROBE_PSOS * Probe::MIN_RELEASE_BYTES) == V::Pays);
    assert(run(Probe::PROBE_PSOS * Probe::MIN_RELEASE_BYTES - 1) == V::DoesNotPay);
    assert(run(Probe::PROBE_PSOS * 100 * K) == V::Pays);
    Probe pays;
    pays.Tick(1, 0, 900 * M, true);
    pays.Tick(2, Probe::PROBE_PSOS, 900 * M, true);
    pays.Tick(2 + Probe::SETTLE_FRAMES, Probe::PROBE_PSOS, 800 * M, true);
    assert(pays.GetVerdict() == V::Pays && pays.MayRelease());

    // An unknown commit neither moves the baseline nor decides.
    Probe unknown;
    unknown.Tick(1, 0, 0, false);
    unknown.Tick(2, Probe::PROBE_PSOS, 0, false);
    assert(unknown.MayRelease() && unknown.GetVerdict() == V::Unknown);

    // A trickle that never reaches PROBE_PSOS starts over from a fresh baseline, so commit that
    // grew meanwhile is not taken for PSOs that freed nothing, nor a drop for ones that did.
    Probe trickle;
    trickle.Tick(1, 0, 900 * M, true);
    trickle.Tick(2, 10, 900 * M, true);
    trickle.Tick(3 + Probe::MAX_FRAMES, 20, 700 * M, true); // stale: the baseline moves here
    trickle.Tick(4 + Probe::MAX_FRAMES, 20 + Probe::PROBE_PSOS, 700 * M, true);
    trickle.Tick(4 + Probe::MAX_FRAMES + Probe::SETTLE_FRAMES, 20 + Probe::PROBE_PSOS, 700 * M,
                 true);
    assert(trickle.GetVerdict() == V::DoesNotPay && trickle.Probed() == Probe::PROBE_PSOS);
}

void TestLatencyHistogram() {
    using H = LatencyHistogram;
    // Buckets: exact below 8 us, quarter octaves above; every value inside its bucket's bounds.
    for (std::uint64_t us = 0; us < 8; ++us) {
        assert(H::Bucket(us) == us && H::UpperBound(us) == us);
    }
    for (std::uint64_t us = 1; us < (std::uint64_t{1} << 30); us = us * 9 / 8 + 1) {
        const std::size_t bucket = H::Bucket(us);
        assert(us <= H::UpperBound(bucket));
        assert(bucket == 0 || us > H::UpperBound(bucket - 1));
        assert(H::UpperBound(bucket) - us <= us / 4); // at most 25% above
    }
    assert(H::Bucket(~std::uint64_t{0}) == H::BUCKETS - 1);

    H empty;
    const auto none = empty.Read();
    assert(none.count == 0 && none.p50 == 0 && none.p99 == 0 && none.max == 0);

    // 100 builds of 1..100 ms: percentiles within a bucket of the exact ones, never past the max.
    H histogram;
    for (std::uint64_t ms = 1; ms <= 100; ++ms) {
        histogram.Record(ms * 1000);
    }
    const auto summary = histogram.Read();
    assert(summary.count == 100 && summary.max == 100'000 && summary.total == 5'050'000);
    assert(summary.p50 >= 50'000 && summary.p50 <= 50'000 * 5 / 4);
    assert(summary.p95 >= 95'000 && summary.p95 <= 100'000);
    assert(summary.p99 >= 99'000 && summary.p99 <= 100'000);

    // One long stall among quick waits shows in the max and the tail, not the median.
    H waits;
    for (int i = 0; i < 999; ++i) {
        waits.Record(200);
    }
    waits.Record(110'000);
    const auto tail = waits.Read();
    assert(tail.p50 <= 250 && tail.p99 <= 250 && tail.max == 110'000);
}

/// Saves the set and starts the next session from it, as a new boot does.
HotPipelineSet NextSession(const HotPipelineSet& set) {
    HotPipelineSet next;
    const std::vector<std::uint8_t> bytes = set.Serialize();
    next.Load(bytes);
    return next;
}

void TestHotSet() {
    // No file, or a damaged one: the start of the disk cache is prewarmed.
    HotPipelineSet set;
    set.Load({});
    assert(!set.HasHistory());
    assert(set.Prewarm(123, 0) && set.Prewarm(123, HotPipelineSet::FIRST_SESSION_PREWARM - 1));
    assert(!set.Prewarm(123, HotPipelineSet::FIRST_SESSION_PREWARM));
    const std::vector<std::uint8_t> garbage{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    set.Load(garbage);
    assert(!set.HasHistory());

    // A session marks what it draws, once.
    assert(set.MarkUsed(10) && !set.MarkUsed(10) && set.MarkUsed(20));
    set = NextSession(set);
    assert(set.HasHistory() && set.Size() == 2);
    assert(set.Prewarm(10, 999999) && set.Prewarm(20, 999999) && !set.Prewarm(30, 0));
    assert(set.Known(10) && set.Known(20) && !set.Known(30));
    // Drawn again in a later session: marked again, so it is saved with the new session.
    assert(set.MarkUsed(10));

    // A pipeline not drawn for KEEP_SESSIONS sessions is forgotten; one drawn again stays.
    for (std::uint32_t i = 0; i < HotPipelineSet::KEEP_SESSIONS; ++i) {
        set = NextSession(set);
        set.MarkUsed(10);
    }
    assert(set.Prewarm(10, 0));
    assert(!set.Prewarm(20, 0));

    // A damaged length is rejected whole.
    std::vector<std::uint8_t> bytes = set.Serialize();
    bytes.pop_back();
    HotPipelineSet truncated;
    truncated.Load(bytes);
    assert(!truncated.HasHistory());

    // At most MAX_PREWARM pipelines are built at boot, the most recently drawn first.
    HotPipelineSet big;
    big.Load({});
    for (std::uint64_t hash = 0; hash < HotPipelineSet::MAX_PREWARM; ++hash) {
        big.MarkUsed(1'000'000 + hash); // the older session
    }
    big = NextSession(big);
    for (std::uint64_t hash = 0; hash < 100; ++hash) {
        big.MarkUsed(hash); // the newer one
    }
    big = NextSession(big);
    assert(big.Size() == HotPipelineSet::MAX_PREWARM + 100);
    assert(big.PrewarmSize() == HotPipelineSet::MAX_PREWARM);
    for (std::uint64_t hash = 0; hash < 100; ++hash) {
        assert(big.Prewarm(hash, 0));
    }
    std::size_t old_prewarmed = 0;
    for (std::uint64_t hash = 0; hash < HotPipelineSet::MAX_PREWARM; ++hash) {
        old_prewarmed += big.Prewarm(1'000'000 + hash, 0) ? 1 : 0;
    }
    assert(old_prewarmed == HotPipelineSet::MAX_PREWARM - 100);
    // Those past the cap are still known: the background prewarm takes them first.
    for (std::uint64_t hash = 0; hash < HotPipelineSet::MAX_PREWARM; ++hash) {
        assert(big.Known(1'000'000 + hash));
    }
}

} // namespace

int main() {
    TestIdleFrames();
    TestPrewarmGate();
    TestReleaseProbe();
    TestHotSet();
    TestLatencyHistogram();
    std::cout << "PASS: idle limits and prewarm gate by headroom, the PSO release probe, hot "
                 "pipeline sessions, decay, prewarm cap, file round trip and stage latency "
                 "percentiles\n";
    return 0;
}
