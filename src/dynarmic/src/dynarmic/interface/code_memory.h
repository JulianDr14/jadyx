// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

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
    std::size_t page_out = 0;   ///< Faults whose page was out of the working set when checked.
    std::size_t page_in = 0;    ///< Faults whose page was back in the working set when checked.
    /// From a fault's first try to the end of its emission, which completed.
    std::uint64_t recover_us_max = 0;
    std::uint64_t recover_us_total = 0;
    /// The first fault's exception record, as the system raised it.
    std::uint32_t first_code = 0;
    std::uint32_t first_params = 0;
    std::uint64_t first_info0 = 0;
    std::uint64_t first_info1 = 0;
};
[[nodiscard]] CodeEmitFaults GetCodeEmitFaults() noexcept;

/// Whether chunks of code memory mapped from now on are locked in the working set through their
/// write view (Windows, dual-mapped chunks). The system then cannot trim a page code is being
/// written to, which it did on the console short of memory: the write faulted. Default on.
void SetLockedCode(bool enabled) noexcept;
/// Whether locked chunks mapped from now on are also locked through their executable view. The
/// working set holds virtual pages, so the write view's lock leaves the executable view's pages
/// free to be mapped on first use; on the console such a first run of code in a page made another
/// core's store into that page fault while the system mapped it. Same physical pages: costs
/// working set minimum, no memory. Default on; only with SetLockedCode.
void SetLockedExecView(bool enabled) noexcept;
struct CodeLockState {
    std::size_t locked_bytes = 0;       ///< Code memory locked now (write views).
    std::size_t exec_locked_bytes = 0;  ///< Of it, also locked through the executable view.
    std::size_t failures = 0;           ///< Chunks that could not be locked.
    std::size_t exec_failures = 0;      ///< Locked chunks whose executable view could not be.
    unsigned long last_error = 0;       ///< GetLastError of the last failure.
    bool available = false;             ///< VirtualLock and the working set calls were found.
    std::size_t chunks = 0;             ///< Chunks locked so far, and the time it took them.
    std::uint64_t lock_us = 0;
    std::uint64_t lock_us_max = 0;
};
[[nodiscard]] CodeLockState GetCodeLockState() noexcept;

} // namespace Dynarmic
