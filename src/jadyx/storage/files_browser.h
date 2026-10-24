// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
#include "jadyx/storage/storage_path.h"

namespace Jadyx {
inline constexpr unsigned FilesPageSize = 64;
enum class FilesError { None, Unavailable, Permission };
struct FilesLocation {
    std::string token; // Empty identifies LocalState; otherwise an existing broker token.
    std::string relative;
};
struct FilesEntry {
    std::wstring name;
    std::string component;
    std::string token; // Root entries only.
    bool folder{};
    std::uint64_t size{};
};
struct FilesPage {
    std::vector<FilesEntry> entries;
    bool more{};
    bool sources_unavailable{}; // Root catalog failed; internal storage remains usable.
    FilesError error{};
};
inline std::optional<FilesLocation> FilesChild(const FilesLocation& parent,
                                               std::string_view component) {
    if (component.empty() || component.find('/') != std::string_view::npos ||
        !ValidStorageRelative(component) || !ValidStorageRelative(parent.relative))
        return {};
    FilesLocation child = parent;
    if (!child.relative.empty())
        child.relative += '/';
    child.relative += component;
    return child;
}
struct FilesBrowser {
    bool open{}, busy{}, roots{true};
    FilesLocation location;
    std::wstring root_name;
    unsigned offset{};
    std::size_t selected{};
    FilesPage page;
    void Apply(FilesPage result) {
        if (result.error != FilesError::None) {
            result.entries.clear();
            result.more = false;
        }
        page = std::move(result);
        selected = 0;
        busy = false;
    }
    bool Enter(std::size_t index) {
        if (busy || index >= page.entries.size() || !page.entries[index].folder)
            return false;
        const auto& entry = page.entries[index];
        if (roots) {
            location = {};
            location.token = entry.token;
            root_name = entry.name;
            roots = false;
        } else {
            const auto child = FilesChild(location, entry.component);
            if (!child)
                return false;
            location = *child;
        }
        offset = 0;
        return true;
    }
    bool Back() {
        if (roots)
            return false;
        if (location.relative.empty()) {
            roots = true;
            root_name.clear();
            location = {};
        } else {
            const auto slash = location.relative.rfind('/');
            location.relative =
                slash == std::string::npos ? "" : location.relative.substr(0, slash);
        }
        offset = 0;
        return true;
    }
    bool NextPage() {
        if (busy || roots || !page.more ||
            offset > std::numeric_limits<unsigned>::max() - FilesPageSize)
            return false;
        offset += FilesPageSize;
        return true;
    }
    bool PreviousPage() {
        if (busy || roots || offset == 0)
            return false;
        offset = offset >= FilesPageSize ? offset - FilesPageSize : 0;
        return true;
    }
};
// Read-only broker access, called only on a library MTA worker.
FilesPage ReadFilesPage(bool roots, const FilesLocation& location, unsigned offset,
                        std::stop_token stop);
} // namespace Jadyx
