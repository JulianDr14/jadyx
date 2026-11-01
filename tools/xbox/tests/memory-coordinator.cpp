// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// The checks must run in any configuration: an NDEBUG build would otherwise pass vacuously.
#undef NDEBUG
#include <cassert>
#include <iostream>
#include "jadyx/app/memory_coordinator.h"
using Jadyx::MemoryCoordinator;
constexpr u64 MiB = MemoryCoordinator::MiB;

namespace {
// Marks the numbers below were worked out with; the checks are about the logic, not the defaults.
MemoryCoordinator::Config Marks() {
    MemoryCoordinator::Config config;
    config.watch = 1936 * MiB;
    config.low = 1424 * MiB;
    config.high = 1680 * MiB;
    return config;
}

// Drives the coordinator in 100 ms steps with the guest stalling `stall_ms_per_s`.
struct Sim {
    MemoryCoordinator c{Marks()};
    u64 now{}, stall_us{};
    MemoryCoordinator::Plan plan{};
    MemoryCoordinator::Plan Run(u64 ms, u64 stall_ms_per_s, u64 heap = 700 * MiB,
                                u64 cap = 768 * MiB) {
        for (u64 t = 0; t < ms; t += 100) {
            now += 100;
            stall_us += stall_ms_per_s * 100;
            plan = c.Update({now, 1000 * MiB, stall_us, heap, 768 * MiB, cap});
        }
        return plan;
    }
};
} // namespace

int main() {
    // With nothing measured, every mark is what it was before the coordinator.
    const auto initial = MemoryCoordinator{Marks()}.Initial();
    assert(initial.low == 1424 * MiB && initial.high == 1680 * MiB && initial.watch == 1936 * MiB);
    assert(initial.urgent == 712 * MiB && initial.emergency == 356 * MiB);
    assert(initial.jit_stop == 712 * MiB && initial.jit_release == 267 * MiB);
    assert(initial.jit_uncap == 1424 * MiB);
    assert(initial.texture_line == 2192 * MiB && !initial.yield && initial.relief == 0);

    // Calm: no stall, no yield, the textures may grow; what they hold above the floor is a loan.
    Sim calm;
    auto p = calm.Run(10000, 0, 1100 * MiB, 1300 * MiB);
    assert(!p.yield && p.relief == 0 && p.texture_line == 2192 * MiB);
    assert(p.texture_loan == 332 * MiB);
    // A trickle (a few cold granules read back) never yields.
    p = calm.Run(60000, 5);
    assert(!p.yield && p.relief == 0);

    // The loop seen on the console: the guest stalls hundreds of ms per second.
    Sim loop;
    loop.Run(5000, 0);
    p = loop.Run(400, 700);
    assert(p.yield && p.relief == 0);       // the others give way first...
    assert(p.texture_line == 0 && p.texture_loan == 0 && p.low == 1424 * MiB);
    p = loop.Run(2500, 700);
    assert(p.yield && p.relief == 0);       // ...for the whole grace
    p = loop.Run(1000, 700);
    assert(p.relief > 0 && p.relief <= 256 * MiB); // then Permafrost's marks come down
    p = loop.Run(10000, 700);
    assert(p.relief == 912 * MiB && p.low == 512 * MiB); // never below the floor
    assert(p.high == 768 * MiB && p.urgent == 384 * MiB && p.emergency == 192 * MiB);
    assert(p.jit_stop == 384 * MiB && p.jit_release == 160 * MiB && p.jit_uncap == 1024 * MiB);
    // Ordering kept at full relief: texture critical (384) <= urgent < low < high.
    assert(p.urgent <= p.low && p.low < p.high && p.high <= p.watch);

    // The stall ends: the yield ends, the relief holds, then eases slowly.
    p = loop.Run(10000, 0); // the smoothed stall takes ~4.5 s to fall from 700 to 8 ms/s
    assert(!p.yield && p.relief == 912 * MiB && p.texture_line == 0);
    p = loop.Run(20000, 0);
    assert(p.relief == 912 * MiB);          // still inside the 30 s hold
    p = loop.Run(10000, 0);
    assert(p.relief < 912 * MiB && p.relief >= 912 * MiB - 11 * 32 * MiB);
    // The stall comes back while it eases: the relief stops easing just above that point.
    const u64 came_back = p.relief;
    loop.Run(4000, 700);
    p = loop.Run(60000, 0);
    assert(p.relief >= std::min<u64>(912 * MiB, came_back + 128 * MiB));
    p = loop.Run(120000, 0);
    p = loop.Run(60000, 0);
    assert(p.relief == 0 && !p.yield);
    p = loop.Run(1000, 0, 1100 * MiB, 1300 * MiB);
    assert(p.texture_line == 2192 * MiB && p.texture_loan == 332 * MiB); // growing again

    // The order of who gives way holds at every relief, not only at both ends. Going down in
    // headroom: idle textures (pressure 768, critical 384), Permafrost (low, then urgent, then
    // emergency), and last the JIT, whose hot code costs the most to get back: it stops growing
    // where Permafrost turns urgent and gives back only below Permafrost's emergency mark.
    Sim order;
    order.Run(5000, 0);
    for (int step = 0; step < 200; ++step) {
        p = order.Run(100, 700);
        assert(p.emergency <= p.urgent && p.urgent <= p.low && p.low <= p.high &&
               p.high <= p.watch);
        assert(p.urgent >= 384 * MiB);      // textures reach critical before Permafrost's urgent
        assert(p.low >= 512 * MiB);
        assert(p.jit_stop == p.urgent);     // the JIT freezes only once Permafrost is urgent...
        assert(p.jit_release < p.emergency); // ...and gives back after its emergency
        assert(p.jit_uncap >= p.low);
        assert(p.texture_line == 0);        // no texture growth while anyone is giving way
    }
    assert(p.relief == 912 * MiB);

    // A short burst (a load) yields, but ends before the grace: no relief.
    Sim burst;
    burst.Run(5000, 0);
    burst.Run(1000, 300);
    p = burst.Run(10000, 0);
    assert(p.relief == 0);

    // The line can be set, or turned off.
    MemoryCoordinator::Config fixed;
    fixed.texture_line = 0;
    assert(MemoryCoordinator{fixed}.Initial().texture_line == 0);
    fixed.texture_line = 3000 * MiB;
    assert(MemoryCoordinator{fixed}.Initial().texture_line == 3000 * MiB);

    std::cout << "PASS: marks unchanged when calm, yield before relief, relief floor, hold, slow "
                 "easing, settling after a returning stall, texture growth and loan\n";
}
