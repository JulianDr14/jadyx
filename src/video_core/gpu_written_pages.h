// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#include "common/common_types.h"

/// Device memory the GPU may have written and a cache may still have to download or flush:
/// images that became GpuModified, buffer ranges marked GPU modified and registered queries set
/// their 64 KiB granules here. Bits are never cleared, so a set bit only means "take the safe
/// path"; a clear bit means no cache holds GPU data for that memory. Pushbuffer reads
/// (DmaPusher::Step) skip their FlushRegion when none of their granules is set
/// ("dma_flush_filter=1"), without taking the buffer and texture cache locks.
namespace VideoCommon::GpuWrittenPages {

inline constexpr u64 GRANULE_BITS = 16;
inline constexpr u64 ADDRESS_BITS = 34; // MaxwellDeviceMemoryManager's device_virtual_bits
inline constexpr u64 GRANULES = u64{1} << (ADDRESS_BITS - GRANULE_BITS);
inline constexpr size_t WORDS = static_cast<size_t>(GRANULES / 64);

inline std::atomic<bool> enabled{};
inline std::array<std::atomic<u64>, WORDS> bits{};

/// Called with the granules of a Mark that set new bits (Tegra::Host1x: the JIT load table).
using NewlyMarkedHook = void (*)(void* context, u64 addr, u64 size);
inline std::atomic<NewlyMarkedHook> newly_marked_hook{};
inline std::atomic<void*> newly_marked_context{};

inline void SetNewlyMarkedHook(NewlyMarkedHook hook, void* context) noexcept {
    newly_marked_context.store(context, std::memory_order_relaxed);
    newly_marked_hook.store(hook, std::memory_order_release);
}

inline void SetEnabled(bool value) noexcept {
    enabled.store(value, std::memory_order_relaxed);
}

[[nodiscard]] inline bool Enabled() noexcept {
    return enabled.load(std::memory_order_relaxed);
}

/// Calls func(word, mask) for each word holding granules of [addr, addr + size).
template <typename Func>
inline bool ForEachWord(u64 addr, u64 size, Func&& func) noexcept {
    if (size == 0) {
        return false;
    }
    const u64 first = addr >> GRANULE_BITS;
    if (first >= GRANULES) {
        return false;
    }
    u64 last = (addr + size - 1) >> GRANULE_BITS;
    if (last >= GRANULES) {
        last = GRANULES - 1;
    }
    for (u64 word = first / 64; word <= last / 64; ++word) {
        const u64 lo = word == first / 64 ? first % 64 : 0;
        const u64 hi = word == last / 64 ? last % 64 : 63;
        const u64 mask = (hi - lo == 63 ? ~u64{0} : ((u64{1} << (hi - lo + 1)) - 1)) << lo;
        if (func(bits[static_cast<size_t>(word)], mask)) {
            return true;
        }
    }
    return false;
}

inline void Mark(u64 addr, u64 size) noexcept {
    if (!Enabled()) {
        return;
    }
    bool newly{};
    ForEachWord(addr, size, [&newly](std::atomic<u64>& word, u64 mask) {
        if ((word.load(std::memory_order_relaxed) & mask) != mask) {
            word.fetch_or(mask, std::memory_order_relaxed);
            newly = true;
        }
        return false;
    });
    if (!newly) {
        return;
    }
    if (const NewlyMarkedHook hook = newly_marked_hook.load(std::memory_order_acquire)) {
        // Every granule of the range is marked now.
        const u64 begin = (addr >> GRANULE_BITS) << GRANULE_BITS;
        const u64 end = ((addr + size - 1) >> GRANULE_BITS) + 1;
        hook(newly_marked_context.load(std::memory_order_relaxed), begin,
             (end << GRANULE_BITS) - begin);
    }
}

[[nodiscard]] inline bool AnyMarked(u64 addr, u64 size) noexcept {
    return ForEachWord(addr, size, [](std::atomic<u64>& word, u64 mask) {
        return (word.load(std::memory_order_relaxed) & mask) != 0;
    });
}

} // namespace VideoCommon::GpuWrittenPages
