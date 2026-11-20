// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OAuth 2.0 authorization code + PKCE (RFC 7636, S256) for the Nextendo sign-in. Jadyx is a public
// client: no secret, only the client id. Pure string work so the desktop gate covers it.

#include <optional>
#include <string>
#include <string_view>

namespace Jadyx::Network::Nextendo {

inline constexpr std::string_view kClientId = "nxc_dXdnliZpNaNT";
inline constexpr std::string_view kApiBase = "https://nextendo.network";
inline constexpr std::string_view kScopes = "game.matchmaking identity presence";
/// The port is ignored by the server's loopback check; 127.0.0.1, never "localhost".
inline constexpr std::string_view kRedirectUri = "http://127.0.0.1/callback";

struct PkcePair {
    std::string verifier;  // 64 unreserved characters
    std::string challenge; // base64url(SHA-256(verifier))
};

/// Fresh verifier and its S256 challenge; std::nullopt if randomness fails.
std::optional<PkcePair> MakePkcePair();
std::string ChallengeFor(std::string_view verifier);

/// Random state value for CSRF protection (base64url of 24 bytes); "" if randomness fails.
std::string MakeState();

/// RFC 3986 percent-encoding of everything outside the unreserved set.
std::string UrlEncode(std::string_view text);

std::string BuildAuthorizeUrl(std::string_view redirect_uri, std::string_view state,
                              std::string_view challenge);

/// Form body for POST /api/oauth/token exchanging the code.
std::string BuildCodeExchangeBody(std::string_view code, std::string_view redirect_uri,
                                  std::string_view verifier);
/// Form body for POST /api/oauth/token refreshing (the refresh token rotates on every use).
std::string BuildRefreshBody(std::string_view refresh_token);

struct CallbackResult {
    std::string code;
    std::string error; // "access_denied", "state_mismatch", "missing_code", ...
};

/// Reads the redirect URL the browser landed on. The state must match the one we sent.
CallbackResult ParseCallback(std::string_view url, std::string_view expected_state);

/// What a player pasted from a phone's address bar, ready for ParseCallback: blanks, quotes and
/// angle brackets around it dropped; a bare query ("code=..&state=..") gets its '?'. "" when it
/// holds nothing usable or is longer than any sign-in answer.
std::string PastedCallback(std::string_view text);

} // namespace Jadyx::Network::Nextendo
