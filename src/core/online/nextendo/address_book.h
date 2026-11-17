// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Remembers the last hostname resolved to each address. Nextendo routes every TLS connection on
// port 443 by SNI, so when a game connects by IP without setting a hostname, the SSL service asks
// this book which name to send.

#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Core::Online::Nextendo {

class AddressBook {
public:
    explicit AddressBook(std::size_t capacity = 256) : capacity_{capacity == 0 ? 1 : capacity} {}

    /// Records host as the latest name for ip. Literal-IP "hosts" are ignored.
    void Remember(std::string_view ip, std::string_view host);

    std::optional<std::string> Lookup(std::string_view ip) const;

    void Clear();
    std::size_t Size() const;

private:
    struct Slot {
        std::string host;
        std::list<std::string>::iterator age;
    };

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Slot> by_ip_;
    std::list<std::string> order_; // front = newest
};

} // namespace Core::Online::Nextendo
