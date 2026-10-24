// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#undef NDEBUG
#include <cassert>
#include <future>
#include <iostream>
#include "common/thread_worker.h"

// Thread placement is irrelevant to this portable lifetime gate.
namespace Common {
void SetCurrentThreadName(const char*) {}
void SetCurrentThreadPriority(ThreadPriority) {}
void SetCurrentThreadToEfficiencyCores() {}
void SetCurrentThreadToBackgroundWork() {}
void SetCurrentThreadToAllCores() {}
} // namespace Common

int main() {
    using namespace std::chrono_literals;
    Common::ThreadWorker workers{1, "WarmupLifetime"};
    std::stop_source skip;
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    std::atomic<unsigned> callbacks{}, pending_builds{}, later_builds{};
    workers.QueueWork([&] {
        entered.set_value();
        released.wait(); // A non-interruptible driver call already in flight.
        ++callbacks;
    });
    entered.get_future().wait();
    for (unsigned n = 0; n < 20; ++n) {
        workers.QueueWork([&] {
            if (skip.stop_requested())
                return;
            ++pending_builds;
        });
    }
    skip.request_stop();
    auto drain = std::async(std::launch::async, [&] { workers.WaitForRequests(); });
    assert(drain.wait_for(20ms) == std::future_status::timeout);
    release.set_value();
    assert(drain.wait_for(2s) == std::future_status::ready);
    drain.get();
    assert(callbacks == 1 && pending_builds == 0);
    // The persistent pool still accepts on-demand gameplay work after a skip.
    workers.QueueWork([&] { ++later_builds; });
    workers.WaitForRequests();
    assert(later_builds == 1 && callbacks == 1);
    std::cout << "PASS: active compilation drains, 20 queued builds skipped, pool reusable\n";
}
