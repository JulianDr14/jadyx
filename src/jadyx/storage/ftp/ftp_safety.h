// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include "jadyx/storage/ftp/ftp_protocol.h"

namespace Jadyx::Ftp {
inline std::optional<std::uint64_t> ParseAllocation(std::string_view argument) {
    std::uint64_t bytes{};
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), bytes);
    if (argument.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != argument.data() + argument.size())
        return std::nullopt;
    return bytes;
}

template <typename Active, typename Publish, typename Discard>
void CompleteUpload(std::optional<std::uint64_t> expected, std::uint64_t bytes, Active active,
                    Publish publish, Discard discard) {
    try {
        if (!expected || bytes != *expected || !active())
            throw std::runtime_error("upload incomplete");
        publish();
    } catch (...) {
        discard();
        throw;
    }
}

class SessionDeadline {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    explicit SessionDeadline(Time now = Clock::now()) : deadline_(now + std::chrono::seconds(30)) {}
    void Login(bool authenticated, Time now = Clock::now()) {
        std::lock_guard lock(mutex_);
        authenticated_ = authenticated;
        deadline_ = now + (authenticated ? std::chrono::seconds(300) : std::chrono::seconds(30));
    }
    void Activity(Time now = Clock::now()) {
        std::lock_guard lock(mutex_);
        if (authenticated_)
            deadline_ = now + std::chrono::seconds(300);
    }
    bool Expired(Time now = Clock::now()) const {
        std::lock_guard lock(mutex_);
        return now >= deadline_;
    }
    std::chrono::steady_clock::duration Remaining(Time now = Clock::now()) const {
        std::lock_guard lock(mutex_);
        return std::max(Time::duration::zero(), deadline_ - now);
    }

private:
    mutable std::mutex mutex_;
    Time deadline_;
    bool authenticated_ = false;
};

// Owned by the server, not a session. Bounded storage never evicts a peer to reset its penalty.
class LoginBackoff {
public:
    std::chrono::seconds Failure(const std::string& peer) {
        std::lock_guard lock(mutex_);
        auto found = delays_.find(peer);
        if (found == delays_.end()) {
            if (delays_.size() >= 1024)
                return std::chrono::seconds(30);
            found = delays_.emplace(peer, 0).first;
        }
        auto& seconds = found->second;
        seconds = seconds == 0 ? 1 : std::min(seconds * 2, 30);
        return std::chrono::seconds(seconds);
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, int> delays_;
};

inline bool CanRenameTo(std::string_view relative, bool exists) {
    return !exists || InGames(relative);
}
} // namespace Jadyx::Ftp
