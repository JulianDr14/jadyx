// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/file_sys/common_funcs.h"
#include "jadyx/library/game_library.h"
#include "jadyx/library/library_cache.h"

namespace Jadyx {
// Borrow presentation only. An accessory keeps its own content identity and version;
// displaying a base cover does not turn it into a launchable application.
inline bool BorrowBaseMetadata(LibraryCacheRecord& record, const LibraryCacheRecord& base) {
    if (!record.program_id || record.program_id == base.program_id ||
        FileSys::GetBaseTitleID(record.program_id) != base.program_id || base.name.empty() ||
        base.icon.empty() || base.icon.size() > LibraryCache::MaxIcon ||
        (!record.name.empty() && !record.icon.empty()))
        return false;
    record.name = base.name;
    record.developer = base.developer;
    record.icon = base.icon;
    return true;
}
// Names-only probes never erase displayed art. A background fill publishes its icon
// immediately if this entry is still visible; off-screen bytes die with the batch.
inline bool MergeLibraryMetadata(LibraryEntry& target, LibraryEntry&& result, bool visible,
                                 bool icon_batch) {
    if (!result.title_loaded)
        return false;
    target.metadata_retry = result.metadata_retry;
    target.name = std::move(result.name);
    target.developer = std::move(result.developer);
    target.display_version = std::move(result.display_version);
    target.program_id = result.program_id;
    target.build_id = result.build_id;
    target.title_loaded = true;
    if (!visible || (!icon_batch && result.icon.empty()))
        return false;
    const bool changed = target.icon != result.icon;
    target.icon = std::move(result.icon);
    return changed;
}
} // namespace Jadyx
