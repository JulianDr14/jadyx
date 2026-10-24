// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "jadyx/storage/storage_path.h"

namespace Jadyx::VerifiedMove {
// Adapted from NXbox's GPL-3.0 frontend_common/library_transfer.h (rc/next).
// The broker owns paths/handles. Verification must compare exact size AND SHA-256.
struct File {
    std::string relative;
    std::uint64_t bytes{};
};
enum class Result {
    Success,
    Invalid,
    Space,
    Cancelled,
    Copy,
    Verification,
    Publication,
    Registry,
    CleanupIncomplete
};
inline constexpr std::uint64_t Reserve = 64ULL << 20;
struct IO {
    std::function<bool(const File&)> copy;
    std::function<bool(const File&)> verify;
    std::function<bool()> publish;
    std::function<bool()> commit;
    std::function<bool(const File&)> remove;
    std::function<bool()> cancelled;
};
// No deletion until the ENTIRE group is verified, published, reread and registered.
// Publication/registry failures keep originals and leave any published copy for recovery.
// After commit cancellation cannot roll back the catalog; cleanup is best effort.
inline Result Run(const std::vector<File>& files, std::optional<std::uint64_t> available,
                  const IO& io) {
    if (files.empty())
        return Result::Invalid;
    std::uint64_t required = Reserve;
    std::set<std::string> names;
    for (const auto& file : files) {
        auto folded = file.relative;
        std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) {
            return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        });
        if (file.relative.empty() || !ValidStorageRelative(file.relative) ||
            !names.insert(folded).second ||
            file.bytes > (std::numeric_limits<std::uint64_t>::max)() - required)
            return Result::Invalid;
        required += file.bytes;
    }
    if (!available || *available < required)
        return Result::Space;
    for (const auto& file : files) {
        if (io.cancelled())
            return Result::Cancelled;
        if (!io.copy(file))
            return io.cancelled() ? Result::Cancelled : Result::Copy;
    }
    for (const auto& file : files) {
        if (io.cancelled())
            return Result::Cancelled;
        if (!io.verify(file))
            return io.cancelled() ? Result::Cancelled : Result::Verification;
    }
    if (io.cancelled())
        return Result::Cancelled;
    if (!io.publish())
        return Result::Publication;
    for (const auto& file : files) {
        if (io.cancelled())
            return Result::Cancelled;
        if (!io.verify(file))
            return io.cancelled() ? Result::Cancelled : Result::Verification;
    }
    if (io.cancelled())
        return Result::Cancelled;
    if (!io.commit())
        return Result::Registry;
    for (const auto& file : files)
        if (!io.remove(file))
            return Result::CleanupIncomplete;
    return Result::Success;
}
} // namespace Jadyx::VerifiedMove
