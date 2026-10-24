// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include "jadyx/app/warmup_control.h"

namespace Core {
class System;
}
namespace Jadyx {

/// A fresh launch only, before System::Run. The GPU thread must remain alive.
/// Workers publish counters; only this screen's thread paints and polls input.
class WarmupScreen {
public:
    WarmupScreen(Core::System& system, WarmupControl& control);
    ~WarmupScreen();
    WarmupScreen(const WarmupScreen&) = delete;
    WarmupScreen& operator=(const WarmupScreen&) = delete;
    void Stage(WarmupPhase phase);
    void Progress(std::size_t done, std::size_t total);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Jadyx
