// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <windows.h>

#include "common/windows/cpu_pin.h"

namespace Common::Windows {

namespace {

struct Core {
    BYTE index{};
    /// CPU set ids in logical processor order: the first is the core's primary thread.
    std::vector<ULONG> sets;
};

struct Plan {
    std::vector<Core> cores;
    /// The second logical processor of every core that has one.
    std::vector<ULONG> siblings;
};

std::atomic<CpuPin> g_pin{CpuPin::Off};
std::mutex g_placed_mutex;
std::string g_placed;

/// The physical cores of the logical processors allocated to this process (all of them when the
/// system allocated none in particular), ordered by core.
Plan BuildPlan() {
    Plan plan;
    ULONG length{};
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    std::vector<std::byte> buffer(length);
    if (length == 0 ||
        !GetSystemCpuSetInformation(reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data()),
                                    length, &length, GetCurrentProcess(), 0)) {
        return plan;
    }
    struct Set {
        ULONG id;
        BYTE core;
        BYTE logical;
        bool ours;
    };
    std::vector<Set> sets;
    for (ULONG offset = 0; offset < length;) {
        const auto& info =
            *reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        offset += info.Size;
        if (info.Type != CpuSetInformation) {
            continue;
        }
        sets.push_back({info.CpuSet.Id, info.CpuSet.CoreIndex,
                        info.CpuSet.LogicalProcessorIndex,
                        info.CpuSet.AllocatedToTargetProcess != 0});
    }
    if (std::any_of(sets.begin(), sets.end(), [](const Set& set) { return set.ours; })) {
        std::erase_if(sets, [](const Set& set) { return !set.ours; });
    }
    std::sort(sets.begin(), sets.end(), [](const Set& a, const Set& b) {
        return a.core != b.core ? a.core < b.core : a.logical < b.logical;
    });
    for (const Set& set : sets) {
        if (plan.cores.empty() || plan.cores.back().index != set.core) {
            plan.cores.push_back({set.core, {}});
        }
        plan.cores.back().sets.push_back(set.id);
    }
    for (const Core& core : plan.cores) {
        if (core.sets.size() > 1) {
            plan.siblings.push_back(core.sets[1]);
        }
    }
    return plan;
}

const Plan& GetPlan() {
    static const Plan plan = BuildPlan();
    return plan;
}

/// The guest core a thread name stands for, or -1.
int GuestCore(const char* name) {
    constexpr const char prefix[] = "CPUCore_";
    if (std::strncmp(name, prefix, sizeof(prefix) - 1) == 0) {
        return std::atoi(name + sizeof(prefix) - 1);
    }
    return std::strcmp(name, "CPUThread") == 0 ? 0 : -1;
}

std::string ListSets(const std::vector<ULONG>& sets) {
    std::string list;
    for (const ULONG id : sets) {
        list += (list.empty() ? "" : "+") + std::to_string(id);
    }
    return list;
}

} // namespace

void SetCpuPin(CpuPin pin) {
    g_pin.store(pin, std::memory_order_relaxed);
}

void PinCurrentThread(const char* name) {
    const CpuPin pin = g_pin.load(std::memory_order_relaxed);
    if (pin == CpuPin::Off || name == nullptr) {
        return;
    }
    const Plan& plan = GetPlan();
    if (plan.cores.size() < 2) {
        return;
    }
    // Split needs a second logical processor on every core for the other threads.
    const bool split = pin == CpuPin::Split && plan.siblings.size() == plan.cores.size();
    std::vector<ULONG> sets;
    if (const int guest = GuestCore(name); guest >= 0) {
        const Core& core = plan.cores[static_cast<std::size_t>(guest) % plan.cores.size()];
        if (split) {
            sets.push_back(core.sets[0]);
        } else {
            sets = core.sets;
        }
    } else if (split) {
        sets = plan.siblings;
    } else {
        return;
    }
    const bool placed =
        SetThreadSelectedCpuSets(GetCurrentThread(), sets.data(), static_cast<ULONG>(sets.size())) != 0;
    std::scoped_lock lk{g_placed_mutex};
    g_placed += std::string(" ") + name + (placed ? "=" : "!=") + ListSets(sets);
}

std::string CpuPinReport() {
    const CpuPin pin = g_pin.load(std::memory_order_relaxed);
    if (pin == CpuPin::Off) {
        return "off";
    }
    const Plan& plan = GetPlan();
    std::string report = pin == CpuPin::Split ? "split;" : "cores;";
    for (const Core& core : plan.cores) {
        report += " c" + std::to_string(core.index) + "=" + ListSets(core.sets);
    }
    std::scoped_lock lk{g_placed_mutex};
    return report + "; placed:" + (g_placed.empty() ? std::string(" none") : g_placed);
}

std::vector<std::vector<unsigned long>> AppCoreCpuSets() {
    std::vector<std::vector<unsigned long>> cores;
    for (const Core& core : GetPlan().cores) {
        cores.emplace_back(core.sets.begin(), core.sets.end());
    }
    return cores;
}

} // namespace Common::Windows
