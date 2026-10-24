// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include "jadyx/storage/ftp/ftp_protocol.h"

namespace Jadyx::Ftp {
// Supplied by the library, using the same paths as its importers. Never expose LocalState itself.
struct Roots {
    std::filesystem::path games;
    std::filesystem::path keys;
    std::filesystem::path firmware;
    std::filesystem::path saves;
};
inline constexpr std::array<std::string_view, 4> RootNames{"games", "keys", "firmware", "saves"};
struct MappedPath {
    std::filesystem::path root;
    std::string relative;
};
inline std::optional<MappedPath> MapPath(const Roots& roots, std::string_view path) {
    const auto normalized = NormalizePath("", path);
    if (!normalized || *normalized != path)
        return std::nullopt;
    // A virtual root has no physical backing; this placeholder is only used for CWD / and LIST /.
    if (path.empty()) {
        MappedPath result;
        result.root = roots.games;
        return result;
    }
    const auto slash = path.find('/');
    const auto alias = path.substr(0, slash);
    const std::array<const std::filesystem::path*, 4> directories{&roots.games, &roots.keys,
                                                                  &roots.firmware, &roots.saves};
    for (std::size_t index = 0; index < RootNames.size(); ++index) {
        if (alias != RootNames[index])
            continue;
        MappedPath result;
        result.root = *directories[index];
        if (slash != path.npos)
            result.relative = path.substr(slash + 1);
        return result;
    }
    return std::nullopt;
}
} // namespace Jadyx::Ftp
