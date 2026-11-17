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
/// With `release`, the cap is `bytes` alone: a cache holding more is cleared at its next compile
/// and gives the memory past `bytes` back, where its memory can be decommitted (Windows).
void SetCodeCacheCap(std::size_t bytes, bool release = false) noexcept;
[[nodiscard]] std::size_t CodeCacheCap() noexcept;
[[nodiscard]] bool CodeCacheCapReleases() noexcept;

/// Whether code caches created from now on may map their memory twice, executable where code runs
/// and writable where it is emitted, so W^X costs no page protection change per compiled block.
/// Only used where the runtime passes a one-time probe; otherwise caches use paged W^X. Default on.
void SetDualMappedCode(bool allowed) noexcept;
/// Code caches alive that are dual-mapped; 0 when the probe failed or SetDualMappedCode(false).
[[nodiscard]] std::size_t DualMappedCodeCaches() noexcept;

/// One line per x64 code cache alive: where its next byte is emitted and that memory's state in
/// both views. For a crash report: allocates nothing and waits on no lock. Returns bytes written.
std::size_t DescribeCodeCaches(char* out, std::size_t size) noexcept;

/// Faults taken while a thread was emitting into a code cache (Windows). Such a write can fault
/// on memory that is mapped and writable a moment later, when the system is short of memory: the
/// faulting instruction is retried after a wait instead of ending the process.
struct CodeEmitFaults {
    std::size_t retries = 0;    ///< Faults retried.
    std::size_t recovered = 0;  ///< Emissions that faulted and then completed.
    std::size_t fatal = 0;      ///< Emissions that kept faulting and were let crash.
};
[[nodiscard]] CodeEmitFaults GetCodeEmitFaults() noexcept;

} // namespace Dynarmic
