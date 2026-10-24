// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "jadyx/storage/files_browser.h"
#include <atomic>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>
#include <winrt/Windows.Storage.h>
#include "jadyx/storage/network_portal.h"
#include "jadyx/storage/system_import.h"
#include <mutex>

namespace Jadyx {
struct GameFolderSource { std::string token; std::wstring name; };
struct FileSetupStatus {
    bool keys_ready{};
    unsigned firmware_files{};
    std::vector<GameFolderSource> sources;
};
struct ConfigurationPanel {
    bool open{};
    bool files{};
    FilesBrowser browser;
    bool busy{};
    size_t selected{};
    FileSetupStatus status;
    std::wstring notice;
    unsigned completed{}, total{};
    PortalStatus network;
    bool network_screen{};
    bool ftp_screen{}; ///< Exclusive FTP page; readers resume after the server joins.
    std::wstring ftp_address, ftp_password;
    bool network_update{}; ///< The screen guides the app update instead of the file portal.
    size_t network_action{};
    std::vector<PortalAddress> network_addresses;
    size_t network_selected{};
    bool confirm_import{};
    std::wstring review_text;
};
/// Rows of the settings home, in order.
enum ConfigurationHomeRow : size_t { HomeSystem, HomeControllers, HomeFiles, HomeRowCount };
enum FileManagerNetworkRow : size_t {
    FilesNetwork = 5,
    FilesNetworkAddress,
    FilesNetworkCode,
    FilesFtp,
    FilesBrowse,
    FilesSources
};
inline size_t ConfigurationRowCount(const ConfigurationPanel& panel) {
    return panel.files ? FilesSources + panel.status.sources.size() : HomeRowCount;
}
inline size_t ConfigurationFirstRow(const ConfigurationPanel& panel) {
    return (panel.selected / 5) * 5;
}
struct FileImportProgress {
    std::atomic<unsigned> completed{};
    std::atomic<unsigned> total{};
    std::atomic<int> decision{}; // 0=pending, 1=install, 2=cancel
    std::atomic<bool> confirmation{};
    std::mutex review_mutex;
    std::wstring review_text;
};
// These operations run on an MTA worker while the guest is stopped.
FileSetupStatus ReadFileSetupStatus();
void ForgetGameFolder(std::string_view token);
std::string ImportSystemFiles(const winrt::Windows::Storage::StorageFolder& source,
                             FileImport kind, FileImportProgress& progress, std::stop_token stop);
bool HasUsableHeaderKey();
bool RunFileManagerGate(const std::function<void(std::string)>& diagnostic);
} // namespace Jadyx
