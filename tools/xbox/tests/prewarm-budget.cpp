// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The checks must run in any configuration: an NDEBUG build would otherwise pass vacuously.
#undef NDEBUG
#include <cassert>
#include <iostream>
#include "jadyx/app/prewarm_budget.h"

namespace {
using Jadyx::PrewarmBudget;
constexpr std::uint64_t M = PrewarmBudget::MiB;

void TestDefaultsAndClamp() {
    assert(PrewarmBudget{}.MiBPerCore() == 192);
    assert(PrewarmBudget{}.BytesPerCore() == 192 * M);
    assert(PrewarmBudget{-3}.Step() == 0);
    assert(PrewarmBudget{99}.MiBPerCore() == 32);
}

void TestLowersOncePerSession() {
    PrewarmBudget budget;
    assert(!budget.Observe(2048 * M));
    assert(!budget.Observe(257 * M));
    for (std::uint32_t i = 1; i < PrewarmBudget::LOW_OBSERVATIONS; ++i) {
        assert(!budget.Observe(255 * M)); // Permafrost may still recover it
    }
    assert(!budget.Observe(300 * M)); // it did: the run starts over
    for (std::uint32_t i = 1; i < PrewarmBudget::LOW_OBSERVATIONS; ++i) {
        assert(!budget.Observe(255 * M));
    }
    assert(budget.MiBPerCore() == 192);
    assert(budget.Observe(255 * M)); // Game C at the limit: 192 -> 128 for the next session
    assert(budget.MiBPerCore() == 128);
    assert(!budget.Observe(0)); // once per session, however low it goes
    assert(budget.MiBPerCore() == 128);
    assert(!budget.Finish(3600)); // a session that came close never raises it
    assert(budget.MinHeadroom() == 0);

    PrewarmBudget lowest{3};
    for (std::uint32_t i = 0; i < PrewarmBudget::LOW_OBSERVATIONS; ++i) {
        assert(!lowest.Observe(0));
    }
    assert(lowest.MiBPerCore() == 32);

    PrewarmBudget dip; // a brief dip lowers nothing, but a session that saw it never raises
    assert(!dip.Observe(100 * M) && dip.MiBPerCore() == 192);
    PrewarmBudget dipped{1};
    assert(!dipped.Observe(100 * M) && !dipped.Finish(3600) && dipped.MiBPerCore() == 128);
}

void TestRaisesAfterAComfortableSession() {
    PrewarmBudget budget{1};
    assert(!budget.Observe(1500 * M));
    assert(!budget.Finish(PrewarmBudget::RAISE_SECONDS - 1)); // too short to judge
    PrewarmBudget long_enough{2};
    assert(!long_enough.Observe(1500 * M));
    assert(long_enough.Finish(PrewarmBudget::RAISE_SECONDS));
    assert(long_enough.MiBPerCore() == 128);

    PrewarmBudget tight{1};
    assert(!tight.Observe(511 * M)); // between the thresholds: keep
    assert(!tight.Finish(3600) && tight.MiBPerCore() == 128);

    PrewarmBudget top{};
    assert(!top.Observe(4096 * M));
    assert(!top.Finish(3600) && top.Step() == 0);

    PrewarmBudget unobserved{1}; // no samples: the minimum stays at its maximum
    assert(unobserved.Finish(PrewarmBudget::RAISE_SECONDS) && unobserved.Step() == 0);
}
} // namespace

int main() {
    TestDefaultsAndClamp();
    TestLowersOncePerSession();
    TestRaisesAfterAComfortableSession();
    std::cout << "PASS: defaults, clamping, lowering once per session after a sustained dip, raising after a "
                 "comfortable session\n";
}
