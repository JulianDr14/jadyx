// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/uwp_nxbox_import.h"

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "jadyx/library/play_history.h"
#include "jadyx/storage/nxbox_import_store.h"
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>

namespace Jadyx {
namespace {
std::filesystem::path TransferDirectory() {
    return std::filesystem::path{
               winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()} /
           "nxbox-import";
}
std::filesystem::path PendingPath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "nxbox-import-library.json";
}
} // namespace

NxboxWelcome ImportNxboxSetup() {
    NxboxWelcome welcome;
    try {
        const auto directory = TransferDirectory();
        if (!std::filesystem::is_directory(directory))
            return welcome;
        const auto values =
            winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        const auto result = NxboxImport::ImportTransfer(
            directory, PendingPath(), [&](const std::wstring& key) { return values.HasKey(key); },
            [&](const std::wstring& key, const NxboxImport::Value& value) {
                std::visit(
                    [&](const auto& item) {
                        using Item = std::decay_t<decltype(item)>;
                        if constexpr (std::is_same_v<Item, std::string>)
                            values.Insert(key, winrt::box_value(winrt::to_hstring(item)));
                        else
                            values.Insert(key, winrt::box_value(item));
                    },
                    value);
            });
        if (result == NxboxImport::ImportResult::Invalid ||
            result == NxboxImport::ImportResult::StorageError) {
            LOG_WARNING(Frontend,
                        "NXbox import: incomplete metadata or storage failure; will retry");
            return welcome;
        }
        if (std::filesystem::exists(directory / "welcome-dismissed.flag"))
            return welcome;
        const auto receipt_text = NxboxImport::ReadTransfer(directory / "receipt.json");
        if (!receipt_text)
            return welcome;
        const auto receipt = NxboxImport::ParseReceipt(*receipt_text);
        if (!receipt)
            return welcome;
        welcome.receipt = *receipt;
        if (const auto folders = NxboxImport::ReadTransfer(directory / "usb-folders.txt"))
            welcome.usb_folders = NxboxImport::ParseUsbFolders(*folders);
        welcome.visible = true;
    } catch (...) {
        LOG_WARNING(Frontend, "NXbox import: storage unavailable; will retry next library start");
    }
    return welcome;
}

bool DismissNxboxWelcome() {
    try {
        return WriteFileAtomically(TransferDirectory() / "welcome-dismissed.flag", "1\n");
    } catch (...) {
        return false;
    }
}

bool ResolveNxboxLibrary(const std::vector<LibraryEntry>& entries, Favorites& favorites) {
    try {
        const auto text = NxboxImport::ReadTransfer(PendingPath());
        if (!text)
            return false;
        const auto games = NxboxImport::ParseLibrary(*text);
        if (!games || games->empty())
            return false;
        auto history = LoadPlayHistory();
        auto merged_favorites = favorites;
        const auto pending = NxboxImport::ResolveGames(*games, entries, merged_favorites, history);
        if (pending.size() == games->size())
            return false;
        // Write destinations first: a retry uses max counters and set union, never addition.
        if (!SaveFavorites(merged_favorites) || !SavePlayHistory(history))
            return false;
        favorites = std::move(merged_favorites);
        return WriteFileAtomically(PendingPath(), NxboxImport::SerializeLibrary(pending));
    } catch (...) {
        return false;
    }
}
} // namespace Jadyx
