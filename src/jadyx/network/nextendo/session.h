// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Nextendo account session, without threads or platform calls: the transport, the secret
// storage and the clock come from outside, so the desktop gate drives it against a fake server.
//
// What it keeps alive while signed in (integration guide):
//   - the OAuth access token (1 h), refreshed with the rotating refresh token (30 days), which is
//     the only secret written anywhere (Storage, the PasswordVault on the console);
//   - the nx2 game token (24 h), asked again two hours before it runs out;
//   - online-status and session-status every 15 minutes (a revoked session signs out);
//   - the profile from /api/me;
//   - presence every 60 s while wanted (the server drops it after 90 s), and the nex-session
//     heartbeat while a game is played;
//   - the signed game catalog, verified with the pinned Ed25519 key and cached.
// 429 waits for Retry-After; network failures back off from 15 s up to 10 minutes.
//
// Tokens never leave this object except through Identity(), which the app hands to the emulated
// account service. Nothing here logs.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/online/nextendo/catalog.h"
#include "jadyx/network/http.h"
#include "jadyx/network/nextendo/api_types.h"
#include "jadyx/network/nextendo/pkce.h"

namespace Jadyx::Network::Nextendo {

/// Where the session keeps what must survive a restart.
class Storage {
public:
    virtual ~Storage() = default;
    virtual std::optional<std::string> LoadRefreshToken() = 0;
    virtual bool SaveRefreshToken(std::string_view token) = 0;
    virtual void ClearRefreshToken() = 0;
    /// The signed catalog envelope as downloaded; verified again on every load.
    virtual std::optional<std::string> LoadCatalog() = 0;
    virtual void SaveCatalog(std::string_view envelope) = 0;
};

using Transport = std::function<Http::ApiResponse(const Http::ApiRequest&)>;
/// Unix seconds.
using Clock = std::function<std::int64_t()>;

enum class Phase : std::uint8_t {
    SignedOut,
    SigningIn,  // the browser page is open
    Connecting, // signed in, asking for the tokens
    Ready,      // the game token is in hand
};

/// The last thing that went wrong, for the screen. Cleared by the next success.
enum class Problem : std::uint8_t {
    None,
    Offline,         // no answer; retried with back-off
    RateLimited,     // 429; retried after Retry-After
    Unavailable,     // 503; retried
    SignInCancelled, // the player closed the page
    SignInRejected,  // the server refused the code, or the callback was wrong
    SessionExpired,  // the refresh token no longer works: sign in again
    MissingScope,    // 403 on the game token: sign in again granting game access
    Revoked,         // the player removed this console from the website
    StorageFailed,   // the vault would not keep the refresh token
};

enum class CatalogState : std::uint8_t { Empty, Loading, Ready, Failed };

struct CatalogEntry {
    std::string name;
    std::vector<std::uint64_t> title_ids;
    bool enabled = false;
    bool client_supported = false; // every required capability is implemented here
    std::vector<std::string> accepted_versions;
    std::string recommended_version;
};

struct SessionView {
    Phase phase = Phase::SignedOut;
    Problem problem = Problem::None;
    std::uint32_t retry_after = 0; // seconds, with RateLimited

    std::uint64_t pid = 0;
    std::string username;
    std::string display_name;
    std::string friend_code;
    bool email_verified = false;
    bool status_known = false; // online-status answered at least once
    bool online_allowed = false;
    OnlineBlock block = OnlineBlock::None;
    std::int64_t game_token_expires = 0; // unix seconds

    bool presence_live = false; // the last presence call succeeded
    std::uint64_t playing_title = 0;

    CatalogState catalog_state = CatalogState::Empty;
    std::int64_t catalog_revision = 0;
    std::int64_t catalog_expires = 0;
    std::vector<CatalogEntry> catalog;

    std::uint64_t version = 0; // bumps on every change
};

/// What the emulated account service needs to log a game in.
struct GameIdentity {
    std::string nex_token;
    std::uint64_t pid = 0;
    std::string nsa_id; // 16 hex or empty
};

namespace Timing {
inline constexpr std::int64_t AccessMargin = 120;       // refresh this long before expiry
inline constexpr std::int64_t GameTokenMargin = 2 * 3600;
inline constexpr std::int64_t StatusEvery = 15 * 60;
inline constexpr std::int64_t ProfileEvery = 6 * 3600;
inline constexpr std::int64_t PresenceEvery = 60;       // the server forgets after 90 s
inline constexpr std::int64_t HeartbeatEvery = 60;
inline constexpr std::int64_t CatalogEvery = 12 * 3600; // it expires after 24 h
inline constexpr std::int64_t CatalogRetry = 10 * 60;
inline constexpr std::int64_t FirstBackoff = 15;
inline constexpr std::int64_t MaxBackoff = 10 * 60;
inline constexpr std::int64_t Idle = 3600; // nothing due: wake up anyway
} // namespace Timing

class Session {
public:
    /// catalog_key: the pinned catalog signer; the gate passes its own test key.
    Session(Transport transport, Storage& storage, Clock clock,
            Core::Online::Nextendo::SigningKey catalog_key = Core::Online::Nextendo::kCatalogKey);

    /// At start: a stored refresh token signs in again without the browser; the cached catalog
    /// is verified and used until a fresh one arrives.
    void Resume();

    /// The sign-in page opened.
    void BeginSignIn();
    /// The page closed on the redirect: the code is exchanged on the next Step.
    void FinishSignIn(const CallbackResult& callback, std::string verifier,
                      std::string redirect_uri);
    /// The page closed without a redirect (the player backed out) or failed to open.
    void CancelSignIn(bool by_player);
    /// Revokes the refresh token on the server (best effort), forgets every token.
    void SignOut();

    /// Presence is posted only while the player chose Nextendo.
    void SetPresenceWanted(bool wanted);
    /// A game with this title id runs (0: back in the menus). detail: what friends read.
    void SetPlaying(std::uint64_t title_id, std::string detail);
    /// Download the catalog on the next Step even if the copy is fresh.
    void RequestCatalog();

    /// Does every call that is due. Blocks on the transport. Returns the seconds until the next
    /// call is due (at least 1).
    std::int64_t Step();

    const SessionView& View() const {
        return view_;
    }
    std::optional<GameIdentity> Identity() const;
    const Core::Online::Nextendo::GameCatalog& Catalog() const {
        return catalog_;
    }

private:
    struct Pending {
        std::string code;
        std::string verifier;
        std::string redirect_uri;
    };

    Http::ApiRequest Api(std::string_view method, std::string_view path,
                         const std::string& bearer) const;
    /// Common handling of a failed call; returns true when the caller should stop this Step.
    bool Fail(CallOutcome outcome, const Http::ApiResponse& response);
    void Succeeded();
    void Touch();
    void Forget(Problem why);

    bool ExchangeCode();
    bool RefreshAccess();
    bool FetchGameToken();
    bool FetchOnlineStatus();
    bool FetchSessionStatus();
    bool FetchProfile();
    void PostPresence(PresenceState state);
    void PostHeartbeat();
    void FetchCatalog();
    bool AdoptCatalog(std::string_view envelope, bool from_cache);

    Transport transport_;
    Storage& storage_;
    Clock clock_;
    Core::Online::Nextendo::SigningKey catalog_key_;
    SessionView view_;

    std::optional<Pending> pending_;
    std::string refresh_token_;
    std::string access_token_;
    std::int64_t access_expires_ = 0;
    std::string nex_token_;
    std::string nsa_id_;

    std::int64_t backoff_until_ = 0;
    std::int64_t backoff_ = 0;
    std::int64_t status_at_ = 0;
    std::int64_t profile_at_ = 0;
    std::int64_t presence_at_ = 0;
    std::int64_t heartbeat_at_ = 0;
    std::int64_t catalog_at_ = 0;
    bool presence_wanted_ = false;
    bool presence_sent_online_ = false;
    std::string playing_detail_;

    Core::Online::Nextendo::GameCatalog catalog_;
};

} // namespace Jadyx::Network::Nextendo
