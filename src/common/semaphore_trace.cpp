// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <vector>

#include <fmt/format.h>

#include "common/semaphore_trace.h"

namespace Common::SemaphoreTrace {
namespace {

struct Entry {
    u64 time_ms{};
    u64 gpu_address{};
    const void* host{};
    u64 payload{};
    u32 type{};
    u32 flags{};
    Stage stage{};
};

constexpr std::size_t Capacity = 1024;

std::mutex g_mutex;
std::array<Entry, Capacity> g_ring; ///< Under g_mutex.
u64 g_count{};                      ///< Under g_mutex; total entries ever recorded.

u64 NowMs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

std::string Format(const Entry& entry, u64 now_ms, bool watched) {
    return fmt::format("  {}{} {} ms ago gpu {:x} host {} value {:x} type {} flags {:#x}\n",
                       watched ? "* " : "  ",
                       entry.stage == Stage::Written ? "written  " : "requested",
                       now_ms - entry.time_ms, entry.gpu_address, entry.host, entry.payload,
                       entry.type, entry.flags);
}

} // namespace

void Record(Stage stage, u64 gpu_address, const void* host, u64 payload, u32 type, u32 flags) {
    const u64 now = NowMs();
    std::scoped_lock lk{g_mutex};
    g_ring[g_count % Capacity] = {now, gpu_address, host, payload, type, flags, stage};
    ++g_count;
}

std::string Describe(std::span<const void* const> watch, std::size_t last) {
    std::array<Entry, Capacity> ring;
    u64 count;
    {
        std::scoped_lock lk{g_mutex};
        ring = g_ring;
        count = g_count;
    }
    const u64 now = NowMs();
    const u64 held = std::min<u64>(count, Capacity);
    const auto at = [&](u64 i) -> const Entry& { return ring[(count - held + i) % Capacity]; };
    const auto watched = [&](const Entry& entry) {
        // A spinning thread may poll a field inside the written record, so allow 16 bytes.
        const auto* host = static_cast<const u8*>(entry.host);
        return host != nullptr && std::ranges::any_of(watch, [host](const void* p) {
                   const auto* q = static_cast<const u8*>(p);
                   return q != nullptr && q >= host - 15 && q <= host + 15;
               });
    };
    std::string text = fmt::format(" gpu semaphore writes: {} in total, {} held\n", count, held);
    // The newest matches only: a per-frame counter would otherwise fill the report.
    constexpr std::size_t MaxMatched = 12;
    std::vector<u64> matched;
    for (u64 i = 0; i < held; ++i) {
        if (watched(at(i))) {
            matched.push_back(i);
        }
    }
    if (matched.empty()) {
        text += "  none at an address a spinning core's registers point at\n";
    } else {
        text += fmt::format("  {} at addresses a spinning core's registers point at, newest {}:\n",
                            matched.size(), std::min(matched.size(), MaxMatched));
        for (std::size_t j = matched.size() - std::min(matched.size(), MaxMatched);
             j < matched.size(); ++j) {
            text += Format(at(matched[j]), now, true);
        }
    }
    text += fmt::format("  newest {}:\n", std::min<u64>(last, held));
    for (u64 i = held - std::min<u64>(last, held); i < held; ++i) {
        text += Format(at(i), now, watched(at(i)));
    }
    return text;
}

} // namespace Common::SemaphoreTrace
