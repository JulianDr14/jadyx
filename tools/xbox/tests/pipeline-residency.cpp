// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Pipeline residency gate: idle limits and the background prewarm against the app's headroom, the
// hot pipeline set's sessions, decay, prewarm choice and file round trip, and the stage latency
// histogram's buckets and percentiles. Run with
// build-uwp/diagnostics/pipeline-residency/run.bat.

// The checks must run in any configuration: an NDEBUG build would otherwise pass vacuously.
#undef NDEBUG
#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <span>
#include <tuple>
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

void TestWithinSessionRecency() {
    // A long session visits two disjoint areas. The old hash tie-break filled the boot cap
    // with the first area's low hashes, leaving the entire area at the save point dormant.
    HotPipelineSet learned;
    for (std::uint64_t i = 0; i < HotPipelineSet::MAX_PREWARM; ++i) {
        assert(learned.MarkUsed(i, 1));
        assert(learned.MarkUsed(100000 + i, 1000));
        assert(!learned.MarkUsed(100000 + i, 1000)); // many draws in the same frame
        assert(!learned.MarkUsed(100000 + i, 999)); // no backwards frame order
    }
    auto resumed = NextSession(learned);
    assert(resumed.PrewarmSize() == HotPipelineSet::MAX_PREWARM);
    for (std::uint64_t i = 0; i < HotPipelineSet::MAX_PREWARM; ++i) {
        assert(resumed.Prewarm(100000 + i, 99999));
        assert(!resumed.Prewarm(i, 0));
        assert(resumed.Known(i)); // still available to background prewarm
    }
    // Refreshing an existing pipeline, not only its first use, changes its priority.
    assert(learned.MarkUsed(0, 1001));
    resumed = NextSession(learned);
    assert(resumed.Prewarm(0, 99999));
    assert(resumed.PrewarmSize() == HotPipelineSet::MAX_PREWARM);
    assert(resumed.Recency(0) > resumed.Recency(100000));
    assert(resumed.Recency(100000) > resumed.Recency(1));
    // Session order takes precedence over a large frame number from an older session.
    assert(resumed.MarkUsed(1, 1));
    assert(resumed.Recency(1) > resumed.Recency(0));

    // Read the public v1 format; no existing cache is invalidated by adding frame order.
    const auto write32 = [](auto& bytes, std::size_t offset, std::uint32_t value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    std::vector<std::uint8_t> v1(28);
    std::memcpy(v1.data(), "D3HP", 4);
    write32(v1, 4, 1); write32(v1, 8, 7); write32(v1, 12, 1);
    const std::uint64_t hash = 42;
    std::memcpy(v1.data() + 16, &hash, sizeof(hash));
    write32(v1, 24, 7);
    resumed.Load(v1);
    assert(resumed.Known(hash) && resumed.Prewarm(hash, 0));
    assert(resumed.Recency(hash).first == 7 && resumed.Recency(hash).second == 0);
    assert(resumed.MarkUsed(hash, 77));
    auto migrated = NextSession(resumed);
    assert(migrated.Recency(hash).second == 77 && migrated.Prewarm(hash, 0));

    // A reused object must not retain stale selected keys after a rejected file.
    auto bytes = resumed.Serialize();
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        migrated.Load(bytes);
        migrated.Load(std::span{bytes}.first(size));
        assert(!migrated.HasHistory() && migrated.PrewarmSize() == 0);
    }
    auto overflow = bytes;
    write32(overflow, 12, 0xffffffff);
    migrated.Load(overflow);
    assert(!migrated.HasHistory());
    write32(bytes, 8, 0xffffffff);
    migrated.Load(bytes);
    assert(!migrated.HasHistory());
}

void TestDormantWaitPriority() {
    HotPipelineSet learned;
    constexpr std::uint64_t recent = 100000;
    const auto max_wait = HotPipelineSet::MAX_WAIT_PRIORITY;
    assert(!learned.RecordBuildWait(0, 100000)); // missing use
    for (std::uint64_t i = 0; i < max_wait + 1; ++i) {
        assert(learned.MarkUsed(i, i + 1));
        assert(learned.RecordBuildWait(i, HotPipelineSet::MIN_DORMANT_WAIT_US + i));
    }
    assert(learned.MarkUsed(max_wait + 1, 1));
    assert(!learned.RecordBuildWait(max_wait + 1, HotPipelineSet::MIN_DORMANT_WAIT_US - 1));
    for (std::uint64_t i = 0; i < HotPipelineSet::MAX_PREWARM; ++i) {
        assert(learned.MarkUsed(recent + i, 1000 + i));
    }
    // A new key with a long build competes in the same bounded budget as dormant keys.
    auto with_new_wait = learned;
    const auto new_key = recent + HotPipelineSet::MAX_PREWARM - 1;
    assert(with_new_wait.RecordBuildWait(new_key, 220000));
    auto replay = NextSession(with_new_wait);
    assert(replay.PrewarmSize() == HotPipelineSet::MAX_PREWARM);
    assert(replay.WaitPriorityCount() == max_wait && replay.WaitPriority(new_key));
    assert(!replay.WaitPriority(1));

    auto resumed = NextSession(learned);
    assert(resumed.PrewarmSize() == HotPipelineSet::MAX_PREWARM);
    assert(resumed.WaitPriorityCount() == max_wait);
    assert(!resumed.WaitPriority(0));
    assert(!resumed.Prewarm(0, 0));
    for (std::uint64_t i = 1; i <= max_wait; ++i) {
        assert(resumed.Prewarm(i, 0));
        assert(resumed.WaitPriority(i));
    }
    assert(!resumed.Prewarm(max_wait + 1, 0));
    // A costly candidate still left dormant leads the background queue when memory allows.
    assert(resumed.RankForBackground(0) > resumed.RankForBackground(recent));
    assert(resumed.RankForBackground(recent + 1) > resumed.RankForBackground(recent));
    assert(resumed.RankForBackground(recent) > resumed.RankForBackground(99999));
    assert(!resumed.Prewarm(recent + max_wait - 1, 0));
    assert(resumed.Prewarm(recent + max_wait, 0));
    assert(!resumed.RecordBuildWait(1, 90000)); // it has not been drawn this session

    // A boot hit does not erase the cost, but a key absent from the last route loses priority.
    assert(resumed.MarkUsed(1, 1));
    assert(resumed.WaitPriority(1)); // frozen membership survives the session/frame update
    assert(!resumed.WaitPriority(recent));
    auto third = NextSession(resumed);
    assert(third.WaitPriorityCount() == 1);
    assert(third.Prewarm(1, 0));
    assert(std::get<1>(third.RankForBackground(0)) == 0); // old costs no longer lead
    assert(!third.RecordBuildWait(1, 100000));
    assert(third.MarkUsed(1, 2));
    assert(third.RecordBuildWait(1, std::numeric_limits<std::uint64_t>::max()));
    assert(!third.RecordBuildWait(1, std::numeric_limits<std::uint64_t>::max()));

    // A key selected by both passes consumes one slot. Unused wait slots go to recency.
    assert(learned.RecordBuildWait(recent + HotPipelineSet::MAX_PREWARM - 1, 200000));
    auto overlap = NextSession(learned);
    assert(overlap.WaitPriorityCount() == max_wait);
    assert(overlap.PrewarmSize() == HotPipelineSet::MAX_PREWARM);
    assert(overlap.Prewarm(recent + max_wait - 1, 0));

    // v2 carried the same header and entries without the trailing 32-bit wait cost.
    HotPipelineSet v3;
    assert(v3.MarkUsed(47, 70) && v3.RecordBuildWait(47, 100000));
    auto v2 = v3.Serialize();
    v2.resize(36);
    const std::uint32_t version = 2;
    std::memcpy(v2.data() + 4, &version, sizeof(version));
    third.Load(v2);
    assert((third.Recency(47) == std::pair<std::uint32_t, std::uint64_t>{1, 70}));
    assert(third.WaitPriorityCount() == 0 && third.Prewarm(47, 0));
    assert(!third.WaitPriority(47));
    third.Load(std::span{v2}.first(v2.size() - 1));
    assert(third.WaitPriorityCount() == 0 && !third.HasHistory());
    std::cout << "PASS: dormant wait admission, ranking, shared cap, recency, v2 migration\n";
}

void TestRetainedMultiSessionWaits() {
    HotPipelineSet learned;
    learned.SetRetainMultiSessionWaits(true);
    assert(learned.RetainsMultiSessionWaits());

    // Session 1: 647 long dormant waits (like .227 Run 1) plus one short frame wait.
    for (std::uint64_t i = 0; i < 647; ++i) {
        assert(learned.MarkUsed(1000 + i, 100 + i));
        assert(learned.RecordBuildWait(1000 + i, HotPipelineSet::MIN_DORMANT_WAIT_US + i));
    }
    constexpr std::uint64_t short_s1_key = 9000;
    assert(learned.MarkUsed(short_s1_key, 999));
    assert(learned.RecordFrameWait(short_s1_key, 15000));

    // Session 2: a different area (4096 new pipelines, 600 with long waits).
    auto s2 = NextSession(learned);
    s2.SetRetainMultiSessionWaits(true);
    for (std::uint64_t i = 0; i < HotPipelineSet::MAX_PREWARM; ++i) {
        assert(s2.MarkUsed(200000 + i, 1 + i));
        if (i < 600) {
            assert(s2.RecordBuildWait(200000 + i, 50000 + i));
        }
    }

    // Session 3: another route of 4096 new pipelines without long waits.
    auto s3 = NextSession(s2);
    s3.SetRetainMultiSessionWaits(true);
    for (std::uint64_t i = 0; i < HotPipelineSet::MAX_PREWARM; ++i) {
        assert(s3.MarkUsed(300000 + i, 1 + i));
    }

    // Legacy single-session behavior loses Session 1 & Session 2 waits once S3 fills recency.
    HotPipelineSet legacy;
    legacy.SetRetainMultiSessionWaits(false);
    legacy.Load(s3.Serialize());
    assert(legacy.WaitPriorityCount() == 0);
    assert(legacy.RetainedWaitPriorityCount() == 0);
    assert(std::get<1>(legacy.RankForBackground(1646)) == 0);
    assert(!legacy.Prewarm(1646, 0));

    // With multi-session retention on, up to MAX_RETAINED_WAIT_PRIORITY (1024) are kept:
    // all 600 long waits from S2 + top 424 long waits from S1, while short_s1_key loses to
    // long waits and >= 2432 pure recency slots from S3 remain intact.
    HotPipelineSet retained;
    retained.SetRetainMultiSessionWaits(true);
    retained.Load(s3.Serialize());
    assert(retained.PrewarmSize() == HotPipelineSet::MAX_PREWARM);
    assert(retained.WaitPriorityCount() == 0);
    assert(retained.RetainedWaitPriorityCount() == HotPipelineSet::MAX_RETAINED_WAIT_PRIORITY);
    for (std::uint64_t i = 0; i < 600; ++i) {
        assert(retained.Prewarm(200000 + i, 999999));
        assert(retained.RetainedWaitPriority(200000 + i));
    }
    for (std::uint64_t i = 647 - 424; i < 647; ++i) {
        assert(retained.Prewarm(1000 + i, 999999));
        assert(retained.RetainedWaitPriority(1000 + i));
    }
    assert(!retained.RetainedWaitPriority(1000)); // evicted from 1024-cap by higher waits / S2
    assert(!retained.RetainedWaitPriority(short_s1_key));
    for (std::uint64_t i = 0; i < 3072; ++i) {
        assert(retained.Prewarm(304095 - i, 999999));
    }
    // Background queue keeps wait cost ahead of zero-wait recent candidates across sessions.
    assert(retained.RankForBackground(1000) > retained.RankForBackground(304095));

    // Expiry after KEEP_SESSIONS still purges unvisited S1 keys cleanly.
    HotPipelineSet expired = retained;
    for (std::uint32_t step = 0; step < HotPipelineSet::KEEP_SESSIONS; ++step) {
        expired = NextSession(expired);
        expired.SetRetainMultiSessionWaits(true);
        expired.MarkUsed(300000, step + 1);
    }
    assert(!expired.Known(1646));
    assert(!expired.Prewarm(1646, 0));
    assert(!expired.RetainedWaitPriority(1646));
    std::cout << "PASS: multi-session wait retention (1024 cap, tier precedence, recency floor, KEEP_SESSIONS expiry)\n";
}

void TestStallFirstAndConfigurableCaps() {
    // 1. Stall-first wait scoring: real GPU/draw stalls always outrank async worker builds.
    static_assert(HotPipelineSet::EffectiveLearnedWaitUs(0, 10000) == 0);
    static_assert(HotPipelineSet::EffectiveLearnedWaitUs(0, 100000) == 100000);
    static_assert(HotPipelineSet::EffectiveLearnedWaitUs(0, 2000000) ==
                  HotPipelineSet::REAL_STALL_BIAS_US - 1);
    static_assert(HotPipelineSet::EffectiveLearnedWaitUs(40000, 100000) ==
                  HotPipelineSet::REAL_STALL_BIAS_US + 40000);
    static_assert(HotPipelineSet::EffectiveLearnedWaitUs(5000000, 5000000) ==
                  HotPipelineSet::REAL_STALL_BIAS_US + 3000000);

    // 2. Configurable wait_priority_cap (1536) retains all 305 real stalls + top async builds.
    HotPipelineSet s1;
    for (std::uint64_t i = 0; i < 1400; ++i) {
        assert(s1.MarkUsed(1000 + i, 10 + i));
        const auto worker_only =
            HotPipelineSet::EffectiveLearnedWaitUs(0, 500000 + i * 100);
        assert(s1.RecordBuildWait(1000 + i, worker_only));
    }
    for (std::uint64_t i = 0; i < 305; ++i) {
        assert(s1.MarkUsed(50000 + i, 2000 + i));
        const auto real_stall =
            HotPipelineSet::EffectiveLearnedWaitUs(32000 + i * 1000, 200000);
        assert(s1.RecordBuildWait(50000 + i, real_stall));
    }

    HotPipelineSet s2_capped;
    s2_capped.SetWaitPriorityCap(1536);
    s2_capped.SetRetainMultiSessionWaits(true);
    s2_capped.Load(s1.Serialize());
    assert(s2_capped.WaitPriorityCount() == 1536);
    for (std::uint64_t i = 0; i < 305; ++i) {
        assert(s2_capped.WaitPriority(50000 + i));
        assert(s2_capped.Prewarm(50000 + i, 999999));
    }
    // Lower worker-only builds outside top 1231 async slots are dropped when 305 real stalls win.
    assert(!s2_capped.WaitPriority(1000));
    assert(s2_capped.WaitPriority(1000 + 1399));

    // Verify 4,000,000 us ceiling survives serialization without 250 ms clamping.
    HotPipelineSet ceiling_src;
    assert(ceiling_src.MarkUsed(1, 10));
    assert(ceiling_src.RecordBuildWait(1, 4000000));
    assert(ceiling_src.MarkUsed(2, 11));
    assert(ceiling_src.RecordBuildWait(2, 1500000));
    HotPipelineSet ceiling_dst;
    ceiling_dst.SetWaitPriorityCap(1);
    ceiling_dst.Load(ceiling_src.Serialize());
    assert(ceiling_dst.WaitPriority(1));
    assert(!ceiling_dst.WaitPriority(2));

    // 3. BackgroundRank tier precedence:
    //    tier 4 (prev long) > tier 3 (prev short) > tier 2 (older long) > tier 1 (older short) > tier 0.
    HotPipelineSet older_src;
    older_src.SetKeepSessions(64);
    older_src.SetRetainMultiSessionWaits(true);
    assert(older_src.MarkUsed(10, 100)); // older long wait
    assert(older_src.RecordBuildWait(10, 200000));
    assert(older_src.MarkUsed(11, 101)); // older short wait
    assert(older_src.RecordFrameWait(11, 15000));

    HotPipelineSet prev_src;
    prev_src.SetKeepSessions(64);
    prev_src.SetRetainMultiSessionWaits(true);
    prev_src.Load(older_src.Serialize());
    assert(prev_src.MarkUsed(20, 200)); // prev long wait (smaller wait than older 10!)
    assert(prev_src.RecordBuildWait(20, 40000));
    assert(prev_src.MarkUsed(21, 201)); // prev short wait
    assert(prev_src.RecordFrameWait(21, 8000));
    assert(prev_src.MarkUsed(22, 9999)); // prev zero-wait high frame

    HotPipelineSet ranked;
    ranked.SetKeepSessions(64);
    ranked.SetRetainMultiSessionWaits(true);
    ranked.Load(prev_src.Serialize());
    assert(std::get<1>(ranked.RankForBackground(20)) == 4);
    assert(std::get<1>(ranked.RankForBackground(21)) == 3);
    assert(std::get<1>(ranked.RankForBackground(10)) == 2);
    assert(std::get<1>(ranked.RankForBackground(11)) == 1);
    assert(std::get<1>(ranked.RankForBackground(22)) == 0);
    assert(ranked.RankForBackground(20) > ranked.RankForBackground(21));
    assert(ranked.RankForBackground(21) > ranked.RankForBackground(10));
    assert(ranked.RankForBackground(10) > ranked.RankForBackground(11));
    assert(ranked.RankForBackground(11) > ranked.RankForBackground(22));

    // 4. Configurable keep_sessions (64) survives past default 16 sessions and expires at 64.
    HotPipelineSet keep = ranked;
    for (std::uint32_t step = 0; step < 30; ++step) {
        HotPipelineSet tmp;
        tmp.SetKeepSessions(64);
        tmp.SetRetainMultiSessionWaits(true);
        tmp.Load(keep.Serialize());
        tmp.MarkUsed(999, step + 1);
        keep = std::move(tmp);
    }
    assert(keep.Known(10));
    assert(keep.RetainedWaitPriority(10));
    for (std::uint32_t step = 30; step < 64; ++step) {
        HotPipelineSet tmp;
        tmp.SetKeepSessions(64);
        tmp.SetRetainMultiSessionWaits(true);
        tmp.Load(keep.Serialize());
        tmp.MarkUsed(999, step + 1);
        keep = std::move(tmp);
    }
    assert(!keep.Known(10));
    assert(!keep.RetainedWaitPriority(10));
    std::cout << "PASS: stall-first scoring, wait_priority_cap(1536), 5-tier BackgroundRank, keep_sessions(64)\n";
}

} // namespace

int main() {
    TestIdleFrames();
    TestPrewarmGate();
    TestReleaseProbe();
    TestHotSet();
    TestWithinSessionRecency();
    TestDormantWaitPriority();
    TestRetainedMultiSessionWaits();
    TestStallFirstAndConfigurableCaps();
    TestLatencyHistogram();
    std::cout << "PASS: idle limits and prewarm gate by headroom, the PSO release probe, hot "
                 "pipeline sessions, decay, prewarm cap, file round trip and stage latency "
                 "percentiles\n";
    return 0;
}
