// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <map>
#include <string>
#include "jadyx/content/content_store.h"

namespace Jadyx {
// Reuse schema 1 external references. Disabled/older revisions and requirements survive.
// Embedded revisions still refer to the entire base container, not extracted fragments.
inline Content::Catalog RelocateGameCatalog(const Content::Catalog& catalog,
                                            std::uint64_t application, const std::string& old_base,
                                            const std::string& new_base, std::uint64_t base_bytes,
                                            const std::map<std::string, std::string>& objects) {
    auto next = catalog;
    for (auto& revision : next.revisions) {
        if (revision.application != application)
            continue;
        const auto found = objects.find(revision.id);
        if (revision.internal && found != objects.end()) {
            revision.internal = false;
            revision.source = found->second;
            revision.source_size = 0;  // InspectPackage's directory representation.
            revision.source_stamp = 0; // Force content verification at first launch.
        } else if (!revision.internal && revision.source == old_base) {
            revision.source = new_base;
            revision.source_size = base_bytes;
            revision.source_stamp = 0;
        }
    }
    return next;
}
} // namespace Jadyx
