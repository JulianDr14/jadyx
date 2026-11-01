// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the app writes when it dies, and what it logs before a fault can kill it.

#include <string>

namespace Jadyx {

/// Once per process, from the UI thread.
void InstallCrashHandlers();
/// Per thread in the MSVC runtime: std::set_terminate on each thread that should name its exception.
[[noreturn]] void OnTerminate();
/// `frames` as RVAs into jadyx.exe or module+offset elsewhere, after `prefix`, as one line.
std::string FormatFrames(const char* prefix, void* const* frames, unsigned short count);

} // namespace Jadyx
