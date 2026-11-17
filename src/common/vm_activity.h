// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

/// Diagnostic: which address-space operations (commits, decommits, protection changes, GPU heap
/// creation) are running right now and when each kind last finished. A fault elsewhere in the
/// process reads it to see what ran beside it. Lock-free and allocation-free, so an exception
/// handler may read it.
namespace Common::VmActivity {

enum class Kind : std::size_t {
    GuestCommit,
    GuestDecommit,
    GuestProtect,
    GpuHeap,
    Count,
};

inline constexpr std::size_t KindCount = static_cast<std::size_t>(Kind::Count);

struct KindState {
    std::atomic<std::uint32_t> in_flight{0};
    std::atomic<std::uint64_t> ops{0};
    std::atomic<std::int64_t> last_end_us{0};  ///< NowUs() when one last finished; 0 never.
};

inline std::array<KindState, KindCount> g_kinds{};

inline std::int64_t NowUs() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Marks one operation of `kind` for its lifetime.
class Scope {
public:
    explicit Scope(Kind kind) noexcept : state{g_kinds[static_cast<std::size_t>(kind)]} {
        state.in_flight.fetch_add(1, std::memory_order_relaxed);
    }
    ~Scope() {
        state.last_end_us.store(NowUs(), std::memory_order_relaxed);
        state.ops.fetch_add(1, std::memory_order_relaxed);
        state.in_flight.fetch_sub(1, std::memory_order_relaxed);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    KindState& state;
};

inline const char* Name(std::size_t kind) noexcept {
    constexpr const char* names[KindCount] = {"guest commit", "guest decommit", "guest protect",
                                              "GPU heap"};
    return kind < KindCount ? names[kind] : "?";
}

} // namespace Common::VmActivity
