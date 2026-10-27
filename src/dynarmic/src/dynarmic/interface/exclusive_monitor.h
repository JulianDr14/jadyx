// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <boost/container/static_vector.hpp>

#include <dynarmic/common/spin_lock.h>

namespace Dynarmic {

using VAddr = std::uint64_t;
using Vector = std::array<std::uint64_t, 2>;

class ExclusiveMonitor {
public:
    /// @param processor_count Maximum number of processors using this global
    ///                        exclusive monitor. Each processor must have a
    ///                        unique id.
    explicit ExclusiveMonitor(size_t processor_count);

    size_t GetProcessorCount() const;

    /// Marks a region containing [address, address+size) to be exclusive to
    /// processor processor_id.
    template<typename T, typename Function>
    T ReadAndMark(size_t processor_id, VAddr address, Function op) {
        static_assert(std::is_trivially_copyable_v<T>);
        const VAddr masked_address = address & RESERVATION_GRANULE_MASK;

        Lock();
        reservations[processor_id].address = masked_address;
        const T value = op();
        std::memcpy(reservations[processor_id].value.data(), &value, sizeof(T));
        Unlock();
        return value;
    }

    /// Checks to see if processor processor_id has exclusive access to the
    /// specified region. If it does, executes the operation then clears
    /// the exclusive state for processors if their exclusive region(s)
    /// contain [address, address+size).
    template<typename T, typename Function>
        requires std::is_trivially_copyable_v<T>
    bool DoExclusiveOperation(size_t processor_id, VAddr address, Function op) {
        if (!CheckAndClear(processor_id, address)) {
            return false;
        }

        T saved_value;
        std::memcpy(&saved_value, reservations[processor_id].value.data(), sizeof(T));
        const bool result = op(saved_value);

        Unlock();
        return result;
    }

    struct CheckpointReservation {
        VAddr address{};
        Vector value{};
        bool active{};
    };
    struct Checkpoint {
        size_t processor_count{};
        std::array<CheckpointReservation, 4> reservations{};
    };

    // All guest CPUs and HLE exclusive operations must be parked by the caller.
    // No JIT code, locks, pointers or layout-dependent storage are saved.
    Checkpoint CaptureCheckpoint() {
        Checkpoint state{};
        Lock();
        state.processor_count = processor_count;
        for (size_t i = 0; i < state.processor_count; ++i) {
            if (reservations[i].address != INVALID_EXCLUSIVE_ADDRESS) {
                state.reservations[i] = {reservations[i].address, reservations[i].value, true};
            }
        }
        Unlock();
        return state;
    }

    void RestoreCheckpoint(const Checkpoint& state) {
        if (state.processor_count != processor_count) {
            throw std::invalid_argument("Pulse: exclusive monitor processor count");
        }
        for (size_t i = 0; i < state.reservations.size(); ++i) {
            const auto& r = state.reservations[i];
            if ((i >= state.processor_count || !r.active) &&
                (r.active || r.address || r.value != Vector{})) {
                throw std::invalid_argument("Pulse: noncanonical inactive reservation");
            }
            if (r.active && r.address == INVALID_EXCLUSIVE_ADDRESS) {
                throw std::invalid_argument("Pulse: invalid exclusive reservation address");
            }
        }
        Lock();
        for (size_t i = 0; i < state.processor_count; ++i) {
            const auto& r = state.reservations[i];
            reservations[i].address = r.active ? r.address : INVALID_EXCLUSIVE_ADDRESS;
            reservations[i].value = r.value;
        }
        Unlock();
    }

    /// Unmark everything.
    void Clear();
    /// Unmark processor id
    void ClearProcessor(size_t processor_id);

private:
    bool CheckAndClear(size_t processor_id, VAddr address);

    void Lock();
    void Unlock();

    friend volatile int* GetExclusiveMonitorLockPointer(ExclusiveMonitor*);
    friend size_t GetExclusiveMonitorProcessorCount(ExclusiveMonitor*);
    friend VAddr* GetExclusiveMonitorAddressPointer(ExclusiveMonitor*, size_t index);
    friend Vector* GetExclusiveMonitorValuePointer(ExclusiveMonitor*, size_t index);

    static constexpr VAddr RESERVATION_GRANULE_MASK = 0xFFFF'FFFF'FFFF'FFFFull;
    static constexpr VAddr INVALID_EXCLUSIVE_ADDRESS = 0xDEAD'DEAD'DEAD'DEADull;
    static constexpr size_t MAX_NUM_CPU_CORES = 4; // Sync with src/core/hardware_properties
    /// One cache line per processor: inline exclusive loads write their processor's reservation
    /// every time, and with all of them on one line each load evicted the other cores' copies.
    struct alignas(64) Reservation {
        VAddr address = INVALID_EXCLUSIVE_ADDRESS;
        Vector value{};
    };
    std::array<Reservation, MAX_NUM_CPU_CORES> reservations{};
    size_t processor_count;
    alignas(64) SpinLock lock;
};

}  // namespace Dynarmic
