// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include "jadyx/storage/usb_discovery.h"

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
}
