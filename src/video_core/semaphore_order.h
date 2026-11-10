// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "common/common_types.h"

/// Order of the semaphore and report writes the GPU makes to guest memory. Some of them wait for a
/// host fence (a query result to flush first) while later ones run at once, so they can reach guest
/// memory out of order: an older semaphore value then overwrites a newer one, the guest counter goes
/// backwards and a thread polling it for the newer value spins forever.
namespace VideoCore::SemaphoreOrder {

enum class Mode : u8 {
    /// A late write to an address a newer write already reached is dropped. Nothing waits; per
    /// address the guest only sees values move forward (writes to different addresses keep no
    /// order between them, as in other emulators).
    Supersede,
    /// Every operation waits while an earlier one still waits for its fence: the hardware order,
    /// at the cost of holding fast writes behind a GPU wait (up to a frame).
    Strict,
};

inline std::atomic<Mode> mode{Mode::Supersede};

inline Mode GetMode() {
    return mode.load(std::memory_order_relaxed);
}

inline void SetMode(Mode value) {
    mode.store(value, std::memory_order_relaxed);
}

/// Tickets per address for the writes in flight. Request() on the GPU thread in command order;
/// Write() wherever the write runs (at once or on fence release).
class Tracker {
public:
    u64 Request(u64 address) {
        std::scoped_lock lock{mutex};
        ++slots[address].in_flight;
        return ++last_ticket;
    }

    /// Runs `write` unless a newer ticket already wrote the address; returns whether it ran. The
    /// write runs under the lock so two late writes cannot interleave their check and store.
    template <typename Func>
    bool Write(u64 address, u64 ticket, Func&& write) {
        std::scoped_lock lock{mutex};
        const auto it = slots.find(address);
        if (it == slots.end()) {
            std::forward<Func>(write)();
            return true;
        }
        Slot& slot = it->second;
        const bool newest = ticket > slot.written;
        if (newest) {
            slot.written = ticket;
            std::forward<Func>(write)();
        }
        if (--slot.in_flight == 0) {
            slots.erase(it); // only addresses with writes in flight are kept
        }
        return newest;
    }

private:
    struct Slot {
        u32 in_flight{};
        u64 written{};
    };

    std::mutex mutex;
    std::unordered_map<u64, Slot> slots; ///< Under mutex.
    u64 last_ticket{};                   ///< Under mutex.
};

} // namespace VideoCore::SemaphoreOrder
