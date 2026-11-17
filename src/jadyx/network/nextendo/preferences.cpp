// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/network/nextendo/preferences.h"

#include "core/online/nextendo/host_rules.h"

namespace Jadyx::Network::Nextendo {

namespace Nx = Core::Online::Nextendo;

std::optional<std::string> CleanAddress(std::string_view typed) {
    while (!typed.empty() && (typed.front() == ' ' || typed.front() == '\t')) {
        typed.remove_prefix(1);
    }
    while (!typed.empty() && (typed.back() == ' ' || typed.back() == '\t')) {
        typed.remove_suffix(1);
    }
    if (typed.empty()) {
        return std::string{};
    }
    if (!Nx::IsLiteralIPv4(typed)) {
        return std::nullopt;
    }
    return std::string(typed);
}

Nx::Config BuildCoreConfig(const Preferences& preferences,
                           const std::optional<GameIdentity>& identity) {
    Nx::Config config;
    config.tls_mode = preferences.tls_mode;
    if (!preferences.use_nextendo || !EndpointsReady(preferences) || !identity ||
        identity->nex_token.empty()) {
        return config;
    }
    config.enabled = true;
    config.endpoints.server_ip = preferences.server_ip;
    config.endpoints.nat_ip = preferences.nat_ip;
    config.nex_token = identity->nex_token;
    config.pid = identity->pid;
    config.nsa_id_hex = identity->nsa_id;
    return config;
}

bool SameCoreConfig(const Nx::Config& a, const Nx::Config& b) {
    return a.enabled == b.enabled && a.endpoints.server_ip == b.endpoints.server_ip &&
           a.endpoints.nat_ip == b.endpoints.nat_ip && a.tls_mode == b.tls_mode &&
           a.nex_token == b.nex_token && a.pid == b.pid && a.nsa_id_hex == b.nsa_id_hex;
}

LaunchCheck CheckLaunch(const Preferences& preferences, const SessionView& view,
                        const Nx::GameCatalog& catalog, std::uint64_t title_id,
                        std::string_view installed_version) {
    LaunchCheck check;
    if (!preferences.use_nextendo) {
        return check;
    }
    if (!EndpointsReady(preferences)) {
        check.advice = LaunchAdvice::NoServer;
        return check;
    }
    if (view.phase != Phase::Ready) {
        check.advice = LaunchAdvice::NotSignedIn;
        return check;
    }
    if (view.status_known && !view.online_allowed) {
        check.advice = LaunchAdvice::Blocked;
        return check;
    }
    const auto verdict = Nx::CheckTitle(catalog, title_id, installed_version);
    check.recommended_version = verdict.recommended_version;
    switch (verdict.support) {
    case Nx::TitleSupport::Supported:
        check.advice = LaunchAdvice::Supported;
        break;
    case Nx::TitleSupport::WrongVersion:
        check.advice = LaunchAdvice::WrongVersion;
        break;
    case Nx::TitleSupport::Disabled:
        check.advice = LaunchAdvice::Disabled;
        break;
    case Nx::TitleSupport::UnsupportedClient:
        check.advice = LaunchAdvice::UnsupportedClient;
        break;
    default:
        check.advice = LaunchAdvice::NotListed;
        break;
    }
    return check;
}

} // namespace Jadyx::Network::Nextendo
