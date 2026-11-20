// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/online/nextendo/id_token.h"

#include <array>
#include <cstdio>

#include <nlohmann/json.hpp>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "core/online/nextendo/encoding.h"

namespace Core::Online::Nextendo {
namespace {

EVP_PKEY* Key(void* key) {
    return static_cast<EVP_PKEY*>(key);
}

std::optional<std::string> RandomHex(std::size_t bytes) {
    const auto random = RandomBytes(bytes);
    if (random.size() != bytes) {
        return std::nullopt;
    }
    return HexEncode(random);
}

std::optional<std::string> RandomUuid() {
    auto random = RandomBytes(16);
    if (random.size() != 16) {
        return std::nullopt;
    }
    random[6] = static_cast<std::uint8_t>((random[6] & 0x0F) | 0x40); // version 4
    random[8] = static_cast<std::uint8_t>((random[8] & 0x3F) | 0x80); // RFC 4122 variant
    const std::string hex = HexEncode(random);
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
           hex.substr(16, 4) + "-" + hex.substr(20);
}

std::string TitleIdHex(std::uint64_t title_id) {
    std::array<char, 17> text{};
    std::snprintf(text.data(), text.size(), "%016llX", static_cast<unsigned long long>(title_id));
    return text.data();
}

} // namespace

std::unique_ptr<RsaSigner> RsaSigner::Generate(std::string key_id, int bits) {
    EVP_PKEY* key = EVP_RSA_gen(static_cast<unsigned>(bits));
    if (key == nullptr) {
        return nullptr;
    }
    return std::unique_ptr<RsaSigner>(new RsaSigner(std::move(key_id), key));
}

std::unique_ptr<RsaSigner> RsaSigner::FromPem(std::string key_id, std::string_view private_pem) {
    BIO* bio = BIO_new_mem_buf(private_pem.data(), static_cast<int>(private_pem.size()));
    if (bio == nullptr) {
        return nullptr;
    }
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (key == nullptr || EVP_PKEY_get_base_id(key) != EVP_PKEY_RSA) {
        EVP_PKEY_free(key);
        return nullptr;
    }
    return std::unique_ptr<RsaSigner>(new RsaSigner(std::move(key_id), key));
}

RsaSigner::~RsaSigner() {
    EVP_PKEY_free(Key(key_));
}

std::optional<std::vector<std::uint8_t>> RsaSigner::SignRs256(std::string_view input) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        return std::nullopt;
    }
    std::optional<std::vector<std::uint8_t>> result;
    std::size_t length = 0;
    const auto* data = reinterpret_cast<const unsigned char*>(input.data());
    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, Key(key_)) == 1 &&
        EVP_DigestSign(ctx, nullptr, &length, data, input.size()) == 1) {
        std::vector<std::uint8_t> signature(length);
        if (EVP_DigestSign(ctx, signature.data(), &length, data, input.size()) == 1) {
            signature.resize(length);
            result = std::move(signature);
        }
    }
    EVP_MD_CTX_free(ctx);
    return result;
}

bool RsaSigner::VerifyRs256(std::string_view input,
                            const std::vector<std::uint8_t>& signature) const {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        return false;
    }
    const bool ok =
        EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, Key(key_)) == 1 &&
        EVP_DigestVerify(ctx, signature.data(), signature.size(),
                         reinterpret_cast<const unsigned char*>(input.data()), input.size()) == 1;
    EVP_MD_CTX_free(ctx);
    return ok;
}

std::string RsaSigner::PrivatePem() const {
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio == nullptr) {
        return {};
    }
    std::string pem;
    if (PEM_write_bio_PrivateKey(bio, Key(key_), nullptr, nullptr, 0, nullptr, nullptr) == 1) {
        char* data = nullptr;
        const long size = BIO_get_mem_data(bio, &data);
        if (size > 0 && data != nullptr) {
            pem.assign(data, static_cast<std::size_t>(size));
        }
    }
    BIO_free(bio);
    return pem;
}

bool NeedsAppIdClaim(std::uint64_t title_id) {
    switch (title_id) {
    case 0x0100D870045B6000ULL:
    case 0x01008D300C50C000ULL:
    case 0x0100C62011050000ULL:
    case 0x010012F017576000ULL:
    case 0x0100C9A00ECE6000ULL:
    case 0x0100B3C014BDA000ULL:
    case 0x0100A3D008C5C000ULL:
    case 0x0100559011740000ULL:
        return true;
    default:
        return false;
    }
}

std::optional<std::string> BuildIdToken(const IdTokenRequest& request, TokenSigner& signer) {
    const auto subject = RandomHex(16);
    const auto device_id = RandomHex(16);
    const auto boot_device_id = RandomHex(16);
    const auto token_id = RandomUuid();
    if (!subject || !device_id || !boot_device_id || !token_id) {
        return std::nullopt;
    }

    nlohmann::ordered_json header;
    header["alg"] = "RS256";
    header["kid"] = signer.KeyId();
    header["typ"] = "id_token";
    header["jku"] = kBaasJku;

    nlohmann::ordered_json payload;
    payload["sub"] = *subject;
    payload["aud"] = kBaasAudience;
    payload["iss"] = kBaasIssuer;
    payload["typ"] = "id_token";
    payload["iat"] = request.issued_at;
    payload["exp"] = request.issued_at + kIdTokenLifetimeSeconds;
    payload["jku"] = kBaasJku;
    payload["jti"] = *token_id;
    payload["di"] = *device_id;
    payload["sn"] = kSerialNumber;
    payload["bs:did"] = *boot_device_id;
    // The console-side account module checks this dictionary's shape before the server ever
    // sees the token; it reuses the top-level device id.
    payload["nintendo"] = {{"dt", "NX Prod 1"},
                           {"pc", "HAC"},
                           {"di", *device_id},
                           {"sn", kSerialNumber},
                           {"ist", false}};
    if (!request.nex_token.empty()) {
        payload["nnex"] = request.nex_token;
    }
    if (!request.title_version.empty()) {
        payload["tv"] = request.title_version;
    }
    if (NeedsAppIdClaim(request.title_id)) {
        payload["app_id"] = TitleIdHex(request.title_id);
    }
    payload["hm"] = true;

    constexpr auto kReplace = nlohmann::json::error_handler_t::replace;
    const std::string signing_input = Base64UrlEncode(header.dump(-1, ' ', false, kReplace)) +
                                      "." +
                                      Base64UrlEncode(payload.dump(-1, ' ', false, kReplace));
    const auto signature = signer.SignRs256(signing_input);
    if (!signature) {
        return std::nullopt;
    }
    return signing_input + "." + Base64UrlEncode(*signature);
}

} // namespace Core::Online::Nextendo
