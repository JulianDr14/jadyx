// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

/// Cheaper ways through the GPU thread's bookkeeping that give the same result as the plain ones,
/// behind one switch so a "hunt=1" package can compare them at run time (game menu):
///  - queued guest CPU writes that touch are applied as one range (VideoCommon::CpuWriteQueue);
///  - a uniform buffer bind does not take the buffer cache lock (only the GPU thread reads or
///    writes the bindings).
namespace VideoCore::FastPaths {

inline std::atomic<bool> enabled{true};

inline void SetEnabled(bool value) noexcept {
    enabled.store(value, std::memory_order_relaxed);
}

[[nodiscard]] inline bool Enabled() noexcept {
    return enabled.load(std::memory_order_relaxed);
}

} // namespace VideoCore::FastPaths
