// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/interface/exclusive_monitor.h"

#include <algorithm>

#include "common/assert.h"

namespace Dynarmic {

ExclusiveMonitor::ExclusiveMonitor(std::size_t processor_count_)
        : processor_count{processor_count_} {
    ASSERT(processor_count <= MAX_NUM_CPU_CORES);
}

size_t ExclusiveMonitor::GetProcessorCount() const {
    return processor_count;
}

void ExclusiveMonitor::Lock() {
    lock.Lock();
}

void ExclusiveMonitor::Unlock() {
    lock.Unlock();
}

bool ExclusiveMonitor::CheckAndClear(std::size_t processor_id, VAddr address) {
    const VAddr masked_address = address & RESERVATION_GRANULE_MASK;

    Lock();
    if (reservations[processor_id].address != masked_address) {
        Unlock();
        return false;
    }

    for (size_t i = 0; i < processor_count; ++i) {
        if (reservations[i].address == masked_address) {
            reservations[i].address = INVALID_EXCLUSIVE_ADDRESS;
        }
    }
    return true;
}

void ExclusiveMonitor::Clear() {
    Lock();
    for (size_t i = 0; i < processor_count; ++i) {
        reservations[i].address = INVALID_EXCLUSIVE_ADDRESS;
    }
    Unlock();
}

void ExclusiveMonitor::ClearProcessor(std::size_t processor_id) {
    Lock();
    reservations[processor_id].address = INVALID_EXCLUSIVE_ADDRESS;
    Unlock();
}

}  // namespace Dynarmic
