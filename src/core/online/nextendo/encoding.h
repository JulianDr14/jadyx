// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Small encoders shared by the Nextendo modules (JWT, PKCE, signed catalogs).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Core::Online::Nextendo {

/// RFC 4648 section 5, no padding.
std::string Base64UrlEncode(std::span<const std::uint8_t> data);
std::string Base64UrlEncode(std::string_view text);

/// Standard alphabet with padding ("+/" and "="). Whitespace is skipped; anything else fails.
std::optional<std::vector<std::uint8_t>> Base64Decode(std::string_view text);

/// Lower-case hex.
std::string HexEncode(std::span<const std::uint8_t> data);

/// SHA-256 through OpenSSL.
std::vector<std::uint8_t> Sha256(std::span<const std::uint8_t> data);

/// Cryptographically secure random bytes (OpenSSL RAND_bytes). Empty on failure.
std::vector<std::uint8_t> RandomBytes(std::size_t count);

} // namespace Core::Online::Nextendo
