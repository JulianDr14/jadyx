// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <iostream>
#include "jadyx/library/library_metadata_policy.h"

int main() {
    using namespace Jadyx;
    constexpr std::uint64_t application = 0x0100123400002000ULL;
    for (std::uint64_t suffix = 0; suffix < 0x2000; ++suffix) {
        assert(FileSys::GetBaseTitleID(application + suffix) == application);
        assert(FileSys::GetBaseTitleID(application + 0x2000 + suffix) != application);
    }
    LibraryCacheRecord base;
    base.program_id = application;
    base.name = L"Synthetic base";
    base.icon = {1, 2, 3};
    base.display_version = L"1.0";
    for (const auto suffix : {0x800ULL, 0x1001ULL, 0x1FFFULL}) {
        LibraryCacheRecord accessory;
        accessory.program_id = application + suffix;
        accessory.display_version = L"2.0";
        accessory.build_id[0] = 42;
        assert(BorrowBaseMetadata(accessory, base));
        assert(accessory.name == base.name && accessory.icon == base.icon);
        assert(accessory.program_id == application + suffix);
        assert(accessory.display_version == L"2.0" && accessory.build_id[0] == 42);
        assert(!BorrowBaseMetadata(accessory, base));
    }
    LibraryCacheRecord unrelated;
    unrelated.program_id = application + 0x2800;
    assert(!BorrowBaseMetadata(unrelated, base));
    unrelated.program_id = 0;
    assert(!BorrowBaseMetadata(unrelated, base));
    unrelated.program_id = application + 0x800;
    base.icon.clear();
    assert(!BorrowBaseMetadata(unrelated, base));
    LibraryEntry failed;
    failed.launch_path = "failed.nsp";
    failed.name = L"Partial";
    failed.title_loaded = true;
    failed.metadata_retry = true;
    LibraryEntry fresh;
    fresh.launch_path = failed.launch_path;
    std::vector<LibraryEntry> scanned{fresh};
    KeepKnownTitles({failed}, scanned);
    assert(!scanned.front().title_loaded);
    failed.metadata_retry = false;
    KeepKnownTitles({failed}, scanned);
    assert(scanned.front().title_loaded);
    LibraryEntry tile;
    tile.name = L"Raw filename";
    LibraryEntry background;
    background.name = L"Control title";
    background.title_loaded = true;
    background.icon = {4, 5, 6};
    assert(MergeLibraryMetadata(tile, std::move(background), true, false));
    assert(tile.name == L"Control title" && tile.icon.size() == 3);
    LibraryEntry names_only;
    names_only.name = L"Cached title";
    names_only.title_loaded = true;
    assert(!MergeLibraryMetadata(tile, std::move(names_only), true, false));
    assert(tile.icon.size() == 3);
    LibraryEntry interrupted;
    interrupted.name = L"Filename";
    assert(!MergeLibraryMetadata(tile, std::move(interrupted), true, true));
    assert(tile.name == L"Cached title" && tile.icon.size() == 3);
    LibraryEntry offscreen;
    offscreen.name = L"Offscreen";
    offscreen.title_loaded = true;
    offscreen.icon = {7};
    LibraryEntry other_tile;
    assert(!MergeLibraryMetadata(other_tile, std::move(offscreen), false, false));
    assert(other_tile.name == L"Offscreen" && other_tile.icon.empty());
    std::cout << "PASS base/update/DLC inheritance, isolation, identity, rescan retry and "
                 "background tile refresh\n";
}
