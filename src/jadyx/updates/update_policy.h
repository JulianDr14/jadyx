// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
namespace Jadyx::Updates {
using Version = std::array<std::uint16_t, 4>;
std::optional<Version> ParseVersion(std::string_view text);
std::string FormatVersion(const Version& version);
enum class Channel { Stable, Preview };
struct Release {
    Version version{};
    Channel channel{};
    std::string url, sha256, notes, published, tag;
    std::uint64_t size{};
};
// Signature covers the exact UTF-8 bytes, not a reserialized JSON object.
std::optional<Release> VerifyManifest(std::string_view body, std::string_view signature,
                                      std::string_view public_pem, const Version& installed,
                                      Channel channel);
} // namespace Jadyx::Updates
