// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Certificate trust for game TLS while Nextendo is active: the five roots published on the
// Nextendo wiki, embedded at build time and pinned by SHA-256. The client's own HTTPS calls to
// nextendo.network do not use this store; they go through the system store.

#include <cstddef>
#include <string>
#include <string_view>

struct x509_store_st;

namespace Core::Online::Nextendo {

enum class TlsVerifyMode {
    Off,     // old behaviour: no verification
    LogOnly, // verify and log failures, but let the handshake continue
    Enforce, // verify; failures abort the handshake
};

/// Number of embedded roots whose PEM parses and whose SHA-256 matches its pin.
std::size_t CountValidRoots();

/// New X509_STORE holding every valid embedded root (caller frees with X509_STORE_free).
/// nullptr if none could be loaded.
x509_store_st* BuildRootStore();

/// Upper-case hex SHA-256 of the DER of the first certificate in pem; "" on parse failure.
std::string CertificateSha256(std::string_view pem);

} // namespace Core::Online::Nextendo
