// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace Dynarmic::Backend::X64 {
class A64CodeCache;
}  // namespace Dynarmic::Backend::X64

namespace Dynarmic::A64 {

/// One code cache for all the cores of a process: a block compiled on one core runs on every
/// core. Create one per process and set it in each core's UserConfig::shared_code_cache.
///
/// A core joins only when its configuration emits the same code as the cores already in (the
/// same page table, fastmem base, optimizations, callback type...) and the host can patch code
/// other threads are running; otherwise it keeps a private cache and Stats count it.
class SharedCodeCache {
public:
    struct Stats {
        std::atomic<std::uint64_t> blocks_compiled{0};
        /// Blocks a core translated and then found already compiled by another core.
        std::atomic<std::uint64_t> duplicate_translations{0};
        /// Blocks a core waited for another core to finish compiling instead of translating them.
        std::atomic<std::uint64_t> compiles_awaited{0};
        std::atomic<std::uint64_t> full_clears{0};
        std::atomic<std::uint32_t> cores{0};
        std::atomic<std::uint32_t> private_fallbacks{0};
    };

    /// `expected_cores` sizes the cache: the sum of that many cores' code_cache_size.
    explicit SharedCodeCache(std::size_t expected_cores)
        : expected_cores{expected_cores != 0 ? expected_cores : 1} {}

    SharedCodeCache(const SharedCodeCache&) = delete;
    SharedCodeCache& operator=(const SharedCodeCache&) = delete;

    [[nodiscard]] std::size_t ExpectedCores() const noexcept {
        return expected_cores;
    }

    [[nodiscard]] const Stats& GetStats() const noexcept {
        return *stats;
    }

private:
    friend class Backend::X64::A64CodeCache;

    const std::size_t expected_cores;
    const std::shared_ptr<Stats> stats = std::make_shared<Stats>();
    std::mutex backend_mutex;
    std::shared_ptr<void> backend;  ///< The backend's cache, created by the first core to join.
    bool backend_unavailable = false;  ///< The host cannot share code: every core keeps its own.
};

}  // namespace Dynarmic::A64
