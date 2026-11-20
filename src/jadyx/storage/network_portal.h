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
#include <string_view>
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

/// A deletion or move asked from the browser. The library runs it with its other storage jobs.
struct PortalRemoval {
    std::uint64_t serial{};      ///< One per request, so the page follows its own.
    std::uint64_t application{};
    std::string content;         ///< Update/DLC id; empty deletes the whole game.
    /// Empty deletes. "internal" or "developer" moves the update/DLC there instead.
    std::string destination;
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
    UploadSnapshot content; ///< Update/DLC package or save archive; its tag is "<kind>:<application hex>".
    SystemUploadStatus system;
    PortalRemoval removal;
};

/// What a phone's camera opens to reach the portal already paired: the console draws it as a QR
/// code (ui/qr_code.h) beside the typed code. "<url>/#pair=<code>", plus "&open=<tab>" to show a
/// tab of the page once paired ("games", "amiibo", "nextendo", "system"). The code rides in the
/// fragment, which browsers never send, and the page takes it out of the address bar at once.
/// "" when no code can pair right now: the portal is stopped or its code expired.
std::string PortalPairingLink(const PortalStatus& status, std::string_view open = {});

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
    bool developer{}; ///< The console's copy is on D: (only with `internal`).
};

/// Read-only library view supplied by the coordinator. Every callback runs on a portal
/// worker, must be thread safe and must outlive the server.
struct PortalLibrary {
    std::function<std::pair<std::uint64_t, std::vector<PortalGame>>()> games; ///< revision + games
    std::function<std::optional<std::string>(std::uint64_t)> cover;          ///< JPEG bytes
    std::function<std::vector<PortalContent>(std::uint64_t)> contents;
    std::function<std::pair<std::uint64_t, std::uint64_t>()> progress;      ///< installed MiB, total MiB
    /// Zips the game's save into the string; returns an error word ("no_save" when there is none).
    std::function<std::string(std::uint64_t, std::string&)> save_export;
};

struct PortalAmiiboFile {
    std::string name;
    std::uint64_t bytes{};
};

/// The player's amiibo files and key (amiibo_store.h). Every callback runs on a portal worker,
/// must be thread safe and must outlive the server. Writes return an error word, empty on
/// success. Nothing here hands the key back or says where anything lives on the console.
struct PortalAmiibo {
    std::function<std::vector<PortalAmiiboFile>()> list;
    std::function<bool()> has_key;
    std::function<std::string(const std::string& name, const std::string& bytes, bool replace)> add;
    std::function<std::string(const std::string& name)> remove;
    std::function<std::string(const std::string& bytes)> set_key;
    std::function<std::string()> remove_key;
};

/// The Nextendo sign-in, finished from the browser (network/nextendo/service.h): the console has
/// no browser that can sign in, so the player opens the page there and pastes back the address it
/// ends on. Callbacks run on a portal worker and must be thread safe. The page's address carries
/// no secret: the code is only worth something with the verifier, which stays in the console.
struct PortalNextendo {
    std::function<std::string()> sign_in_url;                     ///< "" when no sign-in waits
    std::function<std::string(const std::string& pasted)> finish; ///< error word, "" on success
};

/// Installs an update/DLC upload; `tag` is the validated "<kind>:<application hex>" and
/// `developer` asks for its copy on D:.
using ContentValidator = std::function<std::string(const std::filesystem::path&, const std::string& tag,
                                                   bool developer, std::stop_token)>;
using GameValidator = std::function<std::string(const std::filesystem::path&, const std::string& name, std::stop_token)>;
/// Carries out the pending removal or move; returns an error word, empty on success.
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
    // Configure the amiibo files before Start; without them the amiibo routes answer 503.
    void SetAmiibo(PortalAmiibo amiibo);
    // Configure the Nextendo sign-in before Start; without it its routes answer 503.
    void SetNextendo(PortalNextendo nextendo);
    bool BlockForLocalOperation();
    std::string FinalizeGame(const GameValidator& validate);
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
