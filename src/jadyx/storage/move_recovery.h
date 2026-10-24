// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>
#include "jadyx/storage/verified_move.h"

namespace Jadyx::VerifiedMove {
inline bool IsMoveStaging(std::string_view name) {
    return name.starts_with(".jadyx-move");
}

// Only incomplete staging is disposable. Published bundles and originals are never removed.
template <typename Names, typename Remove, typename Cancelled>
void CleanupStaging(const Names& names, Remove&& remove, Cancelled&& cancelled) {
    for (const auto& name : names) {
        if (cancelled())
            break;
        if (IsMoveStaging(name))
            remove(name);
    }
}

enum class ExistingCopy { Conflict, Verified, Cancelled };
// Recognize a previously published copy using the same full-size/hash verifier as a new move.
// Recognition never commits a catalog or deletes sources; recovery requires explicit review.
template <typename Verify, typename Cancelled>
ExistingCopy CheckPublished(const std::vector<File>& files, Verify&& verify,
                            Cancelled&& cancelled) {
    if (files.empty())
        return ExistingCopy::Conflict;
    for (const auto& file : files) {
        if (cancelled())
            return ExistingCopy::Cancelled;
        if (!verify(file))
            return cancelled() ? ExistingCopy::Cancelled : ExistingCopy::Conflict;
    }
    return cancelled() ? ExistingCopy::Cancelled : ExistingCopy::Verified;
}
} // namespace Jadyx::VerifiedMove
