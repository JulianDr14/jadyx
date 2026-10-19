// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include "jadyx/storage/nsp_upload.h"

namespace Jadyx {

struct PortalAddress {
    std::string name;
    std::string ipv4;
};

/// LAN IPv4 addresses on operational Ethernet/Wi-Fi adapters, gateways first.
std::vector<PortalAddress> PortalAddresses();

enum class PortalError { None, NoAddress, Bind, Random, Listen, Storage };

struct PortalStatus {
    bool running{};
    bool paired{};
    bool code_expired{};
    std::string url;
    std::string code; ///< Local UI only; never returned by HTTP or logged.
    PortalError error{};
    UploadSnapshot transfer;
};

/// Library-owned HTTP server. Destruction stops and joins all network workers before launch.
class NetworkPortal {
public:
    explicit NetworkPortal(std::filesystem::path games = {});
    ~NetworkPortal();
    NetworkPortal(const NetworkPortal&) = delete;
    NetworkPortal& operator=(const NetworkPortal&) = delete;

    // Start/Stop/RenewCode are serialized by the library coordinator. Status is thread safe.
    bool Start(const std::string& ipv4, unsigned short port = 8080);
    void Stop();
    bool RenewCode();
    PortalStatus Status() const;
    void SetUploadsAvailable(bool enabled);
    bool BlockForLocalOperation();
    std::string FinalizeGame(const NspValidator& validate);
    void CancelTransfer();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Jadyx
