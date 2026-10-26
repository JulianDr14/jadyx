// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

namespace Dynarmic {

/// Bytes committed by every x64 code cache in the process, all JIT instances together. On Windows
/// code memory is committed as code is emitted, so this is what the JIT costs the app's commit;
/// elsewhere the whole cache is committed up front and this stays 0.
std::size_t CommittedCodeBytes() noexcept;

/// Caps every x64 code cache, created or not, at `bytes` of code or what it had committed when
/// it saw the cap, whichever is more: the JIT stops committing memory, and a cache is cleared
/// only when it fills the pages it has. For when the app runs short of memory; 0 lifts the cap.
void SetCodeCacheCap(std::size_t bytes) noexcept;
[[nodiscard]] std::size_t CodeCacheCap() noexcept;

} // namespace Dynarmic
