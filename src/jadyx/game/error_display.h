// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace Jadyx {
// Adapted from NXbox 17fd8c6a9. Producers queue on emulator threads; the play loop
// owns dismissal/completion, as it does for the UWP software keyboard.
class ErrorDisplay {
public:
    struct Message {
        std::uint64_t id{};
        std::string code;
        std::string message;
    };

    void Show(std::string code, std::string message, std::function<void()> finished) {
        std::scoped_lock lock{mutex};
        if (stopped)
            return;
        Request request{};
        request.message.id = ++next_id;
        request.message.code = std::move(code);
        request.message.message = std::move(message);
        request.finished = std::move(finished);
        requests.push_back(std::move(request));
    }

    [[nodiscard]] std::optional<Message> Current() const {
        std::scoped_lock lock{mutex};
        if (requests.empty() || requests.front().dismissed)
            return std::nullopt;
        return requests.front().message;
    }

    void Dismiss(std::uint64_t id) {
        std::scoped_lock lock{mutex};
        if (!requests.empty() && requests.front().message.id == id)
            requests.front().dismissed = true;
    }

    void Close() {
        std::scoped_lock lock{mutex};
        for (auto& request : requests)
            request.dismissed = true;
    }

    void CompleteDismissed() {
        for (;;) {
            std::function<void()> finished;
            {
                std::scoped_lock lock{mutex};
                if (requests.empty() || !requests.front().dismissed)
                    return;
                finished = std::move(requests.front().finished);
                requests.pop_front();
            }
            // Remove first; guest code can reenter the frontend.
            if (finished)
                finished();
        }
    }

    // Before core teardown, discard callbacks and reject late producers.
    void Stop() {
        std::scoped_lock lock{mutex};
        stopped = true;
        requests.clear();
    }

private:
    struct Request {
        Message message;
        std::function<void()> finished;
        bool dismissed{};
    };
    mutable std::mutex mutex;
    std::deque<Request> requests;
    std::uint64_t next_id{};
    bool stopped{};
};
} // namespace Jadyx
