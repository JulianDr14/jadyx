// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include "common/common_types.h"

namespace Common {

/// One repeated error site. A guest stuck reading unmapped memory logs once per access, which
/// buried a 100 MiB log in minutes: the first FIRST occurrences are logged, then one of each EVERY.
class LogThrottle {
public:
    static constexpr u64 FIRST = 64;
    static constexpr u64 EVERY = 65536;

    /// The occurrence number when this one is to be logged, 0 when it is dropped.
    u64 Next() {
        const u64 n = count.fetch_add(1, std::memory_order_relaxed) + 1;
        return n <= FIRST || n % EVERY == 0 ? n : 0;
    }

    /// The note to append to a logged occurrence: empty while nothing is dropped yet.
    static const char* Note(u64 n) {
        return n < FIRST ? "" : " (repeated: later ones are logged one in 65536)";
    }

private:
    std::atomic<u64> count{};
};

} // namespace Common
