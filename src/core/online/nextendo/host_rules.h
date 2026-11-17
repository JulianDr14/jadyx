// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Nextendo name resolution, without sockets or emulator state so the desktop gate can test it.
// Order (Nextendo integration guide):
//   1. a literal IPv4 address is returned unchanged;
//   2. an exact entry of the emulated SD hosts file (/atmosphere/hosts/default.txt);
//   3. the built-in rules (NAT test hosts -> NAT IP, service hosts -> server IP);
//   4. wildcard entries of the hosts file, most specific first;
//   5. otherwise the real DNS (std::nullopt).
// Account hosts are rewritten to the BAAS host before any of this (RewriteAccountHost).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Core::Online::Nextendo {

inline constexpr std::string_view kBaasHost = "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";

enum class RuleTarget { None, Server, Nat };

/// Case-insensitive glob match where '*' matches any run of characters, dots included.
bool WildcardMatch(std::string_view pattern, std::string_view host);

/// Dotted-quad IPv4 literal ("10.0.0.1"); no hostnames, no IPv6.
bool IsLiteralIPv4(std::string_view host);

/// accounts.nintendo.com and api.accounts.nintendo.com become the BAAS host; others unchanged.
std::string RewriteAccountHost(std::string_view host);

/// Which built-in rule covers the host. NAT rules are checked before the service rules.
RuleTarget MatchBuiltinRule(std::string_view host);

/// Parsed Atmosphère hosts file. Lines are "<ip> <host>"; '#' starts a comment; '%' in a host
/// stands for the environment ("lp1"). Entries with '*' are wildcards.
class HostsFile {
public:
    static HostsFile Parse(std::string_view text);

    std::optional<std::string> FindExact(std::string_view host) const;
    /// Most specific wildcard first: the pattern with the most literal characters wins; ties keep
    /// file order.
    std::optional<std::string> FindWildcard(std::string_view host) const;

    bool Empty() const {
        return exact_.empty() && wildcard_.empty();
    }

private:
    struct Entry {
        std::string host;
        std::string ip;
    };
    std::vector<Entry> exact_;
    std::vector<Entry> wildcard_; // kept sorted by specificity
};

struct Endpoints {
    std::string server_ip; // "" means not configured
    std::string nat_ip;    // "" means not configured; falls back to server_ip
};

/// Unconfigured addresses resolve to loopback so nothing leaks to the real servers.
inline constexpr std::string_view kUnconfiguredIp = "127.0.0.1";

/// Full resolution for an already-rewritten host. std::nullopt means "use the real DNS".
std::optional<std::string> ResolveHost(std::string_view host, const HostsFile& hosts,
                                       const Endpoints& endpoints);

/// ALPN the client may offer for a host: h2 survives only on hosts containing "npln" or
/// "gs.nintendo.net"; everything else is forced to http/1.1.
std::vector<std::string> FilterAlpn(std::string_view host, const std::vector<std::string>& requested);

/// Hosts whose plain HTTP (port 80) must not be forced to TLS.
bool IsPlainHttpHost(std::string_view host);

} // namespace Core::Online::Nextendo
