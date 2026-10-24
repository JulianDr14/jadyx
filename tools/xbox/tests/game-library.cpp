// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include "jadyx/library/game_library.h"
#include "jadyx/storage/move_recovery.h"
#include "jadyx/storage/usb_discovery.h"

// Portable adapter for the shared USB traversal policy: enumerate 64-item
// pages, as StorageFolder does, without requiring WinRT or opening container
// contents.
Jadyx::LibraryScan ScanPagedFolders(const std::filesystem::path& root, std::stop_token stop = {},
                                    bool drive_root = false) {
    namespace fs = std::filesystem;
    Jadyx::LibraryScan result;
    Jadyx::StorageScanBudget budget{};
    Jadyx::ScanLibraryFolders(
        root, budget.visited, budget.limited, stop,
        [&](const fs::path& folder, auto visit, auto descend) {
            auto it = fs::directory_iterator{folder};
            const auto end = fs::directory_iterator{};
            while (it != end) {
                std::vector<fs::directory_entry> page;
                for (unsigned index = 0; index < 64 && it != end; ++index, ++it)
                    page.push_back(*it);
                for (const auto& item : page) {
                    if (!visit())
                        return;
                    const auto relative = item.path().lexically_relative(root);
                    if (Jadyx::VerifiedMove::IsMoveStaging(item.path().filename().string()))
                        continue;
                    if (item.is_symlink())
                        continue;
                    if (item.is_directory()) {
                        if (!drive_root || Jadyx::ShouldDescendUsbFolder(relative.generic_string()))
                            descend(item.path());
                    } else if (item.path().extension() == ".nsp" ||
                               item.path().extension() == ".xci") {
                        Jadyx::LibraryEntry entry{};
                        entry.relative_path = item.path().lexically_relative(root);
                        result.entries.push_back(std::move(entry));
                    }
                }
            }
        });
    result.limited = budget.limited;
    assert(budget.visited <= 10000);
    return result;
}

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("eden-library-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup { fs::path root; ~Cleanup() { fs::remove_all(root); } } cleanup{root};
    assert(Jadyx::ScanGameLibrary(root).entries.empty());
    assert(Jadyx::ScanGameLibrary(root / "missing").missing_root);
    fs::create_directory(root / "nested");
    for (const auto& name : {"b.XCI", "a.nsp", "ignored.nca", "partial.nsp.tmp"})
        std::ofstream{root / name};
    std::ofstream{root / "nested" / "homebrew.NRO"};
    const auto scan = Jadyx::ScanGameLibrary(root);
    assert(scan.entries.size() == 3 && scan.error.empty() && !scan.limited);
    assert(scan.entries[0].relative_path == "a.nsp");
    assert(scan.entries[1].relative_path == "b.XCI");
    assert(scan.entries[2].relative_path == fs::path{"nested"} / "homebrew.NRO");
    std::stop_source source;
    source.request_stop();
    assert(Jadyx::ScanGameLibrary(root, source.get_token()).entries.empty());
    auto deep = root;
    for (int i = 0; i < 6; ++i) { deep /= "level"; fs::create_directory(deep); }
    std::ofstream{deep / "too-deep.nsp"};
    assert(Jadyx::ScanGameLibrary(root).entries.size() == 3);
    std::error_code ec;
    fs::create_directory_symlink(root / "nested", root / "link", ec);
    if (!ec) assert(Jadyx::ScanGameLibrary(root).entries.size() == 3);
    // Put the noisy subtree in the first enumerated directory so this regression
    // is independent of filesystem ordering. Both games are shallower than the
    // noise.
    const auto crowded = root / "crowded";
    fs::create_directories(crowded / "deep");
    fs::create_directories(crowded / "b");
    fs::path noisy, games;
    for (const auto& item : fs::directory_iterator{crowded}) {
        if (noisy.empty())
            noisy = item.path();
        else
            games = item.path();
    }
    fs::create_directories(noisy / "x" / "y");
    for (unsigned index = 0; index < 20000; ++index)
        std::ofstream{noisy / "x" / "y" / (std::to_string(index) + ".txt")};
    std::ofstream{crowded / "a.nsp"};
    std::ofstream{games / "c.xci"};
    const auto check_crowded = [&] {
        for (const auto& limited : {Jadyx::ScanGameLibrary(crowded), ScanPagedFolders(crowded)}) {
            assert(limited.limited && limited.error.empty());
            assert(limited.entries.size() == 2);
            assert(std::any_of(limited.entries.begin(), limited.entries.end(),
                               [](const auto& entry) { return entry.relative_path == "a.nsp"; }));
            assert(
                std::any_of(limited.entries.begin(), limited.entries.end(), [&](const auto& entry) {
                    return entry.relative_path == (games / "c.xci").lexically_relative(crowded);
                }));
        }
    };
    check_crowded();
    // Also exercise the exact reported layout: a.nsp, b/c.xci, deep/x/y/*.
    if (noisy != crowded / "deep") {
        fs::rename(noisy / "x", crowded / "deep" / "x");
        fs::rename(games / "c.xci", crowded / "b" / "c.xci");
        games = crowded / "b";
        check_crowded();
    }
    assert(ScanPagedFolders(crowded, source.get_token()).entries.empty());
    // Exactly the cap is complete; only an additional actual entry marks it
    // limited.
    unsigned visited = 0;
    bool hit_limit = false;
    Jadyx::ScanLibraryFolders(0, visited, hit_limit, {}, [](int, auto visit, auto) {
        for (unsigned index = 0; index < 10000; ++index)
            assert(visit());
    });
    assert(visited == 10000 && !hit_limit);
    Jadyx::ScanLibraryFolders(0, visited, hit_limit, {},
                              [](int, auto visit, auto) { assert(!visit()); });
    assert(visited == 10000 && hit_limit);
    // Root entries are level 0; enumerate through level 4, never level 5.
    visited = 0;
    hit_limit = false;
    unsigned folders = 0;
    Jadyx::ScanLibraryFolders(0, visited, hit_limit, {}, [&](int level, auto visit, auto descend) {
        assert(level < 5 && visit());
        ++folders;
        descend(level + 1);
    });
    assert(folders == 5 && !hit_limit);
    // The integration5 whole-drive policy still rejects arbitrary subtrees,
    // while explicitly selected folders retain all five levels.
    const auto drive = root / "drive";
    for (const auto& directory : {"games/title", "NXbox/games", "other/sub"})
        fs::create_directories(drive / directory);
    for (const auto& file : {"root.nsp", "games/shallow.nsp", "games/title/deep.nsp",
                             "NXbox/games/allowed.xci", "other/sub/hidden.nsp"})
        std::ofstream{drive / file};
    const auto drive_scan = ScanPagedFolders(drive, {}, true);
    assert(drive_scan.entries.size() == 3 && !drive_scan.limited);
    assert(ScanPagedFolders(drive).entries.size() == 5);
    // Interrupted move files and directories never become launchable copies.
    fs::create_directories(drive / "games" / ".jadyx-move-test");
    std::ofstream{drive / "games" / ".jadyx-move-test" / "staged.nsp"};
    std::ofstream{drive / "games" / ".jadyx-move-test.nsp"};
    assert(ScanPagedFolders(drive).entries.size() == 5);
    assert(ScanPagedFolders(drive, {}, true).entries.size() == 3);
    // A capped source contributes its games without starving later sources.
    auto combined = ScanPagedFolders(crowded);
    Jadyx::AppendStorageScan(combined, ScanPagedFolders(drive, {}, true));
    assert(combined.limited && combined.entries.size() == 5);
    // A failed folder adapter returns locally; already queued siblings survive.
    visited = 0;
    hit_limit = false;
    std::vector<int> order;
    Jadyx::ScanLibraryFolders(
        0, visited, hit_limit, {}, [&](int folder_id, auto visit, auto descend) {
            order.push_back(folder_id);
            if (folder_id == 1)
                return; // Model the WinRT adapter catching a folder access error.
            assert(visit());
            if (folder_id == 0) {
                descend(1);
                descend(2);
            }
        });
    assert((order == std::vector<int>{0, 1, 2}) && !hit_limit);
    std::cout << "PASS library extensions, relative paths, nesting, "
                 "cancellation, empty/missing, "
                 "links, breadth-first native/paged scans, 20000-file noise, "
                 "budget and depth\n";
}
