// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace Core::HID {
// Track guest starts separately from legacy sensor delivery, which defaults enabled.
// Each style has three device indices. Repeated Start/Stop calls are idempotent;
// stopping one half of a dual Joy-Con must not stop the other half's request.
class SixAxisRequests {
public:
    void Set(std::uint8_t style, std::uint8_t device, bool requested) {
        if (device >= 3)
            return;
        const auto bit = static_cast<std::uint8_t>(1U << device);
        if (requested)
            requests[style].fetch_or(bit, std::memory_order_relaxed);
        else
            requests[style].fetch_and(static_cast<std::uint8_t>(~bit), std::memory_order_relaxed);
    }
    bool Any(std::uint8_t style) const {
        return requests[style].load(std::memory_order_relaxed) != 0;
    }
    void SetSyntheticEnabled(bool enabled) {
        synthetic_enabled.store(enabled, std::memory_order_relaxed);
    }
    // Keep legacy delivery until the frontend explicitly opts into synthetic motion.
    bool DeliveryEnabled(bool legacy_enabled, std::uint8_t style, int device = -1) const {
        if (!synthetic_enabled.load(std::memory_order_relaxed))
            return legacy_enabled;
        const auto active = requests[style].load(std::memory_order_relaxed);
        return device < 0 ? active != 0 : (active & (1U << device)) != 0;
    }
    void Clear() {
        for (auto& request : requests)
            request.store(0, std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<std::uint8_t>, 256> requests{};
    std::atomic<bool> synthetic_enabled{};
};
} // namespace Core::HID
