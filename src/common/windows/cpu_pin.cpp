// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <charconv>
#include <mutex>
#include <string_view>
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

struct MapPlace {
    int core{};
    int logical{-1}; ///< -1: every logical processor of the core.
};

struct MapEntry {
    std::string name;
    std::vector<MapPlace> places; ///< "core.logical+core.logical": the thread may run on any.
};

struct NamedThread {
    std::string name;
    std::string where; ///< The CPU sets it was placed on, or "any".
    DWORD id{};
    HANDLE handle{};
    std::uint64_t last_ns{};
};

std::atomic<CpuPin> g_pin{CpuPin::Off};
std::mutex g_placed_mutex;
std::string g_placed;
std::vector<MapEntry> g_map; ///< Under g_placed_mutex.
std::mutex g_named_mutex;
std::vector<NamedThread> g_named; ///< Under g_named_mutex; handles stay open for the app's life.
bool g_named_sampled{};          ///< Under g_named_mutex.

std::uint64_t ThreadCpuNs(HANDLE thread) {
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(thread, &creation, &exit, &kernel, &user)) {
        return 0;
    }
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) * 100;
}

/// Keeps a handle to the calling thread so NamedThreadCpuReport can read its CPU time.
/// A thread named again keeps its entry with the new name.
void RegisterNamedThread(const char* name, std::string where) {
    const DWORD id = GetCurrentThreadId();
    {
        std::scoped_lock lk{g_named_mutex};
        for (NamedThread& thread : g_named) {
            // An exited thread's id can come back on a new thread.
            if (thread.id == id && WaitForSingleObject(thread.handle, 0) == WAIT_TIMEOUT) {
                thread.name = name;
                thread.where = std::move(where);
                return;
            }
        }
    }
    HANDLE handle{};
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                         THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION | SYNCHRONIZE,
                         FALSE, 0)) {
        return;
    }
    std::scoped_lock lk{g_named_mutex};
    g_named.push_back({name, std::move(where), id, handle, ThreadCpuNs(handle)});
}

bool ParseIndex(std::string_view text, int& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value >= 0 &&
           value < 64;
}

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

/// The CPU sets the map gives a thread name: the longest matching name wins.
std::vector<ULONG> MapSets(const Plan& plan, const char* name) {
    const MapEntry* best{};
    const std::string_view thread{name};
    for (const MapEntry& entry : g_map) {
        if (thread.starts_with(entry.name) && (!best || entry.name.size() > best->name.size())) {
            best = &entry;
        }
    }
    if (!best) {
        return {};
    }
    // Places the app does not have are left out; none left leaves the thread floating.
    std::vector<ULONG> sets;
    const auto add = [&sets](ULONG id) {
        if (std::find(sets.begin(), sets.end(), id) == sets.end()) {
            sets.push_back(id);
        }
    };
    for (const MapPlace& place : best->places) {
        if (static_cast<std::size_t>(place.core) >= plan.cores.size()) {
            continue;
        }
        const Core& core = plan.cores[static_cast<std::size_t>(place.core)];
        if (place.logical < 0) {
            for (const ULONG id : core.sets) {
                add(id);
            }
        } else if (static_cast<std::size_t>(place.logical) < core.sets.size()) {
            add(core.sets[static_cast<std::size_t>(place.logical)]);
        }
    }
    return sets;
}

} // namespace

void SetCpuPin(CpuPin pin) {
    bool no_map{};
    if (pin == CpuPin::Map) {
        std::scoped_lock lk{g_placed_mutex};
        no_map = g_map.empty();
    }
    if (no_map) {
        // The default: no boot.cfg key gave a map.
        SetCpuPinMap(CpuPinDefault);
    }
    g_pin.store(pin, std::memory_order_relaxed);
}

bool SetCpuPinMap(std::string_view map) {
    std::vector<MapEntry> entries;
    while (!map.empty()) {
        const auto comma = map.find_first_of(",;");
        const std::string_view item = map.substr(0, comma);
        map = comma == std::string_view::npos ? std::string_view{} : map.substr(comma + 1);
        const auto equals = item.find('=');
        if (equals == 0 || equals == std::string_view::npos) {
            return false;
        }
        MapEntry entry{std::string(item.substr(0, equals)), {}};
        std::string_view places = item.substr(equals + 1);
        do {
            const auto plus = places.find('+');
            const std::string_view text = places.substr(0, plus);
            places = plus == std::string_view::npos ? std::string_view{} : places.substr(plus + 1);
            MapPlace place;
            const auto dot = text.find('.');
            if (!ParseIndex(text.substr(0, dot), place.core) ||
                (dot != std::string_view::npos &&
                 !ParseIndex(text.substr(dot + 1), place.logical))) {
                return false;
            }
            entry.places.push_back(place);
        } while (!places.empty());
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) {
        return false;
    }
    std::scoped_lock lk{g_placed_mutex};
    g_map = std::move(entries);
    return true;
}

namespace {

/// The CPU sets the thread called `name` goes on under the current CpuPin; empty, it floats.
std::vector<ULONG> SetsFor(const char* name) {
    const CpuPin pin = g_pin.load(std::memory_order_relaxed);
    if (pin == CpuPin::Off) {
        return {};
    }
    const Plan& plan = GetPlan();
    if (plan.cores.size() < 2) {
        return {};
    }
    // Split needs a second logical processor on every core for the other threads.
    const bool split = pin == CpuPin::Split && plan.siblings.size() == plan.cores.size();
    if (pin == CpuPin::Map) {
        std::scoped_lock lk{g_placed_mutex};
        return MapSets(plan, name);
    }
    if (const int guest = GuestCore(name); guest >= 0) {
        const Core& core = plan.cores[static_cast<std::size_t>(guest) % plan.cores.size()];
        return split ? std::vector<ULONG>{core.sets[0]} : core.sets;
    }
    return split ? plan.siblings : std::vector<ULONG>{};
}

/// Places the calling thread; returns the CPU sets it now runs on, or "any".
std::string PlaceCurrentThread(const char* name) {
    const std::string any = "any";
    const std::vector<ULONG> sets = SetsFor(name);
    if (sets.empty()) {
        return any;
    }
    const bool placed =
        SetThreadSelectedCpuSets(GetCurrentThread(), sets.data(), static_cast<ULONG>(sets.size())) != 0;
    std::scoped_lock lk{g_placed_mutex};
    g_placed += std::string(" ") + name + (placed ? "=" : "!=") + ListSets(sets);
    return placed ? ListSets(sets) : any;
}

} // namespace

void PinCurrentThread(const char* name) {
    if (name == nullptr) {
        return;
    }
    RegisterNamedThread(name, PlaceCurrentThread(name));
}

std::size_t RepinNamedThreads() {
    std::size_t moved = 0;
    std::string placed;
    {
        std::scoped_lock lk{g_named_mutex};
        for (NamedThread& thread : g_named) {
            if (WaitForSingleObject(thread.handle, 0) != WAIT_TIMEOUT) {
                continue; // Exited.
            }
            const std::vector<ULONG> sets = SetsFor(thread.name.c_str());
            const std::string where = sets.empty() ? "any" : ListSets(sets);
            if (where == thread.where) {
                continue;
            }
            // No sets clears the thread's own choice: it floats again.
            if (SetThreadSelectedCpuSets(thread.handle, sets.empty() ? nullptr : sets.data(),
                                         static_cast<ULONG>(sets.size())) == 0) {
                placed += " " + thread.name + "!=" + where;
                continue;
            }
            thread.where = where;
            placed += " " + thread.name + "=" + where;
            ++moved;
        }
    }
    std::scoped_lock lk{g_placed_mutex};
    g_placed += " | moved:" + (placed.empty() ? std::string(" none") : placed);
    return moved;
}

std::string NamedThreadCpuReport(std::size_t limit) {
    struct Use {
        std::uint64_t ns;
        const NamedThread* thread;
    };
    std::scoped_lock lk{g_named_mutex};
    std::vector<Use> uses;
    for (NamedThread& thread : g_named) {
        const std::uint64_t now = ThreadCpuNs(thread.handle);
        uses.push_back({now >= thread.last_ns ? now - thread.last_ns : 0, &thread});
        thread.last_ns = now;
    }
    const bool first = !g_named_sampled;
    g_named_sampled = true;
    if (first) {
        return {};
    }
    std::sort(uses.begin(), uses.end(), [](const Use& a, const Use& b) { return a.ns > b.ns; });
    std::string report;
    for (std::size_t i = 0; i < uses.size() && i < limit; ++i) {
        report += (report.empty() ? "" : ", ") + uses[i].thread->name + " " +
                  std::to_string(uses[i].ns / 1'000'000) + " ms@" + uses[i].thread->where;
    }
    return report;
}

std::string CpuPinReport() {
    const CpuPin pin = g_pin.load(std::memory_order_relaxed);
    if (pin == CpuPin::Off) {
        return "off";
    }
    const Plan& plan = GetPlan();
    std::string report = pin == CpuPin::Split ? "split;" : pin == CpuPin::Map ? "map;" : "cores;";
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
