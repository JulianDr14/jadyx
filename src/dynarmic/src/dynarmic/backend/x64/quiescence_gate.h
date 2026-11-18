// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

namespace Dynarmic::Backend::X64 {

/// Lets threads that run shared JIT code agree on a moment when none of them is running it, so
/// the code memory can be reset. A thread Enters before running the code and Exits after; a clear
/// requested meanwhile runs when the last active thread Exits, and Enter waits until it is done.
///
/// Each completed clear bumps Generation(): a thread that sees it change across an Enter knows
/// every code pointer it cached (indirect-branch tables, return stack) is now dangling.
///
/// The clear runs without the gate's lock held, so it may take other locks; a thread holding a
/// lock the clear needs must not wait on the gate (Enter, or Exit followed by Enter).
class QuiescenceGate {
public:
    explicit QuiescenceGate(std::function<void()> clear)
        : clear{std::move(clear)} {}

    QuiescenceGate(const QuiescenceGate&) = delete;
    QuiescenceGate& operator=(const QuiescenceGate&) = delete;

    /// Waits while a clear is pending or running, then counts the caller as active.
    /// Returns the generation the caller enters in.
    std::uint64_t Enter() {
        std::unique_lock lock{mutex};
        idle.wait(lock, [this] { return !pending; });
        ++active;
        return generation.load(std::memory_order_relaxed);
    }

    /// Stops counting the caller. The last active thread out runs a pending clear.
    void Exit() {
        std::unique_lock lock{mutex};
        --active;
        if (active == 0 && pending && !clearing) {
            RunClear(lock);
        }
    }

    /// Asks for a clear. With no thread active it runs now, on the caller; otherwise the last
    /// thread to Exit runs it. Returns false when one was already pending.
    bool RequestClear() {
        std::unique_lock lock{mutex};
        if (pending) {
            return false;
        }
        pending = true;
        if (active == 0) {
            RunClear(lock);
        }
        return true;
    }

    [[nodiscard]] std::uint64_t Generation() const noexcept {
        return generation.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool ClearPending() const {
        std::scoped_lock lock{mutex};
        return pending;
    }

    [[nodiscard]] std::size_t Active() const {
        std::scoped_lock lock{mutex};
        return active;
    }

private:
    void RunClear(std::unique_lock<std::mutex>& lock) {
        clearing = true;
        lock.unlock();
        clear();
        lock.lock();
        generation.fetch_add(1, std::memory_order_release);
        clearing = false;
        pending = false;
        idle.notify_all();
    }

    mutable std::mutex mutex;
    std::condition_variable idle;
    std::size_t active = 0;
    bool pending = false;
    bool clearing = false;
    std::atomic<std::uint64_t> generation{0};
    std::function<void()> clear;
};

}  // namespace Dynarmic::Backend::X64
