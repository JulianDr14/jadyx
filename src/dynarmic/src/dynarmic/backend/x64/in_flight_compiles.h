// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace Dynarmic::Backend::X64 {

/// Blocks being compiled right now, so that cores missing the same block at once translate it
/// once: the first claims it, the others wait for it and then find it in the cache.
///
/// A claim holder must not wait for anything a waiting thread could be holding up: waiters keep
/// whatever they hold (a shared code cache's quiescence gate above all) while they wait.
class InFlightCompiles {
public:
    /// The right to compile one block, released when this goes.
    class [[nodiscard]] Claim {
    public:
        /// Owned without a registry: nothing else can compile the block (a private cache).
        [[nodiscard]] static Claim Exclusive() noexcept {
            return Claim{nullptr, 0, true};
        }

        Claim(Claim&& other) noexcept
            : registry{std::exchange(other.registry, nullptr)}, key{other.key}, owned{std::exchange(other.owned, false)} {}
        Claim& operator=(Claim&&) = delete;
        Claim(const Claim&) = delete;
        Claim& operator=(const Claim&) = delete;

        ~Claim() {
            if (registry) {
                registry->Release(key);
            }
        }

        /// False: another thread compiled the block meanwhile; look it up again.
        [[nodiscard]] bool Owned() const noexcept {
            return owned;
        }

    private:
        friend class InFlightCompiles;
        Claim(InFlightCompiles* registry, std::uint64_t key, bool owned) noexcept
            : registry{registry}, key{key}, owned{owned} {}

        InFlightCompiles* registry;
        std::uint64_t key;
        bool owned;
    };

    InFlightCompiles() = default;
    InFlightCompiles(const InFlightCompiles&) = delete;
    InFlightCompiles& operator=(const InFlightCompiles&) = delete;

    /// Claims `key`, or, when another thread holds it, waits for that claim to go and returns an
    /// unowned one.
    Claim Acquire(std::uint64_t key) {
        std::unique_lock lock{mutex};
        if (Contains(key)) {
            ++waits;
            released.wait(lock, [&] { return !Contains(key); });
            return Claim{nullptr, key, false};
        }
        keys.push_back(key);
        return Claim{this, key, true};
    }

    /// Claims held now.
    [[nodiscard]] std::size_t Size() const {
        std::scoped_lock lock{mutex};
        return keys.size();
    }

    /// Acquires that waited for another thread's claim.
    [[nodiscard]] std::uint64_t Waits() const {
        std::scoped_lock lock{mutex};
        return waits;
    }

private:
    void Release(std::uint64_t key) {
        {
            std::scoped_lock lock{mutex};
            const auto it = std::find(keys.begin(), keys.end(), key);
            *it = keys.back();
            keys.pop_back();
        }
        released.notify_all();
    }

    [[nodiscard]] bool Contains(std::uint64_t key) const {
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    }

    mutable std::mutex mutex;
    std::condition_variable released;
    std::vector<std::uint64_t> keys;  ///< At most one per compiling thread: a scan beats a hash.
    std::uint64_t waits = 0;
};

}  // namespace Dynarmic::Backend::X64
