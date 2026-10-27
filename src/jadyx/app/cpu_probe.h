// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string>

namespace Jadyx {

/// Measures the app's logical processors before the guest starts ("cpu_probe=1"), about two
/// seconds: how much time the system takes from each one while all of them spin, and how fast a
/// MONITORX/MWAITX wait wakes on a store from the SMT sibling and from another core. Each result
/// is one line through write.
void RunCpuProbe(const std::function<void(const std::string&)>& write);

} // namespace Jadyx
