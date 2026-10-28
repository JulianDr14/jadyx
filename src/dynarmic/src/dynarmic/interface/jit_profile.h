// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: 0BSD

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace Dynarmic::JitProfile {

enum class Phase : std::size_t {
    Compile, Translate, Optimize, Emit, Protect, Invalidate,
    WriteOpen, EmitSetup, Instructions, Terminal, Deferred, Ranges,
    Register, LinkPatch, DescriptorInsert, WriteClose, EmitCleanup,
    CfgPreserveRx, CfgInitializeRx, CfgFallback, Count
};
struct Measurement {
    std::uint64_t calls{};
    std::uint64_t ns{};
};
using Snapshot = std::array<Measurement, static_cast<std::size_t>(Phase::Count)>;
inline constexpr std::array<const char*, static_cast<std::size_t>(Phase::Count)> names{
    "compile", "translate", "optimize", "emit", "protect", "invalidate",
    "write-open", "setup", "instructions", "terminal", "deferred", "ranges",
    "register", "link-patch", "descriptor-insert", "write-close", "cleanup",
    "cfg-preserve-rx", "cfg-initialize-rx", "cfg-fallback"
};
struct Counters {
    std::atomic<std::uint64_t> calls{};
    std::atomic<std::uint64_t> ns{};
};
inline std::array<Counters, static_cast<std::size_t>(Phase::Count)> counters;
inline std::atomic_bool enabled{};
/// Code caches emptied because they filled up, all their blocks dropped with them. Always counted.
inline std::atomic<std::uint64_t> full_cache_clears{};

/// Bytes of emitted A64 code by part of the block, to see what fills the code caches. Counted
/// only with profiling on; the memory part is the guest loads and stores emitted inline.
enum class CodePart : std::size_t { Align, Prologue, Memory, Other, Terminal, Deferred, Count };
inline constexpr std::array<const char*, static_cast<std::size_t>(CodePart::Count)> code_part_names{
    "align", "prologue", "memory", "other", "terminal", "deferred"
};
inline std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(CodePart::Count)> code_bytes{};
inline std::atomic<std::uint64_t> code_memory_accesses{};
inline std::atomic<std::uint64_t> code_guest_instructions{};
// Monotonic on this host thread; sampled around one guest Run to exclude other cores.
inline thread_local std::uint64_t local_compile_ns{};
[[nodiscard]] inline std::uint64_t ReadLocalCompileNs() { return local_compile_ns; }

inline void SetEnabled(bool value) { enabled.store(value, std::memory_order_relaxed); }

/// Measured even with profiling off: the renderer's hitch report names JIT compilation from them.
/// A block compile takes tens of microseconds; two clock reads and two atomic adds are noise.
[[nodiscard]] constexpr bool AlwaysTimed(Phase phase) {
    return phase == Phase::Compile || phase == Phase::Invalidate;
}

// Cold compilation/protection paths only. No timestamp when disabled, except AlwaysTimed phases.
// Measurements are elapsed times summed across JIT instances, including host preemption. Compile
// contains Translate/Optimize/Emit, and Emit may contain Protect; these totals are not additive.
class Timer {
public:
    explicit Timer(Phase phase_)
        : phase{phase_}, active{AlwaysTimed(phase_) || enabled.load(std::memory_order_relaxed)} {
        if (active) start = std::chrono::steady_clock::now();
    }
    ~Timer() { Stop(); }
    void Stop() {
        if (!active) return;
        active = false;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (phase == Phase::Compile) local_compile_ns += static_cast<std::uint64_t>(ns);
        auto& counter = counters[static_cast<std::size_t>(phase)];
        counter.ns.fetch_add(static_cast<std::uint64_t>(ns), std::memory_order_relaxed);
        counter.calls.fetch_add(1, std::memory_order_relaxed);
    }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    Phase phase;
    bool active;
    std::chrono::steady_clock::time_point start;
};

// Monotonic totals can also be sampled at the borders of a T capture without disturbing
// renderer windows. Calls/durations are independent loads, not a transaction across cores.
[[nodiscard]] inline Snapshot Read() {
    Snapshot snapshot{};
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        snapshot[i].calls = counters[i].calls.load(std::memory_order_relaxed);
        snapshot[i].ns = counters[i].ns.load(std::memory_order_relaxed);
    }
    return snapshot;
}

[[nodiscard]] inline Measurement Read(Phase phase) {
    const auto& counter = counters[static_cast<std::size_t>(phase)];
    return {counter.calls.load(std::memory_order_relaxed), counter.ns.load(std::memory_order_relaxed)};
}

// Single consumer (renderer): retain the preceding snapshot instead of clearing totals.
// Calls crossing a boundary are counted when completed, including their whole duration.
[[nodiscard]] inline Snapshot Take() {
    static Snapshot previous{};
    const auto current = Read();
    Snapshot delta{};
    for (std::size_t i = 0; i < current.size(); ++i) {
        delta[i] = {current[i].calls - previous[i].calls, current[i].ns - previous[i].ns};
    }
    previous = current;
    return delta;
}

/// Code part bytes since the last call, then memory accesses and guest instructions.
[[nodiscard]] inline std::array<std::uint64_t, static_cast<std::size_t>(CodePart::Count) + 2> TakeCodeBytes() {
    static std::array<std::uint64_t, static_cast<std::size_t>(CodePart::Count) + 2> previous{};
    std::array<std::uint64_t, static_cast<std::size_t>(CodePart::Count) + 2> current{};
    for (std::size_t i = 0; i < code_bytes.size(); ++i) {
        current[i] = code_bytes[i].load(std::memory_order_relaxed);
    }
    current[code_bytes.size()] = code_memory_accesses.load(std::memory_order_relaxed);
    current[code_bytes.size() + 1] = code_guest_instructions.load(std::memory_order_relaxed);
    auto delta = current;
    for (std::size_t i = 0; i < delta.size(); ++i) {
        delta[i] -= previous[i];
    }
    previous = current;
    return delta;
}

} // namespace Dynarmic::JitProfile
