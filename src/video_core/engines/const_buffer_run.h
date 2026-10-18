// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Tegra::Engines {

/// Maxwell's CB_DATA is sixteen registers that all do the same thing: store the word at
/// const_buffer.offset and advance the offset. A packet whose words all land on them is the same
/// as one block write, which is much cheaper than a method call per word.
///
/// Returns how many of the packet's next words can be written as one block, starting at `method`
/// with `method_count` words left in the packet and `available` of them in the current command
/// list; 0 when the packet is somewhere else or a single word would gain nothing.
/// `increment_once` is the first word of an IncreaseOnce packet: it goes to `method`, the rest to
/// `method + 1`.
[[nodiscard]] constexpr std::uint32_t ConstBufferRunLength(std::uint32_t first_cb_data,
                                                           std::uint32_t method,
                                                           std::uint32_t method_count,
                                                           std::size_t available,
                                                           bool increment_once) {
    constexpr std::uint32_t CB_DATA_REGISTERS = 16;
    if (method < first_cb_data || method >= first_cb_data + CB_DATA_REGISTERS) {
        return 0;
    }
    std::uint32_t words = static_cast<std::uint32_t>(
        std::min<std::size_t>(method_count, available));
    if (increment_once) {
        // Every word after the first repeats method + 1, which must still be CB_DATA.
        if (method + 1 >= first_cb_data + CB_DATA_REGISTERS) {
            words = std::min(words, 1U);
        }
    } else {
        words = std::min(words, first_cb_data + CB_DATA_REGISTERS - method);
    }
    return words >= 2 ? words : 0;
}

} // namespace Tegra::Engines
