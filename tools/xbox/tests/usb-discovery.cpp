// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/usb_discovery.h"
#include <cassert>
#include <vector>

int main() {
    using namespace Jadyx;
    assert(!UsbPlayInPlaceEnabled(nullptr));
    for (const auto* value : {"", "0", "true", "10", "1 "})
        assert(!UsbPlayInPlaceEnabled(value));
    assert(UsbPlayInPlaceEnabled("1"));
    assert(StorageFoldersOverlap(L"E:\\", L"e:\\Games"));
    assert(StorageFoldersOverlap(L"E:/Games/", L"e:\\games\\homebrew"));
    assert(StorageFoldersOverlap(L"E:\\Games", L"E:\\"));
    assert(StorageFoldersOverlap(L"E:\\Games", L"e:\\games"));
    assert(!StorageFoldersOverlap(L"E:\\Games", L"E:\\Games-old"));
    assert(!StorageFoldersOverlap(L"E:\\", L"F:\\Games"));
    assert(!StorageFoldersOverlap(L"", L"E:\\"));
    assert(!StorageFoldersOverlap(L"E:\\", L""));
    assert(StorageFoldersOverlap(L"E:\\Juegos 日本語", L"e:/Juegos 日本語/a"));
    const std::vector<std::wstring> registered{L"F:\\NXbox\\games"};
    assert(!ShouldDiscoverUsbDrive(L"f:/", registered));
    assert(ShouldDiscoverUsbDrive(L"G:/", registered));
    const std::vector<std::wstring> legacy{L"F:/", L"F:/NXbox/games"};
    assert(!ShouldScanStorageSource(L"F:/", legacy));
    assert(ShouldScanStorageSource(L"F:/NXbox/games", legacy));
    assert(ShouldScanStorageSource(L"G:/", legacy));
    const std::vector<std::wstring> reverse{L"F:/NXbox/games", L"F:/"};
    assert(!ShouldScanStorageSource(L"F:/", reverse));
    assert(!ShouldDiscoverUsbDrive(L"f:/", reverse));
    assert(IsStorageDriveRoot(L"F:\\"));
    assert(!IsStorageDriveRoot(L"F:/NXbox/games"));
    for (const auto *folder : {"games", "GAMES", "switch", "NXbox", "Jadyx",
                               "NXbox/games", "JADYX/GAMES"})
      assert(ShouldDescendUsbFolder(folder));
    for (const auto *folder :
         {"Windows", "Users", "other", "NXbox/cache", "switch/sub",
          "games/title", "NXbox/games/title", "NXbox-old/games"})
      assert(!ShouldDescendUsbFolder(folder));
    StorageScanBudget first;
    for (unsigned i = 0; i < 10000; ++i)
      assert(first.Visit());
    assert(!first.Visit() && first.limited && first.visited == 10000);
    LibraryScan combined;
    LibraryScan partial;
    LibraryEntry found{};
    found.launch_path = "first.nro";
    partial.entries.push_back(found);
    partial.limited = first.limited;
    AppendStorageScan(combined, std::move(partial));
    StorageScanBudget second;
    assert(second.Visit() && !second.limited && second.visited == 1);
    LibraryScan complete;
    found.launch_path = "second.nro";
    for (unsigned game = 0; game < 17; ++game)
      complete.entries.push_back(found);
    AppendStorageScan(combined, std::move(complete));
    assert(combined.limited && combined.entries.size() == 18);
    assert(combined.entries[0].launch_path == "first.nro");
    assert(combined.entries[1].launch_path == "second.nro");
}
