// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace Jadyx::Ftp {
inline constexpr std::uint32_t BufferSize = 1024 * 1024;

// A single bounded buffer for every transfer. No file size is narrowed to a socket/block size.
// With a known size, early EOF is an error. Uploads finish at the data stream's orderly EOF.
// read/write/active are injected so the exact streaming loop can run in host tests without WinRT.
template <typename Read, typename Write, typename Active>
std::uint64_t CopyStream(Read read, Write write, Active active,
                         std::uint64_t remaining = std::numeric_limits<std::uint64_t>::max()) {
    const bool until_eof = remaining == std::numeric_limits<std::uint64_t>::max();
    std::vector<std::uint8_t> buffer(BufferSize);
    std::uint64_t total{};
    while (remaining != 0) {
        if (!active())
            throw std::runtime_error("transfer cancelled");
        const auto requested =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        const auto count = read(std::span<std::uint8_t>{buffer.data(), requested});
        if (count > requested)
            throw std::runtime_error("invalid read count");
        if (count == 0) {
            if (!until_eof)
                throw std::runtime_error("unexpected end of file");
            break;
        }
        if (!active())
            throw std::runtime_error("transfer cancelled");
        if (write(std::span<const std::uint8_t>{buffer.data(), count}) != count)
            throw std::runtime_error("transfer write failed");
        if (total > std::numeric_limits<std::uint64_t>::max() - count)
            throw std::runtime_error("transfer byte counter overflow");
        total += count;
        if (!until_eof)
            remaining -= count;
    }
    if (!active())
        throw std::runtime_error("transfer cancelled");
    return total;
}
} // namespace Jadyx::Ftp
