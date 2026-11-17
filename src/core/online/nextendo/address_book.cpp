// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/online/nextendo/address_book.h"

#include "core/online/nextendo/host_rules.h"

namespace Core::Online::Nextendo {

void AddressBook::Remember(std::string_view ip, std::string_view host) {
    if (ip.empty() || host.empty() || IsLiteralIPv4(host)) {
        return;
    }
    std::scoped_lock lock{mutex_};
    const std::string key{ip};
    if (auto it = by_ip_.find(key); it != by_ip_.end()) {
        it->second.host = host;
        order_.splice(order_.begin(), order_, it->second.age);
        return;
    }
    if (by_ip_.size() >= capacity_) {
        by_ip_.erase(order_.back());
        order_.pop_back();
    }
    order_.push_front(key);
    by_ip_.emplace(key, Slot{std::string(host), order_.begin()});
}

std::optional<std::string> AddressBook::Lookup(std::string_view ip) const {
    std::scoped_lock lock{mutex_};
    if (const auto it = by_ip_.find(std::string(ip)); it != by_ip_.end()) {
        return it->second.host;
    }
    return std::nullopt;
}

void AddressBook::Clear() {
    std::scoped_lock lock{mutex_};
    by_ip_.clear();
    order_.clear();
}

std::size_t AddressBook::Size() const {
    std::scoped_lock lock{mutex_};
    return by_ip_.size();
}

} // namespace Core::Online::Nextendo
