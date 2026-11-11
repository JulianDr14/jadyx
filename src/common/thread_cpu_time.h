// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include "common/common_types.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace Common {
/// OS-accounted user+kernel CPU time of this host thread. Not wall time or guest-only time.
/// FILETIME's 100 ns units do not imply 100 ns measurement resolution. Unsupported/failure = null.
inline std::optional<u64> CurrentThreadCpuTimeNs() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return std::nullopt;
    const auto value = [](FILETIME t) {
        return (static_cast<u64>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return (value(kernel) + value(user)) * 100;
#else
    return std::nullopt;
#endif
}

/// A handle to the calling thread that other threads can pass to ThreadCycles. Null on failure.
inline void* DuplicateCurrentThreadHandle() {
#ifdef _WIN32
    HANDLE handle{};
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                         THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        return nullptr;
    }
    return handle;
#else
    return nullptr;
#endif
}

inline void CloseThreadHandle(void* handle) {
#ifdef _WIN32
    if (handle != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle));
    }
#endif
}

/// Processor cycles the thread has run, in the units of the time stamp counter: exact per
/// frame, unlike GetThreadTimes. Zero when unknown.
inline u64 ThreadCycles(void* handle) {
#ifdef _WIN32
    ULONG64 cycles{};
    if (handle != nullptr && QueryThreadCycleTime(static_cast<HANDLE>(handle), &cycles)) {
        return cycles;
    }
#endif
    return 0;
}
} // namespace Common
