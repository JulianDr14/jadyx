// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace D3D12 {

/// An index of objects owned by pipelines, not another residency cache. A compiler owns its
/// entry before starting work inline; equivalent requests wait on that entry, never on a task
/// queued behind themselves. The index retains neither results nor keys after consumers retire.
template <class Key, class Value, class Hash>
class LivePipelineCache {
public:
    enum class Outcome { Created, Hit, SharedInFlight, Bypassed };
    struct Entry {
        explicit Entry(Key key_) : key{std::move(key_)} {}
        ~Entry() {
            // The shared token may outlive the cache. Bypassed entries never own it.
            if (retirement) retirement->store(true, std::memory_order_release);
        }
        const Key key;
        std::optional<Value> value;

    private:
        friend class LivePipelineCache;
        std::shared_ptr<std::atomic_bool> retirement;
        std::mutex mutex;
        std::condition_variable ready_condition;
        bool ready{};
        std::exception_ptr failure;
    };
    struct Result {
        std::shared_ptr<const Entry> entry;
        Outcome outcome;
    };

    explicit LivePipelineCache(size_t limit = 8192) : max_entries{limit} {}

    template <class Build>
    Result Get(Key key, Build&& build) {
        const auto hash = Hash{}(key);
        std::shared_ptr<Entry> entry;
        bool owner = false;
        bool indexed = false;
        {
            std::scoped_lock lock{mutex};
            // Indexed weak slots can only expire after an indexed Entry retires.
            // Exchange before sweeping so a concurrent retirement requests another pass.
            if (++requests % 128 == 0 && retired->exchange(false, std::memory_order_acq_rel)) {
                SweepExpired();
            }
            const auto [first, last] = entries.equal_range(hash);
            for (auto it = first; it != last; ++it) {
                if (auto existing = it->second.lock(); existing && existing->key == key) {
                    entry = std::move(existing);
                    break;
                }
            }
            if (!entry) {
                // Full hits need no scan. A miss reclaims promptly after an owner retires.
                if (entries.size() >= max_entries &&
                    retired->exchange(false, std::memory_order_acq_rel)) {
                    SweepExpired();
                }
                entry = std::make_shared<Entry>(std::move(key));
                owner = true;
                indexed = entries.size() < max_entries;
                if (indexed) {
                    entries.emplace(hash, entry);
                    entry->retirement = retired;
                }
            }
        }
        if (!owner) {
            std::unique_lock lock{entry->mutex};
            const auto outcome = entry->ready ? Outcome::Hit : Outcome::SharedInFlight;
            entry->ready_condition.wait(lock, [&] { return entry->ready; });
            if (entry->failure) {
                std::rethrow_exception(entry->failure);
            }
            return {std::move(entry), outcome};
        }
        try {
            entry->value.emplace(build());
        } catch (...) {
            const auto failure = std::current_exception();
            // A failed build must be retriable even while existing waiters hold its entry.
            {
                std::scoped_lock lock{mutex};
                const auto [first, last] = entries.equal_range(hash);
                for (auto it = first; it != last; ++it) {
                    if (it->second.lock() == entry) {
                        entries.erase(it);
                        break;
                    }
                }
            }
            {
                std::scoped_lock lock{entry->mutex};
                entry->failure = failure;
                entry->ready = true;
            }
            entry->ready_condition.notify_all();
            std::rethrow_exception(failure);
        }
        {
            std::scoped_lock lock{entry->mutex};
            entry->ready = true;
        }
        entry->ready_condition.notify_all();
        return {std::move(entry), indexed ? Outcome::Created : Outcome::Bypassed};
    }

private:
    // Caller holds mutex; no live object or key is retained by the retirement token.
    void SweepExpired() {
        std::erase_if(entries, [](const auto& item) { return item.second.expired(); });
    }
    std::shared_ptr<std::atomic_bool> retired = std::make_shared<std::atomic_bool>(false);
    std::mutex mutex;
    std::unordered_multimap<size_t, std::weak_ptr<Entry>> entries;
    size_t requests{};
    const size_t max_entries;
};

} // namespace D3D12
