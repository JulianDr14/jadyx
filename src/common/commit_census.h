// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include "common/common_types.h"

namespace Common {

/// Committed bytes of the process's address space by kind, from one walk of it.
struct AddressSpaceCommit {
    u64 private_bytes{}; ///< MEM_PRIVATE: heaps, the DRAM's private backing, JIT, CPU-mapped GPU.
    u64 mapped_bytes{};  ///< MEM_MAPPED: section views (a file-backed DRAM, shared memory).
    u64 image_bytes{};   ///< MEM_IMAGE: the executable and DLLs.
    u64 regions{};       ///< Committed regions visited.
    u64 walk_us{};       ///< How long the walk took.
};

/// Walks the whole address space with VirtualQuery. Empty off Windows.
[[nodiscard]] AddressSpaceCommit WalkAddressSpaceCommit();

/// The app's commit split by what the address space holds: private, sections and images, and the
/// commit no view of the process accounts for, which the GPU driver and the kernel hold for it.
/// Images are left out of that difference (most of their pages share the file, not the commit).
[[nodiscard]] std::string DescribeCommitCensus(const AddressSpaceCommit& space, u64 app_commit);

} // namespace Common
