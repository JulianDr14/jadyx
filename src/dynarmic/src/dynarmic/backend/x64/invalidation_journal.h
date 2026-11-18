// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Dynarmic::Backend::X64 {

/// Blocks invalidated in shared code that one core has not yet dropped from its own caches.
/// Any thread pushes; only the owning core drains, on its own thread, before it runs code again.
/// Past `capacity` entries the journal collapses into "drop everything", which is always correct.
class InvalidationJournal {
public:
    explicit InvalidationJournal(std::size_t capacity = 4096)
        : capacity{capacity} {
        pending.reserve(capacity);
        draining.reserve(capacity);
    }

    InvalidationJournal(const InvalidationJournal&) = delete;
    InvalidationJournal& operator=(const InvalidationJournal&) = delete;

    void Push(std::uint64_t descriptor) {
        std::scoped_lock lock{mutex};
        if (!overflowed) {
            if (pending.size() < capacity) {
                pending.push_back(descriptor);
            } else {
                overflowed = true;
                pending.clear();
            }
        }
        has_work.store(true, std::memory_order_release);
    }

    void PushDropAll() {
        std::scoped_lock lock{mutex};
        overflowed = true;
        pending.clear();
        has_work.store(true, std::memory_order_release);
    }

    /// Cheap check for the owner's fast path.
    [[nodiscard]] bool HasWork() const noexcept {
        return has_work.load(std::memory_order_acquire);
    }

    /// Applies what was pushed: `drop_all()` after an overflow, else `drop(descriptor)` for each.
    /// The callbacks run without the journal's lock. Returns whether there was anything.
    template<typename Drop, typename DropAll>
    bool Drain(Drop&& drop, DropAll&& drop_all) {
        if (!HasWork()) {
            return false;
        }
        bool all;
        {
            std::scoped_lock lock{mutex};
            all = overflowed;
            overflowed = false;
            draining.swap(pending);
            pending.clear();
            has_work.store(false, std::memory_order_relaxed);
        }
        if (all) {
            drop_all();
        } else {
            for (const std::uint64_t descriptor : draining) {
                drop(descriptor);
            }
        }
        draining.clear();
        return true;
    }

    /// Forgets everything pushed so far (the owner just dropped all its caches anyway).
    void Discard() {
        std::scoped_lock lock{mutex};
        overflowed = false;
        pending.clear();
        has_work.store(false, std::memory_order_relaxed);
    }

private:
    const std::size_t capacity;
    std::mutex mutex;
    std::vector<std::uint64_t> pending;
    std::vector<std::uint64_t> draining;  ///< Owner-only; swapped with `pending` to drain unlocked.
    bool overflowed = false;
    std::atomic_bool has_work{false};
};

}  // namespace Dynarmic::Backend::X64
