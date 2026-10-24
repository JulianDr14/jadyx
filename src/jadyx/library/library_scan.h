// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <deque>
#include <stop_token>
#include <utility>

namespace Jadyx {
// Both native and brokered folders use this order and budget. Enumerate calls visit
// before inspecting each item, and descend for each child folder. Never open game data.
template <typename Folder, typename Enumerate>
void ScanLibraryFolders(Folder root, unsigned& visited, bool& limited, std::stop_token stop,
                        Enumerate enumerate) {
    struct Pending {
        Folder folder;
        unsigned depth{};
    };
    std::deque<Pending> pending;
    pending.push_back({.folder = std::move(root)});
    while (!pending.empty() && !limited && !stop.stop_requested()) {
        auto current = std::move(pending.front());
        pending.pop_front();
        enumerate(
            current.folder,
            [&] {
                if (stop.stop_requested())
                    return false;
                if (visited >= 10000) {
                    limited = true;
                    return false;
                }
                ++visited;
                return true;
            },
            [&](Folder child) {
                if (current.depth < 4) {
                    pending.push_back({.folder = std::move(child), .depth = current.depth + 1});
                }
            });
    }
}
} // namespace Jadyx
