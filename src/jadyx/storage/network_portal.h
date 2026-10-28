// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <memory>
#include <functional>
#include <cstdint>
#include <optional>
#include <utility>
#include "jadyx/updates/update_package.h"
#include <string>
#include <vector>
#include "jadyx/storage/nsp_upload.h"
#include "jadyx/storage/system_upload.h"

namespace Jadyx {

enum class PortalLink { Ethernet, WiFi };

struct PortalAddress {
    std::string name; ///< "Ethernet" or "Wi-Fi", from `link`.
    std::string ipv4;
    PortalLink link{};
    bool gateway{};    ///< The router answered: phones on the same network can reach it.
    bool link_local{}; ///< 169.254.x.x: the router gave no address, so phones cannot reach it.
};

/// LAN IPv4 addresses on operational Ethernet/Wi-Fi adapters: with a gateway first, then by
/// the system's own route metric, link-local last. Only addresses the console owns for sure.
std::vector<PortalAddress> PortalAddresses();
/// Whether an operational adapter still holds `ipv4`. Cheaper than PortalAddresses for polling.
bool PortalAddressOwned(const std::string& ipv4);
/// Every adapter the console sees, kept or not and why, for the portal fault reports.
/// No MAC addresses or adapter GUIDs.
std::string PortalAdapterReport();

enum class PortalError { None, NoAddress, AddressLost, PortInUse, PortBlocked, Bind, Random, Listen, Storage };

/// A deletion asked from the browser. The library runs it with its other storage jobs.
struct PortalRemoval {
    std::uint64_t serial{};      ///< One per request, so the page follows its own.
    std::uint64_t application{};
    std::string content;         ///< Update/DLC id; empty deletes the whole game.
    std::string phase;           ///< "", "pending", "running", "complete" or "failed".
    std::string error;
    bool operator==(const PortalRemoval&) const = default;
};

struct PortalStatus {
    bool running{};
    bool paired{};
    bool code_expired{};
    std::string url;
    std::string code; ///< Local UI only; never returned by HTTP or logged.
    PortalError error{};
    unsigned short port{}; ///< Listening port, or the first one tried when the start failed.
    int os_error{};        ///< WSA code behind a failed bind, 0 when there was none.
    UploadSnapshot transfer;
    UploadSnapshot content; ///< Update/DLC package; its tag is "<kind>:<application hex>".
    SystemUploadStatus system;
    PortalRemoval removal;
};

/// One installed game as the browser sees it. Never carries a console path.
struct PortalGame {
    std::uint64_t program_id{};
    std::string name, developer, version;
    bool removable{}; ///< Every dump is in internal storage, so the console can delete it.
    bool operator==(const PortalGame&) const = default;
};

/// One update/DLC already registered for a game.
struct PortalContent {
    std::string id, kind, name, version;
    std::uint64_t title{}, bytes{};
    bool enabled{}, internal{};
};

/// Read-only library view supplied by the coordinator. Every callback runs on a portal
/// worker, must be thread safe and must outlive the server.
struct PortalLibrary {
    std::function<std::pair<std::uint64_t, std::vector<PortalGame>>()> games; ///< revision + games
    std::function<std::optional<std::string>(std::uint64_t)> cover;          ///< JPEG bytes
    std::function<std::vector<PortalContent>(std::uint64_t)> contents;
    std::function<std::pair<std::uint64_t, std::uint64_t>()> progress;      ///< installed MiB, total MiB
};

/// Installs an update/DLC upload; `tag` is the validated "<kind>:<application hex>".
using ContentValidator = std::function<std::string(const std::filesystem::path&, const std::string& tag, std::stop_token)>;
/// Carries out the pending removal; returns an error word, empty on success.
using RemovalRunner = std::function<std::string(const PortalRemoval&)>;

/// Library-owned HTTP server. Destruction stops and joins all network workers before launch.
class NetworkPortal {
public:
    explicit NetworkPortal(std::filesystem::path games = {}, SystemImportBackend system_backend = {});
    ~NetworkPortal();
    NetworkPortal(const NetworkPortal&) = delete;
    NetworkPortal& operator=(const NetworkPortal&) = delete;

    // Start/Stop/RenewCode are serialized by the library coordinator. Status is thread safe.
    /// When `port` is taken or the system refuses it, the next `spare_ports` ports are tried.
    bool Start(const std::string& ipv4, unsigned short port = 8080, unsigned spare_ports = 0);
    void Stop();
    bool RenewCode();
    PortalStatus Status() const;
    void SetUploadsAvailable(bool enabled);
    // Fixed verified package supplied by the library; caller outlives this server.
    void SetUpdatePackage(std::function<std::optional<Updates::UpdatePackage>()> package);
    // While true the browser opens on the guided app update instead of the content pages.
    void SetUpdateOffered(bool offered);
    // Configure the fixed app log root before Start; never takes a browser path.
    void SetLogDirectory(std::filesystem::path directory);
    // Configure the library view before Start; without it the library routes answer 503.
    void SetLibrary(PortalLibrary library);
    bool BlockForLocalOperation();
    std::string FinalizeGame(const NspValidator& validate = {});
    /// Only the library coordinator calls this, after its metadata worker has joined.
    std::string FinalizeContent(const ContentValidator& validate);
    /// Runs the pending removal, if any. Only the library coordinator calls this, from its
    /// storage worker; uploads stay closed from the request until it returns.
    std::string RunRemoval(const RemovalRunner& run);
    void CancelTransfer();
    std::string RunSystemImport(std::stop_token stop);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Jadyx
