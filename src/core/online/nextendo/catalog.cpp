// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/online/nextendo/catalog.h"

#include <algorithm>
#include <cctype>
#include <charconv>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "core/online/nextendo/encoding.h"

namespace Core::Online::Nextendo {
namespace {

bool VerifyEd25519(std::span<const std::uint8_t> public_key, std::string_view message,
                   std::span<const std::uint8_t> signature) {
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, public_key.data(),
                                                public_key.size());
    if (key == nullptr) {
        return false;
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    const bool ok =
        ctx != nullptr && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestVerify(ctx, signature.data(), signature.size(),
                         reinterpret_cast<const unsigned char*>(message.data()),
                         message.size()) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

std::vector<std::string_view> SplitDots(std::string_view text) {
    std::vector<std::string_view> parts;
    while (true) {
        const std::size_t dot = text.find('.');
        parts.push_back(text.substr(0, dot));
        if (dot == std::string_view::npos) {
            return parts;
        }
        text.remove_prefix(dot + 1);
    }
}

std::optional<std::uint64_t> ParseNumber(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || text.empty()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint64_t> ParseTitleId(std::string_view text) {
    if (text.size() != 16) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// Howard Hinnant's days_from_civil.
std::int64_t DaysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::vector<std::string> StringArray(const nlohmann::json& node) {
    std::vector<std::string> out;
    if (node.is_array()) {
        for (const auto& item : node) {
            if (item.is_string()) {
                out.push_back(item.get<std::string>());
            }
        }
    }
    return out;
}

} // namespace

VerifiedPayload VerifySignedDocument(std::string_view envelope_json, const SigningKey& key) {
    const auto envelope = nlohmann::json::parse(envelope_json, nullptr, false);
    if (!envelope.is_object() || !envelope.contains("key_id") || !envelope.contains("payload") ||
        !envelope.contains("signature") || !envelope["key_id"].is_string() ||
        !envelope["payload"].is_string() || !envelope["signature"].is_string()) {
        return {SignedDocumentError::Malformed, {}};
    }
    if (envelope["key_id"].get<std::string>() != key.key_id) {
        return {SignedDocumentError::WrongKey, {}};
    }
    const auto payload = Base64Decode(envelope["payload"].get<std::string>());
    const auto signature = Base64Decode(envelope["signature"].get<std::string>());
    const auto public_key = Base64Decode(key.public_key_b64);
    if (!payload || !signature || !public_key || public_key->size() != 32 ||
        signature->size() != 64) {
        return {SignedDocumentError::Malformed, {}};
    }
    const std::string text(payload->begin(), payload->end());
    if (!VerifyEd25519(*public_key, text, *signature)) {
        return {SignedDocumentError::BadSignature, {}};
    }
    return {SignedDocumentError::None, text};
}

int CompareVersions(std::string_view a, std::string_view b) {
    auto left = SplitDots(a);
    auto right = SplitDots(b);
    const std::size_t count = std::max(left.size(), right.size());
    for (std::size_t i = 0; i < count; ++i) {
        const std::string_view l = i < left.size() ? left[i] : std::string_view{"0"};
        const std::string_view r = i < right.size() ? right[i] : std::string_view{"0"};
        const auto ln = ParseNumber(l);
        const auto rn = ParseNumber(r);
        if (ln && rn) {
            if (*ln != *rn) {
                return *ln < *rn ? -1 : 1;
            }
            continue;
        }
        if (const int text = l.compare(r); text != 0) {
            return text < 0 ? -1 : 1;
        }
    }
    return 0;
}

std::optional<std::int64_t> ParseUtcTimestamp(std::string_view text) {
    // YYYY-MM-DDTHH:MM:SS[.fraction]Z
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || (text[10] != 'T' && text[10] != 't') ||
        text[13] != ':' || text[16] != ':' || (text.back() != 'Z' && text.back() != 'z')) {
        return std::nullopt;
    }
    const auto field = [&](std::size_t at, std::size_t len) {
        return ParseNumber(text.substr(at, len));
    };
    const auto year = field(0, 4);
    const auto month = field(5, 2);
    const auto day = field(8, 2);
    const auto hour = field(11, 2);
    const auto minute = field(14, 2);
    const auto second = field(17, 2);
    if (!year || !month || !day || !hour || !minute || !second || *month < 1 || *month > 12 ||
        *day < 1 || *day > 31 || *hour > 23 || *minute > 59 || *second > 60) {
        return std::nullopt;
    }
    const std::string_view rest = text.substr(19, text.size() - 20);
    if (!rest.empty() && (rest.front() != '.' || rest.size() == 1 ||
                          !std::all_of(rest.begin() + 1, rest.end(),
                                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))) {
        return std::nullopt;
    }
    const std::int64_t days = DaysFromCivil(static_cast<std::int64_t>(*year),
                                            static_cast<unsigned>(*month),
                                            static_cast<unsigned>(*day));
    return days * 86400 + static_cast<std::int64_t>(*hour * 3600 + *minute * 60 + *second);
}

const CatalogGame* GameCatalog::Find(std::uint64_t title_id) const {
    for (const auto& game : games) {
        if (std::find(game.title_ids.begin(), game.title_ids.end(), title_id) !=
            game.title_ids.end()) {
            return &game;
        }
    }
    return nullptr;
}

GameCatalog ParseGameCatalog(std::string_view payload, std::int64_t now) {
    GameCatalog catalog;
    const auto root = nlohmann::json::parse(payload, nullptr, false);
    if (!root.is_object()) {
        catalog.error = CatalogError::Malformed;
        return catalog;
    }
    if (root.value("document_type", std::string{}) != "game_catalog") {
        catalog.error = CatalogError::WrongDocumentType;
        return catalog;
    }
    const auto revision = root.find("revision");
    if (revision == root.end() || !revision->is_number_integer() || revision->get<std::int64_t>() < 1) {
        catalog.error = CatalogError::BadRevision;
        return catalog;
    }
    catalog.revision = revision->get<std::int64_t>();
    const auto expires = ParseUtcTimestamp(root.value("expires_at", std::string{}));
    if (!expires) {
        catalog.error = CatalogError::Malformed;
        return catalog;
    }
    catalog.expires_at = *expires;
    if (*expires <= now) {
        catalog.error = CatalogError::Expired;
        return catalog;
    }
    const auto games = root.find("games");
    if (games == root.end() || !games->is_array()) {
        catalog.error = CatalogError::Malformed;
        return catalog;
    }
    for (const auto& node : *games) {
        if (!node.is_object()) {
            continue;
        }
        CatalogGame game;
        for (const auto& id : StringArray(node.value("title_ids", nlohmann::json::array()))) {
            if (const auto parsed = ParseTitleId(id)) {
                game.title_ids.push_back(*parsed);
            }
        }
        if (game.title_ids.empty()) {
            continue;
        }
        game.enabled = node.value("enabled", false);
        if (const auto name = node.find("name"); name != node.end() && name->is_string()) {
            game.name = name->get<std::string>();
        }
        if (const auto protocol = node.find("protocol");
            protocol != node.end() && protocol->is_string()) {
            game.protocol = protocol->get<std::string>();
        }
        game.required_capabilities =
            StringArray(node.value("required_capabilities", nlohmann::json::array()));
        if (const auto versions = node.find("versions");
            versions != node.end() && versions->is_object()) {
            game.version_policy = versions->value("policy", std::string{});
            game.accepted_versions = StringArray(versions->value("accepted", nlohmann::json::array()));
            if (const auto recommended = versions->find("recommended");
                recommended != versions->end() && recommended->is_string()) {
                game.recommended_version = recommended->get<std::string>();
            }
        }
        catalog.games.push_back(std::move(game));
    }
    return catalog;
}

TitleVerdict CheckTitle(const GameCatalog& catalog, std::uint64_t title_id,
                        std::string_view installed_version) {
    const CatalogGame* game = catalog.error == CatalogError::None ? catalog.Find(title_id) : nullptr;
    if (game == nullptr) {
        return {TitleSupport::Unknown, {}};
    }
    if (!game->enabled) {
        return {TitleSupport::Disabled, game->recommended_version};
    }
    for (const auto& capability : game->required_capabilities) {
        if (capability != kCapabilityExactVersions) {
            return {TitleSupport::UnsupportedClient, game->recommended_version};
        }
    }
    if (!game->version_policy.empty() && game->version_policy != "exact") {
        return {TitleSupport::UnsupportedClient, game->recommended_version};
    }
    if (game->accepted_versions.empty()) {
        return {TitleSupport::Supported, game->recommended_version};
    }
    for (const auto& accepted : game->accepted_versions) {
        if (CompareVersions(accepted, installed_version) == 0) {
            return {TitleSupport::Supported, game->recommended_version};
        }
    }
    return {TitleSupport::WrongVersion, game->recommended_version};
}

} // namespace Core::Online::Nextendo
