// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <string>

#include "common/common_types.h"

/// Ring of the latest GPU semaphore and report writes, for the freeze report: a guest thread
/// spinning on a counter the GPU should have bumped shows whether the value was ever requested
/// and whether it reached memory. Entries are matched by host pointer, the one address both the
/// guest CPU view and the GPU view share.
namespace Common::SemaphoreTrace {

enum class Stage : u8 {
    Requested, ///< The GPU command was processed; the write may wait for a fence.
    Written,   ///< The value was stored in guest memory.
};

void Record(Stage stage, u64 gpu_address, const void* host, u64 payload, u32 type, u32 flags);

/// The newest `last` entries plus every entry still in the ring whose host pointer is in `watch`.
std::string Describe(std::span<const void* const> watch, std::size_t last);

} // namespace Common::SemaphoreTrace
