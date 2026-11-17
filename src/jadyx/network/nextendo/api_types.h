// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Request bodies and response parsing for the Nextendo REST API, kept apart from the transport so
// the desktop gate can feed canned responses. Tokens never reach the log from here.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Jadyx::Network::Nextendo {

/// What the caller should do after a call, from the status code (integration guide).
enum class CallOutcome : std::uint8_t {
    Ok,
    RefreshFirst, // 401: refresh the access token, then retry once
    MissingScope, // 403: the account did not grant game.matchmaking; sign in again
    RetryLater,   // 503 or a network failure
    RateLimited,  // 429: wait retry_after_seconds
    Rejected,     // any other 4xx: the request itself is wrong
    Malformed,    // 200 with a body we cannot read
};

CallOutcome OutcomeForStatus(int status);

/// Retry-After as delta seconds; HTTP dates and garbage fall back to fallback_seconds.
std::uint32_t ParseRetryAfter(std::string_view header, std::uint32_t fallback_seconds = 60);

struct OAuthTokens {
    CallOutcome outcome = CallOutcome::Malformed;
    std::string access_token;
    std::string refresh_token; // rotates on every refresh: always store the newest
    std::int64_t expires_in = 0;
    std::string scope;
    std::string error; // OAuth "error" field on 400, e.g. "invalid_grant"
};

OAuthTokens ParseTokenResponse(int status, std::string_view body);

struct GameToken {
    CallOutcome outcome = CallOutcome::Malformed;
    std::string nex_token; // opaque "nx2..." value; never parsed
    std::uint64_t pid = 0;
    std::string friend_code;
    std::string username;
    std::int64_t expires_in = 0;
    std::uint32_t retry_after_seconds = 0;
};

GameToken ParseGameToken(int status, std::string_view body, std::string_view retry_after);

enum class OnlineBlock : std::uint8_t {
    None,
    Unknown,
    Disabled,
    Suspended,
    Unverified,      // e-mail not verified
    DiscordUnlinked, // Discord account not linked
    Elsewhere,       // already online on another device
};

struct OnlineStatus {
    CallOutcome outcome = CallOutcome::Malformed;
    bool allow = false;
    OnlineBlock reason = OnlineBlock::Unknown;
};

OnlineStatus ParseOnlineStatus(int status, std::string_view body);

struct SessionStatus {
    CallOutcome outcome = CallOutcome::Malformed;
    bool email_verified = false;
    bool revoked = false;
};

SessionStatus ParseSessionStatus(int status, std::string_view body);

enum class PresenceState : std::uint8_t { Offline = 0, Online = 1, Playing = 2 };

/// POST /api/presence body. detail is cut to 64 bytes on a UTF-8 boundary.
std::string BuildPresenceBody(PresenceState state, std::uint64_t title_id, std::string_view detail);

/// "/api/me" nsa field (16 hex), in "account" or at the top level, or std::nullopt.
std::optional<std::string> ParseNsaId(std::string_view me_body);

/// What the profile card shows, from GET /api/me ({account: {pid, username, name, friend_code}}).
/// Display only: the identity the games see is the pid of the game token.
struct Profile {
    CallOutcome outcome = CallOutcome::Malformed;
    std::uint64_t pid = 0;
    std::string username;
    std::string display_name; // "name"; empty when the player set none
    std::string friend_code;
    std::string nsa_id; // 16 hex or empty
};

Profile ParseProfile(int status, std::string_view body);

/// The account picture: https://nextendo.network/api/avatar?pid=<pid>.
std::string AvatarUrl(std::uint64_t pid);

} // namespace Jadyx::Network::Nextendo
