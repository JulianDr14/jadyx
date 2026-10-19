// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "common/pulse_archive.h"

// Pulse's own version, apart from the emulator's: a new build of the emulator keeps the sessions
// of the builds before it as long as this version says they still read the same.
//   patch: a change that leaves what a session holds as it was (a service serializer added, a
//          fix in a rejection). Sessions of the same major.minor resume, whatever their patch.
//   minor/major: anything that changes what a session holds or how it is read back (a section's
//          or a service state's layout, a kernel object's fields, the archive). Every session of
//          an older major.minor is discarded.
// Bump it in the same change that touches the capture or the restore (core/pulse_*, the
// services' PulseCapture/PulseRestore, common/pulse_*): the emulator's version no longer does.
namespace Common::Pulse {

struct Version {
    std::uint32_t major{};
    std::uint32_t minor{};
    std::uint32_t patch{};
    bool operator==(const Version&) const = default;
};

// 1.1: blocks are stored with zstd, which a 1.0 build cannot read.
inline constexpr Version CurrentVersion{1, 1, 0};

namespace Detail {
// Marks Identity::revision as a version, not the build hash older sessions stored there.
inline constexpr std::array<std::uint8_t, 8> VersionTag{'J', 'D', 'X', 'P', 'U', 'L', 'S', 'E'};

constexpr void Put32(std::array<std::uint8_t, 32>& out, std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) out[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
constexpr std::uint32_t Get32(const std::array<std::uint8_t, 32>& in, std::size_t at) {
    std::uint32_t value{};
    for (std::size_t i = 0; i < 4; ++i) value |= std::uint32_t{in[at + i]} << (8 * i);
    return value;
}
} // namespace Detail

/// Identity::revision for a version: the tag, major, minor and patch, then zeros.
[[nodiscard]] constexpr std::array<std::uint8_t, 32> RevisionOf(Version version) {
    std::array<std::uint8_t, 32> revision{};
    for (std::size_t i = 0; i < Detail::VersionTag.size(); ++i) revision[i] = Detail::VersionTag[i];
    Detail::Put32(revision, 8, version.major);
    Detail::Put32(revision, 12, version.minor);
    Detail::Put32(revision, 16, version.patch);
    return revision;
}

/// The version a revision holds; none for a build hash or anything else.
[[nodiscard]] constexpr std::optional<Version> VersionOf(const std::array<std::uint8_t, 32>& revision) {
    for (std::size_t i = 0; i < Detail::VersionTag.size(); ++i) {
        if (revision[i] != Detail::VersionTag[i]) return std::nullopt;
    }
    for (std::size_t i = 20; i < revision.size(); ++i) {
        if (revision[i] != 0) return std::nullopt;
    }
    return Version{Detail::Get32(revision, 8), Detail::Get32(revision, 12),
                   Detail::Get32(revision, 16)};
}

/// Whether a session made at `saved` resumes in a build at `current`.
[[nodiscard]] constexpr bool Compatible(Version saved, Version current) {
    return saved.major == current.major && saved.minor == current.minor;
}

/// Whether a stored session resumes as `current`: the same game, user and save data, and a
/// revision this build reads (its own, or a compatible Pulse version).
[[nodiscard]] constexpr bool Compatible(const Identity& saved, const Identity& current) {
    if (saved.program_id != current.program_id || saved.user != current.user ||
        saved.content != current.content || saved.savedata != current.savedata) {
        return false;
    }
    if (saved.revision == current.revision) return true;
    const auto saved_version = VersionOf(saved.revision);
    const auto current_version = VersionOf(current.revision);
    return saved_version && current_version && Compatible(*saved_version, *current_version);
}

} // namespace Common::Pulse
