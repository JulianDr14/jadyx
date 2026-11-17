// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/network/nextendo/pkce.h"

#include <cctype>

#include "core/online/nextendo/encoding.h"

namespace Jadyx::Network::Nextendo {
namespace {

using Core::Online::Nextendo::Base64UrlEncode;
using Core::Online::Nextendo::RandomBytes;
using Core::Online::Nextendo::Sha256;

bool IsUnreserved(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_' ||
           c == '~';
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string UrlDecode(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out += ' ';
        } else if (text[i] == '%' && i + 2 < text.size() && HexValue(text[i + 1]) >= 0 &&
                   HexValue(text[i + 2]) >= 0) {
            out += static_cast<char>(HexValue(text[i + 1]) * 16 + HexValue(text[i + 2]));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

std::optional<std::string> QueryValue(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        const std::size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        const std::size_t eq = pair.find('=');
        if (UrlDecode(pair.substr(0, eq)) == name) {
            return eq == std::string_view::npos ? std::string{} : UrlDecode(pair.substr(eq + 1));
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query.remove_prefix(amp + 1);
    }
    return std::nullopt;
}

} // namespace

std::string ChallengeFor(std::string_view verifier) {
    const auto digest = Sha256(
        std::span{reinterpret_cast<const std::uint8_t*>(verifier.data()), verifier.size()});
    return Base64UrlEncode(digest);
}

std::optional<PkcePair> MakePkcePair() {
    const auto random = RandomBytes(48); // 48 bytes -> 64 base64url characters
    if (random.size() != 48) {
        return std::nullopt;
    }
    PkcePair pair;
    pair.verifier = Base64UrlEncode(random);
    pair.challenge = ChallengeFor(pair.verifier);
    return pair;
}

std::string MakeState() {
    const auto random = RandomBytes(24);
    return random.size() == 24 ? Base64UrlEncode(random) : std::string{};
}

std::string UrlEncode(std::string_view text) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        if (IsUnreserved(c)) {
            out += c;
        } else {
            const auto byte = static_cast<unsigned char>(c);
            out += '%';
            out += digits[byte >> 4];
            out += digits[byte & 15];
        }
    }
    return out;
}

std::string BuildAuthorizeUrl(std::string_view redirect_uri, std::string_view state,
                              std::string_view challenge) {
    std::string url{kApiBase};
    url += "/api/oauth/authorize?response_type=code&client_id=";
    url += UrlEncode(kClientId);
    url += "&redirect_uri=" + UrlEncode(redirect_uri);
    url += "&scope=" + UrlEncode(kScopes);
    url += "&state=" + UrlEncode(state);
    url += "&code_challenge=" + UrlEncode(challenge);
    url += "&code_challenge_method=S256";
    return url;
}

std::string BuildCodeExchangeBody(std::string_view code, std::string_view redirect_uri,
                                  std::string_view verifier) {
    return "grant_type=authorization_code&code=" + UrlEncode(code) +
           "&client_id=" + UrlEncode(kClientId) + "&redirect_uri=" + UrlEncode(redirect_uri) +
           "&code_verifier=" + UrlEncode(verifier);
}

std::string BuildRefreshBody(std::string_view refresh_token) {
    return "grant_type=refresh_token&refresh_token=" + UrlEncode(refresh_token) +
           "&client_id=" + UrlEncode(kClientId);
}

CallbackResult ParseCallback(std::string_view url, std::string_view expected_state) {
    const std::size_t question = url.find('?');
    if (question == std::string_view::npos) {
        return {{}, "missing_code"};
    }
    std::string_view query = url.substr(question + 1);
    if (const std::size_t hash = query.find('#'); hash != std::string_view::npos) {
        query = query.substr(0, hash);
    }
    const auto state = QueryValue(query, "state");
    if (!state || expected_state.empty() || *state != expected_state) {
        return {{}, "state_mismatch"};
    }
    if (const auto error = QueryValue(query, "error"); error && !error->empty()) {
        return {{}, *error};
    }
    const auto code = QueryValue(query, "code");
    if (!code || code->empty()) {
        return {{}, "missing_code"};
    }
    return {*code, {}};
}

} // namespace Jadyx::Network::Nextendo
