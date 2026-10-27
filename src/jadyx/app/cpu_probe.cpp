// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include <intrin.h>
#include <windows.h>

#include "common/cpu_wait.h"
#include "common/windows/cpu_pin.h"
#include "jadyx/app/cpu_probe.h"

namespace Jadyx {

namespace {

using Ticks = long long;

Ticks Rdtsc() {
    return static_cast<Ticks>(__rdtsc());
}

void PinTo(unsigned long cpu_set) {
    SetThreadSelectedCpuSets(GetCurrentThread(), &cpu_set, 1);
    // Off the logical processor it was on, if that is not the one asked for.
    SwitchToThread();
}

/// TSC ticks per microsecond, against the performance counter over 20 ms.
double TscPerMicrosecond() {
    LARGE_INTEGER frequency{}, qpc0{}, qpc1{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&qpc0);
    const Ticks tsc0 = Rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    QueryPerformanceCounter(&qpc1);
    const Ticks tsc1 = Rdtsc();
    const double us = static_cast<double>(qpc1.QuadPart - qpc0.QuadPart) * 1e6 /
                      static_cast<double>(frequency.QuadPart);
    return static_cast<double>(tsc1 - tsc0) / us;
}

std::string Format(const char* format, auto... args) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), format, args...);
    return buffer;
}

/// Every logical processor spins on the TSC at once for 500 ms; a gap longer than 20 us is time
/// the thread did not run.
std::string StealTest(const std::vector<unsigned long>& sets, double tsc_us) {
    constexpr double duration_us = 500'000;
    const Ticks gap = static_cast<Ticks>(20 * tsc_us);
    struct Result {
        Ticks stolen{};
        Ticks longest{};
        unsigned gaps{};
    };
    std::vector<Result> results(sets.size());
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::jthread> threads;
    for (std::size_t i = 0; i < sets.size(); ++i) {
        threads.emplace_back([&, i] {
            PinTo(sets[i]);
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {
                _mm_pause();
            }
            Result& result = results[i];
            const Ticks end = Rdtsc() + static_cast<Ticks>(duration_us * tsc_us);
            Ticks last = Rdtsc();
            while (last < end) {
                const Ticks now = Rdtsc();
                if (now - last > gap) {
                    result.stolen += now - last;
                    result.longest = (std::max)(result.longest, now - last);
                    ++result.gaps;
                }
                last = now;
            }
        });
    }
    while (ready.load() != sets.size()) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    threads.clear();
    std::string line = "CPU probe: time the system took while all spun 500 ms;";
    for (std::size_t i = 0; i < sets.size(); ++i) {
        const Result& result = results[i];
        line += Format(" %lu=%.1f%%/%u gaps/max %.0f us", sets[i],
                       static_cast<double>(result.stolen) * 100.0 / (duration_us * tsc_us),
                       result.gaps, static_cast<double>(result.longest) / tsc_us);
    }
    return line;
}

/// A MWAITX waiter on waiter_set, woken 300 times by a store from writer_set: how long after the
/// store it ran again. The timer is 1 ms, so a store that does not end the wait shows as ~1 ms.
std::string WakeTest(const char* label, unsigned long waiter_set, unsigned long writer_set,
                     double tsc_us) {
    constexpr unsigned rounds = 300;
    const Ticks timeout = static_cast<Ticks>(1000 * tsc_us);
    struct alignas(64) Line {
        std::atomic<unsigned> flag{0};
    };
    Line line;
    alignas(64) std::atomic<Ticks> stamp{0};
    alignas(64) std::atomic<unsigned> round{0};
    alignas(64) std::atomic<unsigned> done{0};
    std::vector<double> latencies;
    latencies.reserve(rounds);
    std::jthread waiter([&] {
        PinTo(waiter_set);
        for (unsigned i = 1; i <= rounds; ++i) {
            while (round.load(std::memory_order_acquire) != i) {
                _mm_pause();
            }
            const Ticks deadline = Rdtsc() + 4 * timeout;
            for (;;) {
                _mm_monitorx(&line.flag, 0, 0);
                if (line.flag.load(std::memory_order_acquire) != 0 || Rdtsc() > deadline) {
                    break;
                }
                _mm_mwaitx(1u << 1, 0, static_cast<unsigned>(timeout));
            }
            const Ticks woke = Rdtsc();
            if (line.flag.load(std::memory_order_acquire) != 0) {
                latencies.push_back(static_cast<double>(woke - stamp.load()) / tsc_us);
            }
            done.store(i, std::memory_order_release);
        }
    });
    std::jthread writer([&] {
        PinTo(writer_set);
        for (unsigned i = 1; i <= rounds; ++i) {
            line.flag.store(0, std::memory_order_relaxed);
            round.store(i, std::memory_order_release);
            // Long enough for the waiter to be asleep in MWAITX before the store.
            const Ticks store_at = Rdtsc() + static_cast<Ticks>((30 + i % 20) * tsc_us);
            while (Rdtsc() < store_at) {
                _mm_pause();
            }
            stamp.store(Rdtsc(), std::memory_order_relaxed);
            line.flag.store(1, std::memory_order_release);
            while (done.load(std::memory_order_acquire) != i) {
                _mm_pause();
            }
        }
    });
    writer.join();
    waiter.join();
    if (latencies.empty()) {
        return Format("CPU probe: MWAITX wake from %s: no wakes", label);
    }
    std::sort(latencies.begin(), latencies.end());
    const auto at = [&](double q) {
        return latencies[static_cast<std::size_t>(q * static_cast<double>(latencies.size() - 1))];
    };
    const auto missed = std::count_if(latencies.begin(), latencies.end(),
                                      [](double us) { return us > 500; });
    return Format("CPU probe: MWAITX wake from %s (%lu -> %lu): median %.2f us, p90 %.2f us, "
                  "p99 %.2f us, max %.0f us, %d of %zu only at the 1 ms timer",
                  label, writer_set, waiter_set, at(0.5), at(0.9), at(0.99), latencies.back(),
                  static_cast<int>(missed), latencies.size());
}

} // namespace

void RunCpuProbe(const std::function<void(const std::string&)>& write) {
    const auto cores = Common::Windows::AppCoreCpuSets();
    std::vector<unsigned long> sets;
    for (const auto& core : cores) {
        sets.insert(sets.end(), core.begin(), core.end());
    }
    if (sets.empty()) {
        write("CPU probe: no CPU sets");
        return;
    }
    const double tsc_us = TscPerMicrosecond();
    write(Format("CPU probe: TSC %.1f MHz", tsc_us));
    write(StealTest(sets, tsc_us));
    if (Common::BestCpuWaitMethod() != Common::CpuWaitMethod::MonitorX) {
        write("CPU probe: MWAITX unavailable");
        return;
    }
    if (cores[0].size() > 1) {
        write(WakeTest("the SMT sibling", cores[0][0], cores[0][1], tsc_us));
    }
    if (cores.size() > 1) {
        write(WakeTest("another core", cores[0][0], cores[1][0], tsc_us));
        if (cores[1].size() > 1) {
            write(WakeTest("another core's second thread", cores[0][0], cores[1][1], tsc_us));
        }
    }
}

} // namespace Jadyx
