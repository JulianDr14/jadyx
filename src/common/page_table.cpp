// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>

#include "common/page_table.h"
#include "common/scope_exit.h"

namespace Common {

namespace {
std::atomic<PageTable::JitReadTable> jit_read_table{PageTable::JitReadTable::Off};
} // Anonymous namespace

void PageTable::SetJitReadTable(JitReadTable mode) noexcept {
    jit_read_table.store(mode, std::memory_order_relaxed);
}

PageTable::JitReadTable PageTable::GetJitReadTable() noexcept {
    return jit_read_table.load(std::memory_order_relaxed);
}

PageTable::PageTable() = default;

PageTable::~PageTable() noexcept = default;

void PageTable::Resize(std::size_t address_space_width_in_bits, std::size_t page_bits) {
    auto const num_page_table_entries = 1ULL << (address_space_width_in_bits - page_bits);
    entries.ResizeAndClear(num_page_table_entries);
    // jit_read_table: the load table follows the store table. Reserved, not committed: only the
    // pages of mapped memory take host memory.
    const bool read_table = GetJitReadTable() != JitReadTable::Off;
    jit_entries.ResizeAndClear(read_table ? 2 * num_page_table_entries : num_page_table_entries);
    jit_read_base = read_table ? num_page_table_entries : 0;
    current_address_space_width_in_bits = address_space_width_in_bits;
    current_page_bits = page_bits;
}

} // namespace Common
