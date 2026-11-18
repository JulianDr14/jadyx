// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/online/nextendo/trust_store.h"

#include <algorithm>
#include <cctype>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "core/online/nextendo/trust_roots.inc"

namespace Core::Online::Nextendo {
namespace {

X509* ReadCertificate(std::string_view pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        return nullptr;
    }
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return cert;
}

std::string Fingerprint(X509* cert) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (X509_digest(cert, EVP_sha256(), digest, &length) != 1) {
        return {};
    }
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::string out;
    for (unsigned int i = 0; i < length; ++i) {
        out += digits[digest[i] >> 4];
        out += digits[digest[i] & 15];
    }
    return out;
}

// Parsed root if its fingerprint matches the pin, else nullptr.
X509* LoadPinned(const Roots::Root& root) {
    X509* cert = ReadCertificate(root.pem);
    if (cert != nullptr && Fingerprint(cert) != root.sha256) {
        X509_free(cert);
        return nullptr;
    }
    return cert;
}

} // namespace

std::size_t CountValidRoots() {
    std::size_t count = 0;
    for (const auto& root : Roots::kRoots) {
        if (X509* cert = LoadPinned(root)) {
            ++count;
            X509_free(cert);
        }
    }
    return count;
}

x509_store_st* BuildRootStore() {
    X509_STORE* store = X509_STORE_new();
    if (store == nullptr) {
        return nullptr;
    }
    std::size_t added = 0;
    for (const auto& root : Roots::kRoots) {
        if (X509* cert = LoadPinned(root)) {
            added += X509_STORE_add_cert(store, cert) == 1 ? 1 : 0;
            X509_free(cert);
        }
    }
    if (added == 0) {
        X509_STORE_free(store);
        return nullptr;
    }
    return store;
}

std::vector<unsigned char> RootDer(std::string_view label) {
    for (const auto& root : Roots::kRoots) {
        if (root.label != label) {
            continue;
        }
        X509* cert = LoadPinned(root);
        if (cert == nullptr) {
            return {};
        }
        std::vector<unsigned char> der;
        const int size = i2d_X509(cert, nullptr);
        if (size > 0) {
            der.resize(static_cast<std::size_t>(size));
            unsigned char* out = der.data();
            if (i2d_X509(cert, &out) != size) {
                der.clear();
            }
        }
        X509_free(cert);
        return der;
    }
    return {};
}

std::string CertificateSha256(std::string_view pem) {
    X509* cert = ReadCertificate(pem);
    if (cert == nullptr) {
        return {};
    }
    std::string out = Fingerprint(cert);
    X509_free(cert);
    return out;
}

} // namespace Core::Online::Nextendo
