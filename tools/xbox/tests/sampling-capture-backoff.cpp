// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include "common/windows/sampling_capture_backoff.h"
using Common::SamplingProfiler::CaptureBackoff;
void Backoff() {
    CaptureBackoff a, b;
    assert(a.Ready(0));
    a.Observe(100, CaptureBackoff::SLOW_NS); // inclusive fast boundary
    assert(a.Ready(100));
    std::uint64_t now = 100;
    for (std::uint64_t rest : {1ULL, 2ULL, 4ULL, 8ULL, 8ULL, 8ULL}) {
        a.Observe(now, CaptureBackoff::SLOW_NS + 1);
        now += rest * 1'000'000'000;
        assert(!a.Ready(now - 1));
        assert(a.Ready(now));
        assert(b.Ready(now)); // one slow target cannot disable another target
    }
    a.Observe(now, 1);
    assert(a.Ready(now));
    a.Observe(now, CaptureBackoff::SLOW_NS + 1);
    assert(a.Ready(now + 1'000'000'000)); // fast recovery resets the streak
    now = (std::numeric_limits<std::uint64_t>::max)() - 10;
    a.Observe(now, CaptureBackoff::SLOW_NS + 1);
    assert(!a.Ready(now) && a.Ready(now + 10)); // no wrapping deadline
}

int main() { Backoff(); std::cout << "PASS: per-thread capture backoff boundaries/recovery/overflow\n"; }
