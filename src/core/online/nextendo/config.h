// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the app hands to the emulated services while Nextendo is active. The app publishes a whole
// new snapshot; services take one snapshot per request, so a request never sees half an update.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "core/online/nextendo/address_book.h"
#include "core/online/nextendo/host_rules.h"
#include "core/online/nextendo/trust_store.h"

namespace Core::Online::Nextendo {

struct Config {
    bool enabled = false;
    Endpoints endpoints;
    TlsVerifyMode tls_mode = TlsVerifyMode::LogOnly;
    std::string nex_token;   // nx2, 24 h; "" until the app has one
    std::uint64_t pid = 0;   // Nextendo principal id, used as the NSA id
    std::string nsa_id_hex;  // optional 16-hex NSA from /api/me for titles that need it
};

class ConfigStore {
public:
    std::shared_ptr<const Config> Snapshot() const {
        std::scoped_lock lock{mutex_};
        return current_;
    }

    void Publish(Config config) {
        auto next = std::make_shared<const Config>(std::move(config));
        std::scoped_lock lock{mutex_};
        current_ = std::move(next);
    }

    bool Enabled() const {
        return Snapshot()->enabled;
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const Config> current_ = std::make_shared<const Config>();
};

/// Process-wide instances shared by the app and the emulated services.
ConfigStore& GlobalConfig();
AddressBook& GlobalAddressBook();

} // namespace Core::Online::Nextendo
