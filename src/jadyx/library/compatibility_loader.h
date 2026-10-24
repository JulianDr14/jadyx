// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include "jadyx/library/compatibility.h"

namespace Jadyx::Compatibility {
/// Owned by one library session. Start only after its first draw. No UI-thread file or HTTP I/O.
class Loader {
public:
    Loader();
    ~Loader();
    void Start();
    std::optional<Table> Take();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Jadyx::Compatibility
