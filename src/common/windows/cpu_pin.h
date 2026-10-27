// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace Common::Windows {

/// Where named threads are placed among the logical processors allocated to the app.
enum class CpuPin {
    /// The scheduler places every thread.
    Off,
    /// Each guest core thread runs on both logical processors of a physical core of its own, so
    /// no two guest cores share one; other threads float.
    Cores,
    /// Each guest core thread runs on the first logical processor of a core of its own; the GPU,
    /// recorder and other named threads run on the second ones.
    Split,
};

/// Read by each thread when it is named (Common::SetCurrentThreadName); set it before the
/// emulator's threads start.
void SetCpuPin(CpuPin pin);

/// Places the calling thread by its name under the current CpuPin. Called by
/// SetCurrentThreadName.
void PinCurrentThread(const char* name);

/// The physical cores and logical processors the plan uses, and the threads placed so far.
[[nodiscard]] std::string CpuPinReport();

/// The CPU set ids of the app's logical processors, one list per physical core, primary first.
[[nodiscard]] std::vector<std::vector<unsigned long>> AppCoreCpuSets();

} // namespace Common::Windows
