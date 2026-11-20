// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/network/nextendo/session.h"

#include <algorithm>
#include <utility>

namespace Jadyx::Network::Nextendo {
namespace {

namespace Nx = Core::Online::Nextendo;

constexpr std::string_view kTokenPath = "/api/oauth/token";
constexpr std::string_view kRevokePath = "/api/oauth/revoke";
constexpr std::string_view kGameTokenPath = "/api/nex-token";
constexpr std::string_view kOnlineStatusPath = "/api/online-status";
constexpr std::string_view kSessionStatusPath = "/api/session-status";
constexpr std::string_view kSessionPath = "/api/nex-session";
constexpr std::string_view kPresencePath = "/api/presence";
constexpr std::string_view kProfilePath = "/api/me";
constexpr std::string_view kCatalogPath = "/service-catalog/v1/game-catalog";
constexpr std::string_view kForm = "application/x-www-form-urlencoded";
constexpr std::string_view kJson = "application/json";
constexpr std::size_t kCatalogMaxBytes = 1u << 20;

bool ClientSupports(const Nx::CatalogGame& game) {
    return std::all_of(game.required_capabilities.begin(), game.required_capabilities.end(),
                       [](const std::string& capability) {
                           return capability == Nx::kCapabilityExactVersions;
                       });
}

} // namespace

Session::Session(Transport transport, Storage& storage, Clock clock, Nx::SigningKey catalog_key)
    : transport_{std::move(transport)}, storage_{storage}, clock_{std::move(clock)},
      catalog_key_{catalog_key} {}

Http::ApiRequest Session::Api(std::string_view method, std::string_view path,
                              const std::string& bearer) const {
    Http::ApiRequest request;
    request.method = std::string(method);
    request.url = std::string(kApiBase) + std::string(path);
    request.bearer = bearer;
    request.headers.emplace_back("X-Nextendo-Client-Id", std::string(kClientId));
    return request;
}

void Session::Touch() {
    ++view_.version;
}

void Session::Succeeded() {
    backoff_ = 0;
    backoff_until_ = 0;
    if (view_.problem == Problem::Offline || view_.problem == Problem::RateLimited ||
        view_.problem == Problem::Unavailable) {
        view_.problem = Problem::None;
        view_.retry_after = 0;
        Touch();
    }
}

bool Session::Fail(CallOutcome outcome, const Http::ApiResponse& response) {
    const std::int64_t now = clock_();
    if (outcome == CallOutcome::RateLimited) {
        const auto wait = ParseRetryAfter(response.retry_after, 60);
        backoff_until_ = now + wait;
        view_.problem = Problem::RateLimited;
        view_.retry_after = wait;
        Touch();
        return true;
    }
    backoff_ = backoff_ == 0 ? Timing::FirstBackoff : std::min(backoff_ * 2, Timing::MaxBackoff);
    backoff_until_ = now + backoff_;
    const Problem problem = response.status == 0 ? Problem::Offline : Problem::Unavailable;
    if (view_.problem != problem) {
        view_.problem = problem;
        Touch();
    }
    return true;
}

void Session::Forget(Problem why) {
    storage_.ClearRefreshToken();
    pending_.reset();
    refresh_token_.clear();
    access_token_.clear();
    nex_token_.clear();
    nsa_id_.clear();
    access_expires_ = 0;
    backoff_ = backoff_until_ = 0;
    status_at_ = profile_at_ = presence_at_ = heartbeat_at_ = 0;
    presence_sent_online_ = false;
    const auto catalog_state = view_.catalog_state;
    const auto catalog_revision = view_.catalog_revision;
    const auto catalog_expires = view_.catalog_expires;
    auto catalog = std::move(view_.catalog);
    const auto version = view_.version;
    const auto playing = view_.playing_title;
    view_ = {};
    view_.catalog_state = catalog_state;
    view_.catalog_revision = catalog_revision;
    view_.catalog_expires = catalog_expires;
    view_.catalog = std::move(catalog);
    view_.playing_title = playing;
    view_.version = version;
    view_.problem = why;
    Touch();
}

void Session::Resume() {
    if (const auto envelope = storage_.LoadCatalog()) {
        AdoptCatalog(*envelope, true);
    }
    if (const auto token = storage_.LoadRefreshToken(); token && !token->empty()) {
        refresh_token_ = *token;
        view_.phase = Phase::Connecting;
        Touch();
    }
}

void Session::BeginSignIn() {
    if (view_.phase != Phase::SignedOut) {
        return;
    }
    view_.phase = Phase::SigningIn;
    view_.problem = Problem::None;
    Touch();
}

void Session::FinishSignIn(const CallbackResult& callback, std::string verifier,
                           std::string redirect_uri) {
    if (view_.phase != Phase::SigningIn) {
        return;
    }
    if (callback.code.empty()) {
        view_.phase = Phase::SignedOut;
        view_.problem = callback.error == "access_denied" ? Problem::SignInCancelled
                                                          : Problem::SignInRejected;
        Touch();
        return;
    }
    pending_ = Pending{callback.code, std::move(verifier), std::move(redirect_uri)};
    view_.phase = Phase::Connecting;
    Touch();
}

void Session::CancelSignIn(bool by_player) {
    if (view_.phase != Phase::SigningIn) {
        return;
    }
    view_.phase = Phase::SignedOut;
    view_.problem = by_player ? Problem::SignInCancelled : Problem::Offline;
    Touch();
}

void Session::SignOut() {
    if (presence_sent_online_ && !access_token_.empty()) {
        PostPresence(PresenceState::Offline);
    }
    if (!refresh_token_.empty()) {
        // Always 200, even for an unknown token: the answer does not matter.
        auto request = Api("POST", kRevokePath, {});
        request.content_type = std::string(kForm);
        request.body = "token=" + UrlEncode(refresh_token_) + "&client_id=" + UrlEncode(kClientId);
        transport_(request);
    }
    Forget(Problem::None);
}

void Session::SetPresenceWanted(bool wanted) {
    if (presence_wanted_ == wanted) {
        return;
    }
    presence_wanted_ = wanted;
    presence_at_ = 0;
}

void Session::SetPlaying(std::uint64_t title_id, std::string detail) {
    if (view_.playing_title == title_id && playing_detail_ == detail) {
        return;
    }
    view_.playing_title = title_id;
    playing_detail_ = std::move(detail);
    presence_at_ = 0;
    heartbeat_at_ = 0;
    Touch();
}

void Session::RequestCatalog() {
    catalog_at_ = 0;
}

std::optional<GameIdentity> Session::Identity() const {
    if (view_.phase != Phase::Ready || nex_token_.empty()) {
        return std::nullopt;
    }
    return GameIdentity{nex_token_, view_.pid, nsa_id_};
}

bool Session::ExchangeCode() {
    auto request = Api("POST", kTokenPath, {});
    request.content_type = std::string(kForm);
    request.body = BuildCodeExchangeBody(pending_->code, pending_->redirect_uri, pending_->verifier);
    const auto response = transport_(request);
    const auto tokens = ParseTokenResponse(static_cast<int>(response.status), response.body);
    if (tokens.outcome == CallOutcome::Ok) {
        pending_.reset();
        refresh_token_ = tokens.refresh_token;
        access_token_ = tokens.access_token;
        access_expires_ = clock_() + tokens.expires_in;
        if (!storage_.SaveRefreshToken(refresh_token_)) {
            view_.problem = Problem::StorageFailed;
        }
        Succeeded();
        Touch();
        return true;
    }
    if (tokens.outcome == CallOutcome::RetryLater || tokens.outcome == CallOutcome::RateLimited) {
        return !Fail(tokens.outcome, response);
    }
    // invalid_grant, an unknown application, a malformed answer: the code is spent.
    pending_.reset();
    view_.phase = Phase::SignedOut;
    view_.problem = Problem::SignInRejected;
    Touch();
    return false;
}

bool Session::RefreshAccess() {
    auto request = Api("POST", kTokenPath, {});
    request.content_type = std::string(kForm);
    request.body = BuildRefreshBody(refresh_token_);
    const auto response = transport_(request);
    const auto tokens = ParseTokenResponse(static_cast<int>(response.status), response.body);
    switch (tokens.outcome) {
    case CallOutcome::Ok:
        // The refresh token rotates: the old one is dead from now on.
        refresh_token_ = tokens.refresh_token.empty() ? refresh_token_ : tokens.refresh_token;
        access_token_ = tokens.access_token;
        access_expires_ = clock_() + tokens.expires_in;
        if (!storage_.SaveRefreshToken(refresh_token_)) {
            view_.problem = Problem::StorageFailed;
            Touch();
        }
        Succeeded();
        return true;
    case CallOutcome::RetryLater:
    case CallOutcome::RateLimited:
        return !Fail(tokens.outcome, response);
    default:
        // invalid_grant (expired or reused), the application removed or suspended.
        Forget(Problem::SessionExpired);
        return false;
    }
}

bool Session::FetchGameToken() {
    const auto response = transport_(Api("GET", kGameTokenPath, access_token_));
    const auto token =
        ParseGameToken(static_cast<int>(response.status), response.body, response.retry_after);
    switch (token.outcome) {
    case CallOutcome::Ok: {
        const std::int64_t now = clock_();
        nex_token_ = token.nex_token;
        view_.pid = token.pid;
        view_.username = token.username;
        view_.friend_code = token.friend_code;
        view_.game_token_expires = now + token.expires_in;
        view_.phase = Phase::Ready;
        status_at_ = 0;
        presence_at_ = 0;
        Succeeded();
        Touch();
        return true;
    }
    case CallOutcome::RefreshFirst:
        access_expires_ = 0;
        return false;
    case CallOutcome::MissingScope:
        Forget(Problem::MissingScope);
        return false;
    default:
        return !Fail(token.outcome, response);
    }
}

bool Session::FetchOnlineStatus() {
    const auto response = transport_(Api("GET", kOnlineStatusPath, nex_token_));
    const auto status = ParseOnlineStatus(static_cast<int>(response.status), response.body);
    if (status.outcome == CallOutcome::RefreshFirst) {
        nex_token_.clear();
        view_.game_token_expires = 0;
        return false;
    }
    if (status.outcome != CallOutcome::Ok) {
        return !Fail(status.outcome, response);
    }
    if (!view_.status_known || view_.online_allowed != status.allow ||
        view_.block != status.reason) {
        view_.status_known = true;
        view_.online_allowed = status.allow;
        view_.block = status.allow ? OnlineBlock::None : status.reason;
        Touch();
    }
    Succeeded();
    return true;
}

bool Session::FetchSessionStatus() {
    const auto response = transport_(Api("GET", kSessionStatusPath, nex_token_));
    const auto status = ParseSessionStatus(static_cast<int>(response.status), response.body);
    if (status.outcome == CallOutcome::RefreshFirst) {
        nex_token_.clear();
        view_.game_token_expires = 0;
        return false;
    }
    if (status.outcome != CallOutcome::Ok) {
        return !Fail(status.outcome, response);
    }
    if (status.revoked) {
        Forget(Problem::Revoked);
        return false;
    }
    if (view_.email_verified != status.email_verified) {
        view_.email_verified = status.email_verified;
        Touch();
    }
    Succeeded();
    return true;
}

bool Session::FetchProfile() {
    const auto response = transport_(Api("GET", kProfilePath, access_token_));
    const auto profile = ParseProfile(static_cast<int>(response.status), response.body);
    switch (profile.outcome) {
    case CallOutcome::Ok:
        if (!profile.username.empty()) {
            view_.username = profile.username;
        }
        view_.display_name = profile.display_name;
        if (!profile.friend_code.empty()) {
            view_.friend_code = profile.friend_code;
        }
        if (view_.pid == 0) {
            view_.pid = profile.pid;
        }
        nsa_id_ = profile.nsa_id;
        Succeeded();
        Touch();
        return true;
    case CallOutcome::RefreshFirst:
        access_expires_ = 0;
        return false;
    case CallOutcome::RetryLater:
    case CallOutcome::RateLimited:
        return !Fail(profile.outcome, response);
    default:
        // No identity scope or an unexpected answer: the game token already gave the basics.
        return true;
    }
}

void Session::PostPresence(PresenceState state) {
    auto request = Api("POST", kPresencePath, access_token_);
    request.content_type = std::string(kJson);
    request.body = BuildPresenceBody(state, view_.playing_title, playing_detail_);
    const auto response = transport_(request);
    const auto outcome = OutcomeForStatus(static_cast<int>(response.status));
    const bool live = outcome == CallOutcome::Ok && state != PresenceState::Offline;
    presence_sent_online_ = live;
    if (outcome == CallOutcome::RefreshFirst) {
        access_expires_ = 0;
    } else if (outcome == CallOutcome::RateLimited) {
        Fail(outcome, response);
    }
    if (view_.presence_live != live) {
        view_.presence_live = live;
        Touch();
    }
}

void Session::PostHeartbeat() {
    auto request = Api("POST", kSessionPath, nex_token_);
    request.content_type = std::string(kJson);
    request.body = "{}";
    const auto response = transport_(request);
    const auto outcome = OutcomeForStatus(static_cast<int>(response.status));
    if (outcome == CallOutcome::RefreshFirst) {
        nex_token_.clear();
        view_.game_token_expires = 0;
    } else if (outcome == CallOutcome::RateLimited) {
        Fail(outcome, response);
    }
}

bool Session::AdoptCatalog(std::string_view envelope, bool from_cache) {
    const auto verified = Nx::VerifySignedDocument(envelope, catalog_key_);
    if (verified.error != Nx::SignedDocumentError::None) {
        return false;
    }
    auto parsed = Nx::ParseGameCatalog(verified.payload, clock_());
    if (!Nx::ShouldReplaceCatalog(catalog_, parsed)) {
        // Expired, malformed or older than what is shown. An identical revision is a refresh.
        return parsed.error == Nx::CatalogError::None;
    }
    catalog_ = std::move(parsed);
    view_.catalog.clear();
    view_.catalog.reserve(catalog_.games.size());
    for (const auto& game : catalog_.games) {
        view_.catalog.push_back({.name = game.name,
                                 .title_ids = game.title_ids,
                                 .enabled = game.enabled,
                                 .client_supported = ClientSupports(game),
                                 .accepted_versions = game.accepted_versions,
                                 .recommended_version = game.recommended_version});
    }
    view_.catalog_state = CatalogState::Ready;
    view_.catalog_revision = catalog_.revision;
    view_.catalog_expires = catalog_.expires_at;
    if (!from_cache) {
        storage_.SaveCatalog(envelope);
    }
    Touch();
    return true;
}

void Session::FetchCatalog() {
    const std::int64_t now = clock_();
    if (view_.catalog_state != CatalogState::Ready) {
        view_.catalog_state = CatalogState::Loading;
        Touch();
    }
    Http::ApiRequest request = Api("GET", kCatalogPath, {});
    request.headers.clear(); // public document
    request.max_bytes = kCatalogMaxBytes;
    const auto response = transport_(request);
    const bool ok = response.status == 200 && AdoptCatalog(response.body, false);
    if (ok) {
        catalog_at_ = now + Timing::CatalogEvery;
        if (catalog_.expires_at != 0) {
            catalog_at_ = std::min(catalog_at_, std::max(now + Timing::CatalogRetry,
                                                         catalog_.expires_at - 3600));
        }
        return;
    }
    catalog_at_ = now + Timing::CatalogRetry;
    // A failed download keeps a valid copy on screen.
    if (view_.catalog_state != CatalogState::Ready) {
        view_.catalog_state = CatalogState::Failed;
        Touch();
    }
}

std::int64_t Session::Step() {
    std::int64_t now = clock_();
    if (catalog_at_ <= now) {
        FetchCatalog();
        now = clock_();
    }
    std::int64_t next = catalog_at_;
    const auto wait = [&](std::int64_t at) { next = std::min(next, at); };
    const auto result = [&] { return std::max<std::int64_t>(1, std::min(next - now, Timing::Idle)); };

    if (backoff_until_ > now) {
        wait(backoff_until_);
        return result();
    }
    if (pending_) {
        if (!ExchangeCode()) {
            wait(backoff_until_ > now ? backoff_until_ : now + Timing::Idle);
            return result();
        }
    }
    if (refresh_token_.empty()) {
        return result();
    }
    // Each call that fails ends this Step; the next one (or the back-off) picks up from there.
    // A 401 makes the call clear what it used, and the loop below asks again at once.
    for (int round = 0; round < 3; ++round) {
        now = clock_();
        if (access_token_.empty() || access_expires_ - Timing::AccessMargin <= now) {
            if (!RefreshAccess()) {
                break;
            }
        }
        if (nex_token_.empty() || view_.game_token_expires - Timing::GameTokenMargin <= now) {
            if (!FetchGameToken()) {
                if (refresh_token_.empty() || backoff_until_ > now) {
                    break;
                }
                continue;
            }
        }
        if (status_at_ <= now) {
            if (!FetchOnlineStatus() || !FetchSessionStatus()) {
                if (refresh_token_.empty() || backoff_until_ > now) {
                    break;
                }
                continue;
            }
            status_at_ = now + Timing::StatusEvery;
        }
        if (profile_at_ <= now) {
            if (!FetchProfile()) {
                if (backoff_until_ > now) {
                    break;
                }
                continue;
            }
            profile_at_ = now + Timing::ProfileEvery;
        }
        break;
    }
    now = clock_();
    if (refresh_token_.empty() || backoff_until_ > now) {
        wait(backoff_until_ > now ? backoff_until_ : now + Timing::Idle);
        return result();
    }
    if (view_.phase == Phase::Ready && !access_token_.empty()) {
        if (presence_wanted_ && presence_at_ <= now) {
            PostPresence(view_.playing_title != 0 ? PresenceState::Playing : PresenceState::Online);
            presence_at_ = now + Timing::PresenceEvery;
        } else if (!presence_wanted_ && presence_sent_online_) {
            PostPresence(PresenceState::Offline);
        }
        if (view_.playing_title != 0 && heartbeat_at_ <= now && !nex_token_.empty()) {
            PostHeartbeat();
            heartbeat_at_ = now + Timing::HeartbeatEvery;
        }
    }
    if (presence_wanted_ && view_.phase == Phase::Ready) {
        wait(presence_at_);
    }
    if (view_.playing_title != 0 && view_.phase == Phase::Ready) {
        wait(heartbeat_at_);
    }
    wait(access_expires_ - Timing::AccessMargin);
    wait(view_.game_token_expires - Timing::GameTokenMargin);
    wait(status_at_);
    wait(profile_at_);
    if (backoff_until_ > now) {
        wait(backoff_until_);
    }
    return result();
}

} // namespace Jadyx::Network::Nextendo
