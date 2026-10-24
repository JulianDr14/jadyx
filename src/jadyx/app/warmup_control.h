// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <stop_token>
#include <string_view>

namespace Jadyx {

inline bool WarmupControlEnabled(const char* value) {
    return value && std::string_view{value} == "1";
}

enum class WarmupPhase { Shaders, Cpu };
struct WarmupView {
    WarmupPhase phase{WarmupPhase::Shaders};
    std::size_t done{}, total{};
    unsigned seconds{};
    bool stopping{}, cancelled{};
};

/// One launch only. Input belongs to the screen thread; workers only read Token().
/// A skip retains compiled blocks; a cancel also prevents System::Run.
class WarmupControl {
public:
    void Skip() {
        stop.request_stop();
    }
    void Cancel() {
        cancelled.store(true, std::memory_order_release);
        stop.request_stop();
    }
    bool Cancelled() const {
        return cancelled.load(std::memory_order_acquire);
    }
    std::stop_token Token() const {
        return stop.get_token();
    }
    void Disconnect() {
        sampled = false;
    }
    void Poll(bool skip, bool cancel) {
        if (sampled) {
            if (cancel && !previous_cancel)
                Cancel();
            else if (skip && !previous_skip)
                Skip();
        }
        previous_skip = skip;
        previous_cancel = cancel;
        sampled = true;
    }

private:
    std::stop_source stop;
    std::atomic_bool cancelled{};
    bool sampled{}, previous_skip{}, previous_cancel{};
};

} // namespace Jadyx
