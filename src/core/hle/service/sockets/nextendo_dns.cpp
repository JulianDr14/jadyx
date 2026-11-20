// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/service/sockets/nextendo_dns.h"

#include <charconv>

#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/internal_network/network.h"
#include "core/online/nextendo/config.h"
#include "core/online/nextendo/host_rules.h"

namespace Service::Sockets::NextendoDns {
namespace {

namespace Nx = Core::Online::Nextendo;

std::optional<Network::IPv4Address> ParseIPv4(std::string_view text) {
    if (!Nx::IsLiteralIPv4(text)) {
        return std::nullopt;
    }
    Network::IPv4Address ip{};
    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t dot = text.find('.');
        const std::string_view part = text.substr(0, dot);
        unsigned value = 0; // IsLiteralIPv4 already validated every part
        [[maybe_unused]] const auto parsed =
            std::from_chars(part.data(), part.data() + part.size(), value);
        ip[i] = static_cast<u8>(value);
        text = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    }
    return ip;
}

u16 ServicePort(const std::optional<std::string>& service) {
    if (!service || service->empty()) {
        return 0;
    }
    unsigned value = 0;
    const auto [end, ec] = std::from_chars(service->data(), service->data() + service->size(), value);
    if (ec != std::errc{} || end != service->data() + service->size() || value > 0xFFFF) {
        return 0;
    }
    return static_cast<u16>(value);
}

// The emulated SD's Atmosphère hosts file, read per lookup: it is tiny and lookups are rare.
Nx::HostsFile LoadHostsFile(Core::System& system) {
    FileSys::VirtualDir sdmc;
    if (system.GetFileSystemController().OpenSDMC(&sdmc).IsError() || sdmc == nullptr) {
        return {};
    }
    const auto file = sdmc->GetFileRelative("atmosphere/hosts/default.txt");
    if (file == nullptr) {
        return {};
    }
    const auto bytes = file->ReadAllBytes();
    return Nx::HostsFile::Parse(
        std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

} // namespace

std::optional<Answer> Resolve(Core::System& system, const std::string& host,
                              const std::optional<std::string>& service) {
    const auto config = Nx::GlobalConfig().Snapshot();
    if (!config->enabled) {
        return std::nullopt;
    }
    Answer answer;
    answer.host = Nx::RewriteAccountHost(host);
    const auto hosts = LoadHostsFile(system);
    const auto ip_text = Nx::ResolveHost(answer.host, hosts, config->endpoints);
    if (!ip_text) {
        LOG_DEBUG(Service, "[Nextendo] '{}' not redirected, real DNS", answer.host);
        return answer;
    }
    const auto ip = ParseIPv4(*ip_text);
    if (!ip) {
        return answer;
    }
    answer.addrs.push_back(Network::AddrInfo{
        .family = Network::Domain::INET,
        .socket_type = Network::Type::STREAM,
        .protocol = Network::Protocol::TCP,
        .addr = {.family = Network::Domain::INET, .ip = *ip, .portno = ServicePort(service)},
        .canon_name = answer.host,
    });
    if (answer.host != host) {
        LOG_INFO(Service, "[Nextendo] '{}' -> '{}' -> {}", host, answer.host, *ip_text);
    } else {
        LOG_INFO(Service, "[Nextendo] '{}' -> {}", host, *ip_text);
    }
    RememberAnswers(answer.host, answer.addrs);
    return answer;
}

void RememberAnswers(const std::string& host, const std::vector<Network::AddrInfo>& addrs) {
    if (!Nx::GlobalConfig().Enabled()) {
        return;
    }
    for (const auto& addr : addrs) {
        Nx::GlobalAddressBook().Remember(Network::IPv4AddressToString(addr.addr.ip), host);
    }
}

} // namespace Service::Sockets::NextendoDns
