// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The BAAS id_token the game reads through acc (LoadIdTokenCache / GetIdTokenData). NEX parses it
// before logging in, so it must be a well-formed RS256 JWT; the Nextendo servers authenticate the
// player through the nx2 game token carried in the "nnex" claim, not through this signature, so
// our own RSA key is enough.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Core::Online::Nextendo {

inline constexpr std::string_view kBaasIssuer =
    "https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";
inline constexpr std::string_view kBaasJku =
    "https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com/1.0.0/certificates";
inline constexpr std::string_view kBaasAudience = "ed9e2f05d286f7b8";
inline constexpr std::string_view kSerialNumber = "XAW10000000000";
inline constexpr std::int64_t kIdTokenLifetimeSeconds = 3 * 60 * 60;

class TokenSigner {
public:
    virtual ~TokenSigner() = default;
    virtual std::string KeyId() const = 0;
    /// RSASSA-PKCS1-v1_5 with SHA-256 over input. std::nullopt on failure.
    virtual std::optional<std::vector<std::uint8_t>> SignRs256(std::string_view input) = 0;
};

/// RSA key held in memory. The app persists PrivatePem() so the key survives restarts.
class RsaSigner final : public TokenSigner {
public:
    static std::unique_ptr<RsaSigner> Generate(std::string key_id, int bits = 2048);
    static std::unique_ptr<RsaSigner> FromPem(std::string key_id, std::string_view private_pem);

    ~RsaSigner() override;
    RsaSigner(const RsaSigner&) = delete;
    RsaSigner& operator=(const RsaSigner&) = delete;

    std::string KeyId() const override {
        return key_id_;
    }
    std::optional<std::vector<std::uint8_t>> SignRs256(std::string_view input) override;

    bool VerifyRs256(std::string_view input, const std::vector<std::uint8_t>& signature) const;
    std::string PrivatePem() const;

private:
    RsaSigner(std::string key_id, void* key) : key_id_{std::move(key_id)}, key_{key} {}

    std::string key_id_;
    void* key_; // EVP_PKEY*, kept opaque so OpenSSL headers stay out of this header
};

struct IdTokenRequest {
    std::uint64_t title_id = 0;
    std::string title_version; // installed display version, "" leaves out "tv"
    std::string nex_token;     // nx2 game token, opaque; "" leaves out "nnex"
    std::int64_t issued_at = 0; // unix seconds
};

/// Titles that share another title's NPLN tenant (the online classics apps and two more) and
/// therefore need an explicit "app_id" claim.
bool NeedsAppIdClaim(std::uint64_t title_id);

/// Builds and signs the token. std::nullopt if randomness or signing fails.
std::optional<std::string> BuildIdToken(const IdTokenRequest& request, TokenSigner& signer);

} // namespace Core::Online::Nextendo
