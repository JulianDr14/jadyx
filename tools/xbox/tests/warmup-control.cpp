// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#undef NDEBUG
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include "core/arm/jit_prewarm_parallel.h"
#include "jadyx/app/warmup_control.h"

int main() {
    using namespace Jadyx;
    assert(!WarmupControlEnabled(nullptr));
    assert(!WarmupControlEnabled("0"));
    assert(!WarmupControlEnabled("true"));
    assert(WarmupControlEnabled("1"));
    // New session, held buttons, disconnect/reconnect and duplicate requests.
    for (unsigned repeat = 0; repeat < 10; ++repeat) {
        WarmupControl state;
        state.Poll(true, true);
        assert(!state.Token().stop_requested());
        state.Disconnect();
        state.Poll(true, true);
        assert(!state.Token().stop_requested());
        state.Poll(false, false);
        state.Poll(true, false);
        assert(state.Token().stop_requested() && !state.Cancelled());
        state.Poll(false, false);
        state.Poll(false, true);
        assert(state.Cancelled()); // Cancel can upgrade a skip while draining.
        state.Skip();
        assert(state.Cancelled());
    }
    // Real coordinator/workers: early/middle/late stop retains actual counts,
    // joins every owner and never invokes progress after returning.
    for (const std::size_t cutoff : {1u, 40u, 95u}) {
        WarmupControl state;
        std::atomic<unsigned> active{}, processed{};
        std::size_t last_done{};
        unsigned callbacks{};
        const auto coordinator = std::this_thread::get_id();
        Core::JitPrewarm::RunOwners(
            {100, 100, 100, 0},
            [&](std::size_t, const Core::JitPrewarm::Progress& report) {
                ++active;
                for (std::size_t n = 0; n < 100 && !state.Token().stop_requested(); ++n) {
                    ++processed;
                    report(n + 1, 100);
                    if (n + 1 == cutoff) {
                        // Hold the exact stop point until the coordinator observes all owners.
                        while (!state.Token().stop_requested()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds{1});
                        }
                    }
                }
                --active;
            },
            [&](std::size_t done, std::size_t total) {
                assert(std::this_thread::get_id() == coordinator && total == 300);
                assert(done >= last_done && done <= total);
                last_done = done;
                ++callbacks;
                if (done >= cutoff * 3)
                    state.Skip();
            },
            state.Token());
        assert(state.Token().stop_requested() && active == 0);
        assert(last_done == processed && last_done == cutoff * 3);
        const auto saved = callbacks;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        assert(callbacks == saved);
    }
    // No request preserves the normal completion contract, including empty owners.
    {
        std::size_t done{}, total{};
        Core::JitPrewarm::RunOwners(
            {2, 3, 0, 0}, [](std::size_t, const Core::JitPrewarm::Progress&) {},
            [&](auto completed, auto planned) {
                done = completed;
                total = planned;
            });
        assert(done == 5 && total == 5);
        Core::JitPrewarm::RunOwners(
            {}, [](std::size_t, const Core::JitPrewarm::Progress&) { assert(false); },
            [](auto, auto) { assert(false); });
    }
    // Exceptions in the UI still join workers before captured state dies.
    std::atomic<unsigned> active{};
    try {
        Core::JitPrewarm::RunOwners(
            {1, 1, 0, 0},
            [&](std::size_t, const Core::JitPrewarm::Progress&) {
                ++active;
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                --active;
            },
            [](auto, auto) { throw 7; });
        assert(false);
    } catch (int) {
        assert(active == 0);
    }
    std::cout << "PASS: switches, session isolation, held input, skip/cancel, 3 stop points, "
                 "join/unwind\n";
}
