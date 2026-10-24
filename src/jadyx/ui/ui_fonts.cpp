// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/ui/ui_fonts.h"

#include <filesystem>
#include <mutex>

#include <dwrite_3.h>
#include <wrl/client.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>

namespace Jadyx {
using Microsoft::WRL::ComPtr;

namespace {

ComPtr<IDWriteFontCollection> LoadInter(IDWriteFactory* write) {
    ComPtr<IDWriteFactory5> factory;
    if (FAILED(write->QueryInterface(IID_PPV_ARGS(&factory)))) return {};
    std::filesystem::path root;
    try {
        root = std::filesystem::path{winrt::Windows::ApplicationModel::Package::Current()
                                         .InstalledLocation().Path().c_str()} / L"Assets" / L"Fonts";
    } catch (const winrt::hresult_error&) {
        return {}; // Not packaged (PC tests).
    }
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(factory->CreateFontSetBuilder(&builder))) return {};
    for (const wchar_t* name : {L"Inter-Regular.ttf", L"Inter-Medium.ttf", L"Inter-SemiBold.ttf",
                                L"Inter-Bold.ttf"}) {
        ComPtr<IDWriteFontFile> file;
        if (FAILED(factory->CreateFontFileReference((root / name).c_str(), nullptr, &file)) ||
            FAILED(builder->AddFontFile(file.Get())))
            return {};
    }
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(builder->CreateFontSet(&set)) ||
        FAILED(factory->CreateFontCollectionFromFontSet(set.Get(), &collection)))
        return {};
    UINT32 index{};
    BOOL exists{};
    if (FAILED(collection->FindFamilyName(L"Inter", &index, &exists)) || !exists) return {};
    return collection;
}

} // Anonymous namespace

InterfaceTypeface SystemInterfaceFonts(IDWriteFactory* write, bool monospace) {
    if (monospace) {
        ComPtr<IDWriteFontCollection> fonts;
        if (SUCCEEDED(write->GetSystemFontCollection(&fonts, FALSE))) {
            for (const auto* family : {L"Consolas", L"Cascadia Mono", L"Courier New"}) {
                UINT32 index{};
                BOOL exists{};
                if (SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists)
                    return {.collection = nullptr, .family = family};
            }
        }
    }
    return {.collection = nullptr, .family = L"Segoe UI"};
}

InterfaceTypeface InterfaceFonts(IDWriteFactory* write) {
    static std::once_flag once;
    static ComPtr<IDWriteFontCollection> inter;
    std::call_once(once, [&] { inter = LoadInter(write); });
    if (inter) return {inter.Get(), L"Inter"};
    return {nullptr, L"Segoe UI"};
}

} // namespace Jadyx
