// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Signed Nextendo documents: the game catalog (which titles and versions are accepted) and the
// NPLN patch index. Each is {key_id, payload (base64), signature (base64)} with an Ed25519
// signature over the decoded payload bytes.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Core::Online::Nextendo {

struct SigningKey {
    std::string_view key_id;
    std::string_view public_key_b64; // raw 32-byte Ed25519 key
};

inline constexpr SigningKey kCatalogKey{"catalog-key-1",
                                        "xI+atR6VesrCH89ZDZD4t3WQLcYOFEhk5F0HLMzgWk4="};
inline constexpr SigningKey kNplnPatchesKey{"npln-patches-key-1",
                                            "LAI+Pi+GZtj3p4DpNM5ORBraw1cbZCVSMFgUmAVWNeA="};

/// The only capability this client implements. A game requiring anything else is treated as
/// unsupported rather than guessed at.
inline constexpr std::string_view kCapabilityExactVersions = "versions.exact.v1";

enum class SignedDocumentError { None, Malformed, WrongKey, BadSignature };

struct VerifiedPayload {
    SignedDocumentError error = SignedDocumentError::None;
    std::string payload; // decoded bytes, valid only when error == None
};

/// Checks the envelope's key_id against key and the Ed25519 signature over the payload.
VerifiedPayload VerifySignedDocument(std::string_view envelope_json, const SigningKey& key);

/// Numeric dotted comparison: "4.0.1" == "4.0.1.0", "1.10" > "1.9". Non-numeric parts compare
/// as text. Returns <0, 0 or >0.
int CompareVersions(std::string_view a, std::string_view b);

/// "2026-11-16T19:09:07.183287Z" -> unix seconds (fraction dropped). UTC only.
std::optional<std::int64_t> ParseUtcTimestamp(std::string_view text);

struct CatalogGame {
    std::vector<std::uint64_t> title_ids;
    std::string name; // display name from the signed catalog; may be empty
    bool enabled = false;
    std::string protocol;
    std::vector<std::string> required_capabilities;
    std::string version_policy;
    std::vector<std::string> accepted_versions;
    std::string recommended_version;
};

enum class CatalogError { None, Malformed, WrongDocumentType, BadRevision, Expired };

struct GameCatalog {
    CatalogError error = CatalogError::None;
    std::int64_t revision = 0;
    std::int64_t expires_at = 0;
    std::vector<CatalogGame> games;

    const CatalogGame* Find(std::uint64_t title_id) const;
};

/// Parses a verified game_catalog payload; now is unix seconds.
GameCatalog ParseGameCatalog(std::string_view payload, std::int64_t now);

enum class TitleSupport {
    Unknown,           // not in the catalog
    Disabled,          // listed but switched off on the server
    UnsupportedClient, // needs a capability this client does not have
    WrongVersion,      // installed version is not accepted
    Supported,
};

struct TitleVerdict {
    TitleSupport support = TitleSupport::Unknown;
    std::string recommended_version;
};

TitleVerdict CheckTitle(const GameCatalog& catalog, std::uint64_t title_id,
                        std::string_view installed_version);

/// A catalog replacement is accepted only if it does not go back in revision.
inline bool ShouldReplaceCatalog(const GameCatalog& current, const GameCatalog& incoming) {
    return incoming.error == CatalogError::None && incoming.revision >= current.revision;
}

} // namespace Core::Online::Nextendo
