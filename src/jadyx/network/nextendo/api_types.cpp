// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/network/nextendo/api_types.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>

#include <nlohmann/json.hpp>

namespace Jadyx::Network::Nextendo {
namespace {

std::string StringField(const nlohmann::json& node, const char* name) {
    const auto it = node.find(name);
    return it != node.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::int64_t IntField(const nlohmann::json& node, const char* name) {
    const auto it = node.find(name);
    return it != node.end() && it->is_number_integer() ? it->get<std::int64_t>() : 0;
}

bool BoolField(const nlohmann::json& node, const char* name) {
    const auto it = node.find(name);
    return it != node.end() && it->is_boolean() && it->get<bool>();
}

nlohmann::json ParseObject(std::string_view body) {
    auto node = nlohmann::json::parse(body, nullptr, false);
    return node.is_object() ? node : nlohmann::json{};
}

bool IsHex16(std::string_view text) {
    return text.size() == 16 && std::all_of(text.begin(), text.end(), [](char c) {
               return std::isxdigit(static_cast<unsigned char>(c)) != 0;
           });
}

} // namespace

CallOutcome OutcomeForStatus(int status) {
    if (status >= 200 && status < 300) {
        return CallOutcome::Ok;
    }
    switch (status) {
    case 401:
        return CallOutcome::RefreshFirst;
    case 403:
        return CallOutcome::MissingScope;
    case 429:
        return CallOutcome::RateLimited;
    default:
        break;
    }
    if (status >= 400 && status < 500) {
        return CallOutcome::Rejected;
    }
    return CallOutcome::RetryLater; // 0 (no response), 5xx, anything unexpected
}

std::uint32_t ParseRetryAfter(std::string_view header, std::uint32_t fallback_seconds) {
    while (!header.empty() && std::isspace(static_cast<unsigned char>(header.front()))) {
        header.remove_prefix(1);
    }
    while (!header.empty() && std::isspace(static_cast<unsigned char>(header.back()))) {
        header.remove_suffix(1);
    }
    std::uint32_t seconds = 0;
    const auto [end, ec] = std::from_chars(header.data(), header.data() + header.size(), seconds);
    if (header.empty() || ec != std::errc{} || end != header.data() + header.size()) {
        return fallback_seconds;
    }
    return std::min<std::uint32_t>(seconds, 3600);
}

OAuthTokens ParseTokenResponse(int status, std::string_view body) {
    OAuthTokens tokens;
    tokens.outcome = OutcomeForStatus(status);
    const auto node = ParseObject(body);
    if (tokens.outcome != CallOutcome::Ok) {
        if (node.is_object()) {
            tokens.error = StringField(node, "error");
        }
        return tokens;
    }
    if (!node.is_object()) {
        tokens.outcome = CallOutcome::Malformed;
        return tokens;
    }
    tokens.access_token = StringField(node, "access_token");
    tokens.refresh_token = StringField(node, "refresh_token");
    tokens.expires_in = IntField(node, "expires_in");
    tokens.scope = StringField(node, "scope");
    if (tokens.access_token.empty()) {
        tokens.outcome = CallOutcome::Malformed;
    }
    return tokens;
}

GameToken ParseGameToken(int status, std::string_view body, std::string_view retry_after) {
    GameToken token;
    token.outcome = OutcomeForStatus(status);
    if (token.outcome == CallOutcome::RateLimited) {
        token.retry_after_seconds = ParseRetryAfter(retry_after);
    }
    if (token.outcome != CallOutcome::Ok) {
        return token;
    }
    const auto node = ParseObject(body);
    if (!node.is_object()) {
        token.outcome = CallOutcome::Malformed;
        return token;
    }
    token.nex_token = StringField(node, "nex_token");
    if (const auto pid = node.find("pid"); pid != node.end()) {
        if (pid->is_number_unsigned() || pid->is_number_integer()) {
            token.pid = pid->get<std::uint64_t>();
        } else if (pid->is_string()) {
            const std::string text = pid->get<std::string>();
            if (std::from_chars(text.data(), text.data() + text.size(), token.pid).ec !=
                std::errc{}) {
                token.pid = 0;
            }
        }
    }
    if (const auto code = node.find("friend_code"); code != node.end()) {
        token.friend_code = code->is_string() ? code->get<std::string>() : code->dump();
    }
    token.username = StringField(node, "username");
    token.expires_in = IntField(node, "expires_in");
    if (token.nex_token.empty() || token.pid == 0) {
        token.outcome = CallOutcome::Malformed;
    }
    return token;
}

OnlineStatus ParseOnlineStatus(int status, std::string_view body) {
    OnlineStatus result;
    result.outcome = OutcomeForStatus(status);
    if (result.outcome != CallOutcome::Ok) {
        return result;
    }
    const auto node = ParseObject(body);
    if (!node.is_object() || !node.contains("allow")) {
        result.outcome = CallOutcome::Malformed;
        return result;
    }
    result.allow = BoolField(node, "allow");
    if (result.allow) {
        result.reason = OnlineBlock::None;
        return result;
    }
    const std::string reason = StringField(node, "reason");
    if (reason == "disabled") {
        result.reason = OnlineBlock::Disabled;
    } else if (reason == "suspended") {
        result.reason = OnlineBlock::Suspended;
    } else if (reason == "unverified") {
        result.reason = OnlineBlock::Unverified;
    } else if (reason == "discord_unlinked") {
        result.reason = OnlineBlock::DiscordUnlinked;
    } else if (reason == "elsewhere") {
        result.reason = OnlineBlock::Elsewhere;
    } else {
        result.reason = OnlineBlock::Unknown;
    }
    return result;
}

SessionStatus ParseSessionStatus(int status, std::string_view body) {
    SessionStatus result;
    result.outcome = OutcomeForStatus(status);
    if (result.outcome != CallOutcome::Ok) {
        return result;
    }
    const auto node = ParseObject(body);
    if (!node.is_object()) {
        result.outcome = CallOutcome::Malformed;
        return result;
    }
    result.email_verified = BoolField(node, "email_verified");
    result.revoked = BoolField(node, "revoked");
    return result;
}

std::string BuildPresenceBody(PresenceState state, std::uint64_t title_id,
                              std::string_view detail) {
    std::size_t cut = std::min<std::size_t>(detail.size(), 64);
    // Back off continuation bytes so the cut never splits a UTF-8 sequence.
    while (cut > 0 && cut < detail.size() &&
           (static_cast<unsigned char>(detail[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    std::array<char, 17> id{};
    std::snprintf(id.data(), id.size(), "%016llx", static_cast<unsigned long long>(title_id));

    nlohmann::ordered_json body;
    body["status"] = static_cast<int>(state);
    if (state == PresenceState::Playing && title_id != 0) {
        body["app_id"] = id.data();
        body["app_detail"] = std::string(detail.substr(0, cut));
    }
    body["platform"] = "emulator";
    return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::optional<std::string> ParseNsaId(std::string_view me_body) {
    const auto node = ParseObject(me_body);
    if (!node.is_object()) {
        return std::nullopt;
    }
    const std::string nsa = StringField(node, "nsa");
    if (!IsHex16(nsa)) {
        return std::nullopt;
    }
    return nsa;
}

} // namespace Jadyx::Network::Nextendo
