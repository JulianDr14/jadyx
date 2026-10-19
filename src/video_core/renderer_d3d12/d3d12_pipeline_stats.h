// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>

// Where a pipeline's time goes, stage by stage, as distributions: the perf counters only keep
// totals, and a 110 ms stall hides in a total of small waits. Recording is a few relaxed atomic
// adds, cheap enough to stay on in release builds. Pure, so the bucketing is covered by
// tools/xbox/tests/pipeline-residency.cpp.
namespace D3D12 {

/// Microsecond latencies in quarter-octave buckets: exact below 8 us, at most 25% above the real
/// value past that. Percentiles report a bucket's upper bound, capped at the largest sample.
class LatencyHistogram {
public:
    static constexpr std::size_t BUCKETS = 8 + 4 * 40; ///< Up to 2^43 us, about 100 days.

    struct Summary {
        std::uint64_t count{};
        std::uint64_t total{};
        std::uint64_t p50{};
        std::uint64_t p95{};
        std::uint64_t p99{};
        std::uint64_t max{};
    };

    [[nodiscard]] static constexpr std::size_t Bucket(std::uint64_t us) noexcept {
        if (us < 8) {
            return static_cast<std::size_t>(us);
        }
        const auto width = static_cast<std::size_t>(std::bit_width(us)); // >= 4
        const auto sub = static_cast<std::size_t>((us >> (width - 3)) & 3);
        return (std::min)(8 + (width - 4) * 4 + sub, BUCKETS - 1);
    }

    /// The largest value a bucket holds.
    [[nodiscard]] static constexpr std::uint64_t UpperBound(std::size_t bucket) noexcept {
        if (bucket < 8) {
            return bucket;
        }
        const std::size_t width = (bucket - 8) / 4 + 4;
        const std::uint64_t sub = (bucket - 8) % 4;
        const std::uint64_t step = std::uint64_t{1} << (width - 3);
        return ((4 + sub) << (width - 3)) + step - 1;
    }

    void Record(std::uint64_t us) noexcept {
        buckets[Bucket(us)].fetch_add(1, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
        total.fetch_add(us, std::memory_order_relaxed);
        std::uint64_t seen = max.load(std::memory_order_relaxed);
        while (us > seen && !max.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {
        }
    }

    /// A consistent enough snapshot for a log line: samples recorded meanwhile may be half in.
    [[nodiscard]] Summary Read() const noexcept {
        Summary summary;
        std::array<std::uint64_t, BUCKETS> snapshot;
        for (std::size_t i = 0; i < BUCKETS; ++i) {
            snapshot[i] = buckets[i].load(std::memory_order_relaxed);
            summary.count += snapshot[i];
        }
        summary.total = total.load(std::memory_order_relaxed);
        summary.max = max.load(std::memory_order_relaxed);
        const auto percentile = [&](std::uint64_t per_mille) {
            // The smallest bucket that covers per_mille of the samples (rank rounded up).
            const std::uint64_t rank = (summary.count * per_mille + 999) / 1000;
            std::uint64_t seen = 0;
            for (std::size_t i = 0; i < BUCKETS; ++i) {
                seen += snapshot[i];
                if (seen >= rank && seen != 0) {
                    return (std::min)(UpperBound(i), summary.max);
                }
            }
            return summary.max;
        };
        if (summary.count != 0) {
            summary.p50 = percentile(500);
            summary.p95 = percentile(950);
            summary.p99 = percentile(990);
        }
        return summary;
    }

    /// Starts over; samples recorded meanwhile may be half kept.
    void Reset() noexcept {
        for (auto& bucket : buckets) {
            bucket.store(0, std::memory_order_relaxed);
        }
        count.store(0, std::memory_order_relaxed);
        total.store(0, std::memory_order_relaxed);
        max.store(0, std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<std::uint64_t>, BUCKETS> buckets{};
    std::atomic<std::uint64_t> count{};
    std::atomic<std::uint64_t> total{};
    std::atomic<std::uint64_t> max{};
};

/// The stages of a graphics pipeline's preparation, and the GPU thread's waits by their cause.
enum class PipelineStage : std::size_t {
    Frontend,    ///< GPU thread: Maxwell -> IR -> SPIR-V of a new pipeline.
    QueueWait,   ///< Queued -> started, on a worker or claimed by the GPU thread.
    Dxil,        ///< SPIR-V -> linked, signed DXIL (Mesa + validator).
    PsoCreate,   ///< The driver's CreateGraphicsPipelineState, misses of the PSO index only.
    WaitNew,     ///< GPU thread blocked on a pipeline first seen this session.
    WaitDormant, ///< GPU thread blocked on a disk-cache pipeline whose PSO was never built.
    WaitEvicted, ///< GPU thread blocked on a PSO released while idle and drawn again.
    Count,
};

struct PipelineStats {
    std::array<LatencyHistogram, static_cast<std::size_t>(PipelineStage::Count)> stages;
    std::atomic<std::uint64_t> inline_builds{};  ///< Builds the waiting GPU thread ran itself.
    std::atomic<std::uint64_t> prewarm_queued{}; ///< Dormant PSOs built ahead in the background.
    std::atomic<std::uint64_t> prewarm_hits{};   ///< Of those, drawn later this session.

    void Record(PipelineStage stage, std::uint64_t us) noexcept {
        stages[static_cast<std::size_t>(stage)].Record(us);
    }
    [[nodiscard]] LatencyHistogram::Summary Read(PipelineStage stage) const noexcept {
        return stages[static_cast<std::size_t>(stage)].Read();
    }
    /// At each game's load: the counters are process-wide and would mix a second game's numbers.
    void Reset() noexcept {
        for (auto& stage : stages) {
            stage.Reset();
        }
        inline_builds.store(0, std::memory_order_relaxed);
        prewarm_queued.store(0, std::memory_order_relaxed);
        prewarm_hits.store(0, std::memory_order_relaxed);
    }
};

/// Monotonic microseconds, for the stages' timestamps.
inline std::uint64_t SteadyMicros() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

/// Process-wide: one renderer at a time.
inline PipelineStats& GetPipelineStats() noexcept {
    static PipelineStats stats;
    return stats;
}

} // namespace D3D12
