// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>
#include <openssl/evp.h>

namespace Jadyx::VerifiedMove {
using Digest = std::array<unsigned char, 32>;
using Reader = std::function<std::size_t(std::span<unsigned char>, std::uint64_t)>;
// Independent reread, bounded memory, 64-bit offsets and exact EOF. An I/O failure is
// never represented by an all-zero digest. The caller additionally checks storage size.
inline std::optional<Digest> Hash(const Reader& read, std::uint64_t bytes,
                                  std::stop_token stop = {}) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free};
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        return {};
    std::vector<unsigned char> buffer(1024 * 1024);
    for (std::uint64_t offset = 0; offset < bytes;) {
        if (stop.stop_requested())
            return {};
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), bytes - offset));
        if (read(std::span{buffer}.first(count), offset) != count ||
            EVP_DigestUpdate(context.get(), buffer.data(), count) != 1)
            return {};
        offset += count;
    }
    if (stop.stop_requested() || read(std::span{buffer}.first(1), bytes) != 0)
        return {};
    Digest result{};
    unsigned length{};
    if (EVP_DigestFinal_ex(context.get(), result.data(), &length) != 1 || length != result.size())
        return {};
    return result;
}
} // namespace Jadyx::VerifiedMove
