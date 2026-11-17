// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/online/nextendo/host_rules.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace Core::Online::Nextendo {
namespace {

char Lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string ToLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), Lower);
    return out;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

// A name the resolver accepts: trailing dot dropped, lower case.
std::string Normalize(std::string_view host) {
    host = Trim(host);
    if (!host.empty() && host.back() == '.') {
        host.remove_suffix(1);
    }
    return ToLower(host);
}

constexpr std::array kNatRules{
    std::string_view{"nncs2-*.n.n.srv.nintendo.net"},
    std::string_view{"g2b309e01-lp1.s.n.srv.nintendo.net"},
    std::string_view{"g2df33d01-lp1.p.srv.nintendo.net"},
};

constexpr std::array kServerRules{
    std::string_view{"*.nintendo.net"},
    std::string_view{"*.nintendo.com"},
    std::string_view{"*.nintendowifi.net"},
    std::string_view{"*.nintendo.co.jp"},
    std::string_view{"demonware.net"},
    std::string_view{"*.demonware.net"},
    std::string_view{"nswitch.f13.illfonic.com"},
    std::string_view{"spring18.gosredirector.ea.com"},
    std::string_view{"pvzgw3prodapp-*.ea.com"},
    std::string_view{"public-ubiservices.ubi.com"},
    std::string_view{"public-ws-ubiservices.ubi.com"},
    std::string_view{"jd.ubisoft.com"},
    std::string_view{"phonescoring.jd.ubisoft.com"},
    std::string_view{"v2.phonescoring.jd.ubisoft.com"},
    std::string_view{"api.braincloudservers.com"},
    std::string_view{"na.ultimatechickenhorseserver.com"},
    std::string_view{"eu.ultimatechickenhorseserver.com"},
    std::string_view{"ap.ultimatechickenhorseserver.com"},
};

std::size_t Specificity(std::string_view pattern) {
    return static_cast<std::size_t>(std::count_if(pattern.begin(), pattern.end(),
                                                  [](char c) { return c != '*'; }));
}

const std::string& OrLoopback(const std::string& ip, const std::string& loopback) {
    return ip.empty() ? loopback : ip;
}

} // namespace

bool WildcardMatch(std::string_view pattern, std::string_view host) {
    // Iterative glob with single-star backtracking; '*' spans any characters, dots included.
    std::size_t p = 0;
    std::size_t h = 0;
    std::size_t star = std::string_view::npos;
    std::size_t resume = 0;
    while (h < host.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = h;
        } else if (p < pattern.size() && Lower(pattern[p]) == Lower(host[h])) {
            ++p;
            ++h;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            h = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

bool IsLiteralIPv4(std::string_view host) {
    int parts = 0;
    while (true) {
        std::size_t digits = 0;
        unsigned value = 0;
        while (digits < host.size() && std::isdigit(static_cast<unsigned char>(host[digits]))) {
            value = value * 10 + static_cast<unsigned>(host[digits] - '0');
            if (++digits > 3) {
                return false;
            }
        }
        if (digits == 0 || value > 255 || (digits > 1 && host[0] == '0')) {
            return false;
        }
        ++parts;
        host.remove_prefix(digits);
        if (host.empty()) {
            return parts == 4;
        }
        if (host.front() != '.' || parts == 4) {
            return false;
        }
        host.remove_prefix(1);
    }
}

std::string RewriteAccountHost(std::string_view host) {
    const std::string normalized = Normalize(host);
    if (normalized == "accounts.nintendo.com" || normalized == "api.accounts.nintendo.com") {
        return std::string(kBaasHost);
    }
    return std::string(host);
}

RuleTarget MatchBuiltinRule(std::string_view host) {
    const std::string normalized = Normalize(host);
    for (const auto rule : kNatRules) {
        if (WildcardMatch(rule, normalized)) {
            return RuleTarget::Nat;
        }
    }
    for (const auto rule : kServerRules) {
        if (WildcardMatch(rule, normalized)) {
            return RuleTarget::Server;
        }
    }
    return RuleTarget::None;
}

HostsFile HostsFile::Parse(std::string_view text) {
    HostsFile file;
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);

        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = Trim(line);
        const std::size_t gap = line.find_first_of(" \t");
        if (gap == std::string_view::npos) {
            continue;
        }
        const std::string_view ip = line.substr(0, gap);
        if (!IsLiteralIPv4(ip)) {
            continue;
        }
        // Atmosphère allows several hosts after one address.
        std::string_view rest = Trim(line.substr(gap));
        while (!rest.empty()) {
            const std::size_t end = rest.find_first_of(" \t");
            std::string host = Normalize(rest.substr(0, end));
            rest = end == std::string_view::npos ? std::string_view{} : Trim(rest.substr(end));
            for (std::size_t at = host.find('%'); at != std::string::npos; at = host.find('%')) {
                host.replace(at, 1, "lp1");
            }
            if (host.empty()) {
                continue;
            }
            auto& list = host.find('*') == std::string::npos ? file.exact_ : file.wildcard_;
            list.push_back({std::move(host), std::string(ip)});
        }
    }
    std::stable_sort(file.wildcard_.begin(), file.wildcard_.end(),
                     [](const Entry& a, const Entry& b) {
                         return Specificity(a.host) > Specificity(b.host);
                     });
    return file;
}

std::optional<std::string> HostsFile::FindExact(std::string_view host) const {
    const std::string normalized = Normalize(host);
    for (const auto& entry : exact_) {
        if (entry.host == normalized) {
            return entry.ip;
        }
    }
    return std::nullopt;
}

std::optional<std::string> HostsFile::FindWildcard(std::string_view host) const {
    const std::string normalized = Normalize(host);
    for (const auto& entry : wildcard_) {
        if (WildcardMatch(entry.host, normalized)) {
            return entry.ip;
        }
    }
    return std::nullopt;
}

std::optional<std::string> ResolveHost(std::string_view host, const HostsFile& hosts,
                                       const Endpoints& endpoints) {
    const std::string trimmed{Trim(host)};
    if (IsLiteralIPv4(trimmed)) {
        return trimmed;
    }
    if (auto ip = hosts.FindExact(trimmed)) {
        return ip;
    }
    const std::string loopback{kUnconfiguredIp};
    switch (MatchBuiltinRule(trimmed)) {
    case RuleTarget::Nat:
        return endpoints.nat_ip.empty() ? OrLoopback(endpoints.server_ip, loopback)
                                        : endpoints.nat_ip;
    case RuleTarget::Server:
        return OrLoopback(endpoints.server_ip, loopback);
    case RuleTarget::None:
        break;
    }
    return hosts.FindWildcard(trimmed);
}

std::vector<std::string> FilterAlpn(std::string_view host,
                                    const std::vector<std::string>& requested) {
    const std::string normalized = Normalize(host);
    const bool h2_host = normalized.find("npln") != std::string::npos ||
                         normalized.find("gs.nintendo.net") != std::string::npos;
    if (h2_host && !requested.empty()) {
        return requested;
    }
    return {"http/1.1"};
}

bool IsPlainHttpHost(std::string_view host) {
    return Normalize(host) == "ctest.cdn.nintendo.net";
}

} // namespace Core::Online::Nextendo
