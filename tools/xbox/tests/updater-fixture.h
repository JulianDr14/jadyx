// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cassert>
#include <memory>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <string>
struct UpdateSigner {
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr,
                                                          EVP_PKEY_free};
  std::string public_key;
  UpdateSigner() {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> gen{
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free};
    assert(gen && EVP_PKEY_keygen_init(gen.get()) == 1 &&
           EVP_PKEY_CTX_set_rsa_keygen_bits(gen.get(), 2048) == 1);
    EVP_PKEY *raw{};
    assert(EVP_PKEY_keygen(gen.get(), &raw) == 1);
    key.reset(raw);
    std::unique_ptr<BIO, decltype(&BIO_free)> bio{BIO_new(BIO_s_mem()),
                                                  BIO_free};
    assert(PEM_write_bio_PUBKEY(bio.get(), key.get()) == 1);
    char *bytes{};
    const auto n = BIO_get_mem_data(bio.get(), &bytes);
    public_key.assign(bytes, n);
  }
  std::string Sign(const std::string &body) const {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{
        EVP_MD_CTX_new(), EVP_MD_CTX_free};
    assert(EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr,
                              key.get()) == 1);
    std::size_t n{};
    assert(EVP_DigestSign(ctx.get(), nullptr, &n,
                          reinterpret_cast<const unsigned char *>(body.data()),
                          body.size()) == 1);
    std::string out(n, '\0');
    assert(EVP_DigestSign(ctx.get(),
                          reinterpret_cast<unsigned char *>(out.data()), &n,
                          reinterpret_cast<const unsigned char *>(body.data()),
                          body.size()) == 1);
    out.resize(n);
    return out;
  }
};
