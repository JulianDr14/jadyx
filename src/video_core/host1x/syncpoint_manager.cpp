// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/scope_exit.h"
#include "video_core/host1x/syncpoint_manager.h"

namespace Tegra {

namespace Host1x {

namespace {
// Nonzero while this thread runs syncpoint actions: an action that raises a syncpoint runs the
// actions it satisfies itself, since it already holds the turn the others would wait for.
thread_local u32 action_depth = 0;
} // namespace

SyncpointManager::ActionHandle SyncpointManager::RegisterAction(
    std::atomic<u32>& syncpoint, std::list<RegisteredAction>& action_storage, u32 expected_value,
    std::function<void()>&& action) {
    if (syncpoint.load(std::memory_order_acquire) >= expected_value) {
        action();
        return {};
    }

    std::unique_lock lk(guard);
    if (syncpoint.load(std::memory_order_relaxed) >= expected_value) {
        lk.unlock();
        action();
        return {};
    }
    auto it = action_storage.begin();
    while (it != action_storage.end()) {
        if (it->expected_value >= expected_value) {
            break;
        }
        ++it;
    }
    return action_storage.emplace(it, expected_value, std::move(action));
}

void SyncpointManager::DeregisterAction(std::list<RegisteredAction>& action_storage,
                                        const ActionHandle& handle) {
    std::unique_lock lk(guard);

    // We want to ensure the iterator still exists prior to erasing it
    // Otherwise, if an invalid iterator was passed in then it could lead to UB
    // It is important to avoid UB in that case since the deregister isn't called from a locked
    // context
    for (auto it = action_storage.begin(); it != action_storage.end(); it++) {
        if (it == handle) {
            action_storage.erase(it);
            return;
        }
    }
    // Gone from the storage: it ran, or a batch took it and may be running it now. The caller
    // frees what the action uses next, so wait out every batch taken until here. An action
    // deregistering another one holds the turn itself and must not wait for it.
    if (action_depth == 0) {
        const u64 taken = next_ticket;
        turn_cv.wait(lk, [&] { return serving_ticket >= taken; });
    }
}

void SyncpointManager::DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(guest_action_storage[syncpoint_id], handle);
}

void SyncpointManager::DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(host_action_storage[syncpoint_id], handle);
}

void SyncpointManager::IncrementGuest(u32 syncpoint_id) {
    Increment(syncpoints_guest[syncpoint_id], wait_guest_cv, guest_action_storage[syncpoint_id]);
}

void SyncpointManager::IncrementHost(u32 syncpoint_id) {
    Increment(syncpoints_host[syncpoint_id], wait_host_cv, host_action_storage[syncpoint_id]);
}

void SyncpointManager::WaitGuest(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_guest[syncpoint_id], wait_guest_cv, expected_value);
}

void SyncpointManager::WaitHost(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_host[syncpoint_id], wait_host_cv, expected_value);
}

void SyncpointManager::CancelWaits() {
    std::scoped_lock lk(guard);
    stopping.store(true, std::memory_order_release);
    wait_guest_cv.notify_all();
    wait_host_cv.notify_all();
}

void SyncpointManager::PulseRaise(u32 syncpoint_id, u32 guest_value, u32 host_value) {
    std::list<RegisteredAction> ready;
    const auto raise = [&](std::atomic<u32>& syncpoint, std::list<RegisteredAction>& action_storage,
                           u32 value) {
        if (syncpoint.load(std::memory_order_acquire) >= value) return;
        syncpoint.store(value, std::memory_order_release);
        TakeReady(action_storage, value, ready);
    };
    std::unique_lock lk(guard);
    raise(syncpoints_guest.at(syncpoint_id), guest_action_storage[syncpoint_id], guest_value);
    raise(syncpoints_host.at(syncpoint_id), host_action_storage[syncpoint_id], host_value);
    wait_guest_cv.notify_all();
    wait_host_cv.notify_all();
    RunReady(lk, ready);
}

void SyncpointManager::Increment(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                                 std::list<RegisteredAction>& action_storage) {
    auto new_value{syncpoint.fetch_add(1, std::memory_order_acq_rel) + 1};

    std::list<RegisteredAction> ready;
    std::unique_lock lk(guard);
    TakeReady(action_storage, new_value, ready);
    wait_cv.notify_all();
    RunReady(lk, ready);
}

void SyncpointManager::TakeReady(std::list<RegisteredAction>& action_storage, u32 value,
                                 std::list<RegisteredAction>& ready) {
    auto last = action_storage.begin();
    while (last != action_storage.end() && last->expected_value <= value) {
        ++last;
    }
    // Splicing keeps the nodes, so the handle of a taken action stays valid while it runs.
    ready.splice(ready.end(), action_storage, action_storage.begin(), last);
}

void SyncpointManager::RunReady(std::unique_lock<std::mutex>& lk,
                                std::list<RegisteredAction>& ready) {
    if (ready.empty()) {
        return;
    }
    const auto run = [&] {
        lk.unlock();
        ++action_depth;
        SCOPE_EXIT {
            --action_depth;
            lk.lock();
        };
        for (auto& entry : ready) {
            entry.action();
        }
    };
    if (action_depth != 0) {
        run();
        return;
    }
    // Batches run in the order their values were reached, so a composite and the signals after
    // it keep the order the GPU gave them.
    const u64 ticket = next_ticket++;
    turn_cv.wait(lk, [&] { return serving_ticket == ticket; });
    SCOPE_EXIT {
        ++serving_ticket;
        turn_cv.notify_all();
    };
    run();
}

void SyncpointManager::Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                            u32 expected_value) {
    const auto pred = [&]() {
        return syncpoint.load(std::memory_order_acquire) >= expected_value ||
               stopping.load(std::memory_order_acquire);
    };
    if (pred()) {
        return;
    }

    std::unique_lock lk(guard);
    wait_cv.wait(lk, pred);
}

} // namespace Host1x

} // namespace Tegra
