// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/interface/code_memory.h"

#include <atomic>

#include <cstdlib>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
// On the Xbox/UWP AppContainer the plain Virtual* APIs are unavailable for JIT memory; the
// sandbox-legal *FromApp variants must be used instead, and emitting executable pages additionally
// requires the `codeGeneration` restricted capability in the package manifest. The function shapes
// are identical, so we just select the symbol. RWX is never grantable under the AppContainer, so
// DYNARMIC_UWP_APPCONTAINER always implies DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT (W^X) — see CMakeLists.
#    if defined(DYNARMIC_UWP_APPCONTAINER)
#        define DYNARMIC_VIRTUAL_ALLOC   VirtualAllocFromApp
#        define DYNARMIC_VIRTUAL_PROTECT VirtualProtectFromApp
// A failed protect or commit here does not fail at the call: it surfaces later as a fault inside
// JIT code, where the process dies without reaching any unhandled-exception filter. Raise at the
// cause instead, with a code the UWP frontend's first-chance logger decodes:
//   Information[0] = GetLastError(), [1] = address, [2] = size, [3] = requested protection
#        define DYNARMIC_UWP_PROTECT_FAILED 0xE0DA0001u
#        define DYNARMIC_UWP_COMMIT_FAILED  0xE0DA0002u
// A dual-mapped chunk held mapped whose write view is gone; [3] is the view's State | Type.
#        define DYNARMIC_UWP_WRITE_VIEW_LOST 0xE0DA0003u
static void RaiseJitMemoryFailure(DWORD code, const void* base, size_t size, DWORD protection) {
    const ULONG_PTR info[4] = {GetLastError(), reinterpret_cast<ULONG_PTR>(base), size, protection};
    RaiseException(code, EXCEPTION_NONCONTINUABLE, 4, info);
}
#    else
#        define DYNARMIC_VIRTUAL_ALLOC   VirtualAlloc
#        define DYNARMIC_VIRTUAL_PROTECT VirtualProtect
#    endif
#else
#    include <sys/mman.h>
#endif

#ifdef __APPLE__
#    include <errno.h>
#    include <fmt/format.h>
#    include <sys/sysctl.h>
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/assert.h"
#include "common/dynamic_library.h"
#include "common/vm_activity.h"
#include "dynarmic/mcl/bit.hpp"
#include "dynarmic/backend/x64/xbyak.h"

#include "dynarmic/backend/x64/a32_jitstate.h"
#include "dynarmic/backend/x64/abi.h"
#include "dynarmic/backend/x64/hostloc.h"
#include "dynarmic/backend/x64/perf_map.h"
#include "dynarmic/backend/x64/stack_layout.h"
#include "dynarmic/backend/x64/writable_code_ranges.h"
#include "dynarmic/interface/jit_profile.h"

namespace Dynarmic {
namespace {
/// Committed bytes of every BlockOfCode alive (CommittedCodeBytes).
std::atomic<size_t> s_committed_code_bytes{0};
/// SetCodeCacheCap; SIZE_MAX when uncapped. Each call bumps the epoch for the caches to see.
std::atomic<size_t> s_code_cache_cap{SIZE_MAX};
std::atomic<size_t> s_code_cache_cap_epoch{0};
/// SetCodeCacheCap's release: caches past the cap are cleared and give the memory past it back.
std::atomic_bool s_code_cache_release{false};
/// What a capped cache may still emit past the pages it had committed: one block's worth
/// (MINIMUM_REMAINING_CODESIZE) and its commit ahead, so it is not cleared on the spot, plus
/// room for an area's new code. With 2 MiB a cache holding a large prewarm filled up within the
/// next load and was cleared, recompiling everything it had warmed.
constexpr size_t CAP_SLACK = 16 * 1024 * 1024;
/// SetDualMappedCode, and how many code caches live dual-mapped (DualMappedCodeCaches).
std::atomic_bool s_dual_map_allowed{true};
std::atomic<size_t> s_dual_mapped_caches{0};
/// SetLockedCode, and what locking the chunks' write views did (GetCodeLockState).
std::atomic_bool s_lock_code{true};
std::atomic<size_t> s_locked_code_bytes{0};
std::atomic_bool s_lock_exec_view{true};
std::atomic<size_t> s_locked_exec_bytes{0};
std::atomic<size_t> s_exec_lock_failures{0};
std::atomic<size_t> s_locked_chunks{0};
std::atomic<std::uint64_t> s_chunk_lock_us{0};
std::atomic<std::uint64_t> s_chunk_lock_us_max{0};
std::atomic<size_t> s_code_lock_failures{0};
std::atomic<unsigned long> s_code_lock_last_error{0};
std::atomic_bool s_code_lock_available{false};
/// Every BlockOfCode alive, for DescribeCodeCaches; a crash report reads it without a lock.
constexpr size_t MAX_LIVE_CODE_CACHES = 16;
std::array<std::atomic<const Backend::X64::BlockOfCode*>, MAX_LIVE_CODE_CACHES> s_live_code_caches{};
} // anonymous namespace

size_t CommittedCodeBytes() noexcept {
    return s_committed_code_bytes.load(std::memory_order_relaxed);
}

void SetCodeCacheCap(size_t bytes, bool release) noexcept {
    s_code_cache_cap.store(bytes != 0 ? bytes : SIZE_MAX, std::memory_order_relaxed);
    s_code_cache_release.store(bytes != 0 && release, std::memory_order_relaxed);
    s_code_cache_cap_epoch.fetch_add(1, std::memory_order_release);
}

size_t CodeCacheCap() noexcept {
    const size_t cap = s_code_cache_cap.load(std::memory_order_relaxed);
    return cap != SIZE_MAX ? cap : 0;
}

bool CodeCacheCapReleases() noexcept {
    return s_code_cache_release.load(std::memory_order_relaxed);
}

void SetLockedCode(bool enabled) noexcept {
    s_lock_code.store(enabled, std::memory_order_relaxed);
}

void SetLockedExecView(bool enabled) noexcept {
    s_lock_exec_view.store(enabled, std::memory_order_relaxed);
}

CodeLockState GetCodeLockState() noexcept {
    CodeLockState state;
    state.locked_bytes = s_locked_code_bytes.load(std::memory_order_relaxed);
    state.exec_locked_bytes = s_locked_exec_bytes.load(std::memory_order_relaxed);
    state.failures = s_code_lock_failures.load(std::memory_order_relaxed);
    state.exec_failures = s_exec_lock_failures.load(std::memory_order_relaxed);
    state.last_error = s_code_lock_last_error.load(std::memory_order_relaxed);
    state.available = s_code_lock_available.load(std::memory_order_relaxed);
    state.chunks = s_locked_chunks.load(std::memory_order_relaxed);
    state.lock_us = s_chunk_lock_us.load(std::memory_order_relaxed);
    state.lock_us_max = s_chunk_lock_us_max.load(std::memory_order_relaxed);
    return state;
}

void SetDualMappedCode(bool allowed) noexcept {
    s_dual_map_allowed.store(allowed, std::memory_order_relaxed);
}

size_t DualMappedCodeCaches() noexcept {
    return s_dual_mapped_caches.load(std::memory_order_relaxed);
}

size_t DescribeCodeCaches(char* out, size_t size) noexcept {
    size_t written = 0;
    for (const auto& slot : s_live_code_caches) {
        const auto* const code = slot.load(std::memory_order_acquire);
        if (code == nullptr || written + 1 >= size) {
            continue;
        }
        const int n = code->DescribeState(out + written, size - written);
        if (n > 0) {
            written = std::min(size - 1, written + static_cast<size_t>(n));
        }
    }
    if (size != 0) {
        out[written] = '\0';
    }
    return written;
}

namespace {
std::atomic<size_t> s_emit_fault_retries{0};
std::atomic<size_t> s_emit_faults_recovered{0};
std::atomic<size_t> s_emit_faults_fatal{0};
std::atomic<size_t> s_emit_fault_page_out{0};
std::atomic<size_t> s_emit_fault_page_in{0};
static_assert(::Common::VmActivity::KindCount == 4, "CodeEmitFaults::busy has one slot per kind");
std::array<std::atomic<size_t>, ::Common::VmActivity::KindCount> s_emit_fault_busy{};
std::array<std::atomic<size_t>, ::Common::VmActivity::KindCount> s_emit_fault_recent{};
std::atomic<size_t> s_emit_fault_quiet{0};
std::atomic<std::uint64_t> s_emit_recover_us_max{0};
std::atomic<std::uint64_t> s_emit_recover_us_total{0};
/// The first fault's exception record: taken once, published by s_emit_first_ready.
std::atomic<bool> s_emit_first_taken{false};
std::atomic<bool> s_emit_first_ready{false};
std::uint32_t s_emit_first_code = 0;
std::uint32_t s_emit_first_params = 0;
std::uint64_t s_emit_first_info0 = 0;
std::uint64_t s_emit_first_info1 = 0;
/// What finished this close before a fault counts as having run beside it.
constexpr std::int64_t RECENT_VM_US = 5000;
/// The cache this thread is emitting into (BlockOfCode::EmitScope), and the faults it retried in
/// the current emission.
thread_local const Backend::X64::BlockOfCode* t_emitting = nullptr;
thread_local unsigned t_emit_fault_retries = 0;
thread_local std::int64_t t_emit_fault_start_us = 0;
/// About 2.5 s of waits in all: past that the fault is not the transient kind.
constexpr unsigned MAX_EMIT_FAULT_RETRIES = 64;

#ifdef _WIN32
// PSAPI_WORKING_SET_EX_INFORMATION, declared here: psapi.h hides it from app partitions.
struct WorkingSetExInfo {
    PVOID address;
    ULONG_PTR attributes; ///< Bit 0: Valid (resident in the working set); bit 22: Locked.
};
using QueryWorkingSetExFn = BOOL(WINAPI*)(HANDLE, PVOID, DWORD);
QueryWorkingSetExFn s_query_working_set_ex = nullptr;

// Whether the page `p` is in the working set: 1 yes, 0 no, -1 unknown.
int PageResident(const void* p) {
    if (s_query_working_set_ex == nullptr) {
        return -1;
    }
    WorkingSetExInfo info{const_cast<void*>(p), 0};
    if (!s_query_working_set_ex(GetCurrentProcess(), &info, sizeof(info))) {
        return -1;
    }
    return (info.attributes & 1) != 0 ? 1 : 0;
}

// A write into code memory, while emitting, faulted on the console on a page that was mapped,
// committed and writable when the crash report looked at it right after, with free memory at the
// app limit. Retried after a short wait instead of ending the process; a fault that persists goes
// on to the other handlers (and the crash report) as before.
LONG NTAPI EmitFaultHandler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if ((code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR) || t_emitting == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (t_emit_fault_retries >= MAX_EMIT_FAULT_RETRIES) {
        if (t_emit_fault_retries++ == MAX_EMIT_FAULT_RETRIES) {
            s_emit_faults_fatal.fetch_add(1, std::memory_order_relaxed);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const unsigned attempt = t_emit_fault_retries++;
    s_emit_fault_retries.fetch_add(1, std::memory_order_relaxed);
    if (attempt == 0) {
        t_emit_fault_start_us = ::Common::VmActivity::NowUs();
        if (!s_emit_first_taken.exchange(true, std::memory_order_relaxed)) {
            const EXCEPTION_RECORD& record = *ep->ExceptionRecord;
            s_emit_first_code = record.ExceptionCode;
            s_emit_first_params = record.NumberParameters;
            s_emit_first_info0 = record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0;
            s_emit_first_info1 = record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0;
            s_emit_first_ready.store(true, std::memory_order_release);
        }
        // Which address-space operations ran beside the fault (diagnostic).
        bool any = false;
        for (size_t kind = 0; kind < ::Common::VmActivity::KindCount; ++kind) {
            const auto& state = ::Common::VmActivity::g_kinds[kind];
            if (state.in_flight.load(std::memory_order_relaxed) != 0) {
                s_emit_fault_busy[kind].fetch_add(1, std::memory_order_relaxed);
                any = true;
            } else if (const std::int64_t end = state.last_end_us.load(std::memory_order_relaxed);
                       end != 0 && t_emit_fault_start_us - end <= RECENT_VM_US) {
                s_emit_fault_recent[kind].fetch_add(1, std::memory_order_relaxed);
                any = true;
            }
        }
        if (!any) {
            s_emit_fault_quiet.fetch_add(1, std::memory_order_relaxed);
        }
        // The console reports no faulting address; the write goes where the next byte is
        // emitted. Out of the working set there says the system trimmed a page being written.
        const int resident = PageResident(t_emitting->EmitWritePointer());
        if (resident == 0) {
            s_emit_fault_page_out.fetch_add(1, std::memory_order_relaxed);
        } else if (resident == 1) {
            s_emit_fault_page_in.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (attempt < 4) {
        SwitchToThread();
    } else {
        Sleep(std::min(attempt, 50u));
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void InstallEmitFaultHandler() {
    static const bool installed = [] {
        // Resolved by name, never imported: importing the errorhandling api-set makes the app
        // fail to activate on the console.
        using AddVehFn = PVOID(WINAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER);
        static ::Common::DynamicLibrary kernelbase("Kernelbase");
        AddVehFn add_veh{};
        if (kernelbase.IsOpen() && !kernelbase.GetSymbol("QueryWorkingSetEx", &s_query_working_set_ex)) {
            (void)kernelbase.GetSymbol("K32QueryWorkingSetEx", &s_query_working_set_ex);
        }
        // Last in the chain: a fault the app resolves itself (demand-committed backing, pages
        // swapped out) must reach its handler first, not wait here.
        return kernelbase.IsOpen() && kernelbase.GetSymbol("AddVectoredExceptionHandler", &add_veh) &&
               add_veh(/*first=*/0, EmitFaultHandler) != nullptr;
    }();
    (void)installed;
}
#endif
} // anonymous namespace

CodeEmitFaults GetCodeEmitFaults() noexcept {
    CodeEmitFaults faults{s_emit_fault_retries.load(std::memory_order_relaxed),
                          s_emit_faults_recovered.load(std::memory_order_relaxed),
                          s_emit_faults_fatal.load(std::memory_order_relaxed),
                          s_emit_fault_page_out.load(std::memory_order_relaxed),
                          s_emit_fault_page_in.load(std::memory_order_relaxed)};
#ifdef _WIN32
    for (size_t kind = 0; kind < faults.busy.size(); ++kind) {
        faults.busy[kind] = s_emit_fault_busy[kind].load(std::memory_order_relaxed);
        faults.recent[kind] = s_emit_fault_recent[kind].load(std::memory_order_relaxed);
    }
    faults.quiet = s_emit_fault_quiet.load(std::memory_order_relaxed);
    faults.recover_us_max = s_emit_recover_us_max.load(std::memory_order_relaxed);
    faults.recover_us_total = s_emit_recover_us_total.load(std::memory_order_relaxed);
    if (s_emit_first_ready.load(std::memory_order_acquire)) {
        faults.first_code = s_emit_first_code;
        faults.first_params = s_emit_first_params;
        faults.first_info0 = s_emit_first_info0;
        faults.first_info1 = s_emit_first_info1;
    }
#endif
    return faults;
}
} // namespace Dynarmic

namespace Dynarmic::Backend::X64 {

const Xbyak::Reg64 BlockOfCode::ABI_JIT_PTR = HostLocToReg64(Dynarmic::Backend::X64::ABI_JIT_PTR);
#ifdef _WIN32
const Xbyak::Reg64 BlockOfCode::ABI_RETURN = HostLocToReg64(Dynarmic::Backend::X64::ABI_RETURN);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM1 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM1);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM2 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM2);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM3 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM3);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM4 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM4);
const std::array<Xbyak::Reg64, ABI_PARAM_COUNT> BlockOfCode::ABI_PARAMS = {BlockOfCode::ABI_PARAM1, BlockOfCode::ABI_PARAM2, BlockOfCode::ABI_PARAM3, BlockOfCode::ABI_PARAM4};
#else
const Xbyak::Reg64 BlockOfCode::ABI_RETURN = HostLocToReg64(Dynarmic::Backend::X64::ABI_RETURN);
const Xbyak::Reg64 BlockOfCode::ABI_RETURN2 = HostLocToReg64(Dynarmic::Backend::X64::ABI_RETURN2);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM1 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM1);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM2 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM2);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM3 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM3);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM4 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM4);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM5 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM5);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM6 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM6);
const std::array<Xbyak::Reg64, ABI_PARAM_COUNT> BlockOfCode::ABI_PARAMS = {BlockOfCode::ABI_PARAM1, BlockOfCode::ABI_PARAM2, BlockOfCode::ABI_PARAM3, BlockOfCode::ABI_PARAM4, BlockOfCode::ABI_PARAM5, BlockOfCode::ABI_PARAM6};
#endif

namespace {

constexpr size_t CONSTANT_POOL_SIZE = 2 * 1024 * 1024;
constexpr size_t PRELUDE_COMMIT_SIZE = 16 * 1024 * 1024;

#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
// Dual-mapped code memory: pagefile-backed sections mapped twice, read-execute where code runs
// and read-write where it is written (Xbyak's writeOffset_). W^X then needs no page protection
// change per block. Making a page executable again is not cheap on the console: each RW→RX flip
// cost ~21 µs, a third of a block's compile time, and shoots down the TLB of every core running
// guest code. Where the section or the executable view is refused, the paged W^X below is used.
#    if defined(DYNARMIC_UWP_APPCONTAINER)
#        define DYNARMIC_CREATE_CODE_SECTION(size, commit) \
            CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | (commit), (size), nullptr)
#        define DYNARMIC_MAP_CODE_VIEW(section, access, size) MapViewOfFileFromApp((section), (access), 0, (size))
#        define DYNARMIC_VIRTUAL_ALLOC2    "VirtualAlloc2FromApp"
#        define DYNARMIC_MAP_VIEW_OF_FILE3 "MapViewOfFile3FromApp"
#    else
#        define DYNARMIC_CREATE_CODE_SECTION(size, commit)                                                 \
            CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | (commit),             \
                               static_cast<DWORD>(u64(size) >> 32), static_cast<DWORD>(size), nullptr)
#        define DYNARMIC_MAP_CODE_VIEW(section, access, size) MapViewOfFile((section), (access), 0, 0, (size))
#        define DYNARMIC_VIRTUAL_ALLOC2    "VirtualAlloc2"
#        define DYNARMIC_MAP_VIEW_OF_FILE3 "MapViewOfFile3"
#    endif
#    ifndef MEM_RESERVE_PLACEHOLDER
#        define MEM_RESERVE_PLACEHOLDER 0x00040000
#    endif
#    ifndef MEM_REPLACE_PLACEHOLDER
#        define MEM_REPLACE_PLACEHOLDER 0x00004000
#    endif
#    ifndef MEM_PRESERVE_PLACEHOLDER
#        define MEM_PRESERVE_PLACEHOLDER 0x00000002
#    endif

// A cache is mapped a chunk at a time where the runtime allows it, each chunk a section of its
// own in placeholders of the two reservations: the chunks past a cap can then be unmapped and
// their commit returned, which a single section's pages never can be (VirtualFree refuses them).
constexpr size_t CODE_CHUNK_SIZE = 4 * 1024 * 1024;

struct DualCodeView {
    u8* exec;
    u8* write;
    HANDLE section;           ///< The whole cache's section; nullptr when it is mapped in chunks.
    size_t size;
    std::vector<bool> chunks; ///< Chunked: which CODE_CHUNK_SIZE chunks are mapped.
};
std::mutex s_dual_views_mutex;
std::vector<DualCodeView> s_dual_views;

// The placeholder APIs, resolved by name like host_memory's, so the target's link set needs none.
struct PlaceholderApi {
    using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
                                            MEM_EXTENDED_PARAMETER*, ULONG);
    using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

    VirtualAlloc2Fn virtual_alloc2{};
    MapViewOfFile3Fn map_view_of_file3{};
    UnmapViewOfFile2Fn unmap_view_of_file2{};

    explicit operator bool() const {
        return virtual_alloc2 != nullptr && map_view_of_file3 != nullptr && unmap_view_of_file2 != nullptr;
    }
};

const PlaceholderApi& Placeholders() {
    static const PlaceholderApi api = [] {
        static ::Common::DynamicLibrary kernelbase("Kernelbase");
        PlaceholderApi result;
        if (kernelbase.IsOpen()) {
            (void)kernelbase.GetSymbol(DYNARMIC_VIRTUAL_ALLOC2, &result.virtual_alloc2);
            (void)kernelbase.GetSymbol(DYNARMIC_MAP_VIEW_OF_FILE3, &result.map_view_of_file3);
            (void)kernelbase.GetSymbol("UnmapViewOfFile2", &result.unmap_view_of_file2);
        }
        return result;
    }();
    return api;
}

// Locking a chunk's write view keeps its pages in the working set, so writing code there never
// faults: on the console, short of memory, the system trimmed a page a block was being written
// to and the write faulted though the page was mapped and writable. A process locks no more than
// its minimum working set allows, so that is raised by each chunk first. Resolved by name like
// the placeholder APIs.
struct CodeLockApi {
    using VirtualLockFn = BOOL(WINAPI*)(LPVOID, SIZE_T);
    using GetWorkingSetFn = BOOL(WINAPI*)(HANDLE, PSIZE_T, PSIZE_T, PDWORD);
    using SetWorkingSetFn = BOOL(WINAPI*)(HANDLE, SIZE_T, SIZE_T, DWORD);

    VirtualLockFn lock{};
    VirtualLockFn unlock{};
    GetWorkingSetFn get_working_set{};
    SetWorkingSetFn set_working_set{};

    explicit operator bool() const {
        return lock != nullptr && unlock != nullptr && get_working_set != nullptr && set_working_set != nullptr;
    }
};

const CodeLockApi& CodeLocks() {
    static const CodeLockApi api = [] {
        static ::Common::DynamicLibrary kernelbase("Kernelbase");
        CodeLockApi result;
        if (kernelbase.IsOpen()) {
            (void)kernelbase.GetSymbol("VirtualLock", &result.lock);
            (void)kernelbase.GetSymbol("VirtualUnlock", &result.unlock);
            (void)kernelbase.GetSymbol("GetProcessWorkingSetSizeEx", &result.get_working_set);
            (void)kernelbase.GetSymbol("SetProcessWorkingSetSizeEx", &result.set_working_set);
        }
        s_code_lock_available.store(static_cast<bool>(result), std::memory_order_relaxed);
        return result;
    }();
    return api;
}

// Moves the minimum working set (and the maximum, when it would fall below) by `delta` bytes.
bool AdjustMinimumWorkingSet(const CodeLockApi& api, std::ptrdiff_t delta) {
    SIZE_T minimum = 0, maximum = 0;
    DWORD flags = 0;
    if (!api.get_working_set(GetCurrentProcess(), &minimum, &maximum, &flags)) {
        return false;
    }
    const SIZE_T new_minimum = delta >= 0 ? minimum + static_cast<SIZE_T>(delta)
                                          : minimum - std::min<SIZE_T>(minimum, static_cast<SIZE_T>(-delta));
    const SIZE_T new_maximum = std::max<SIZE_T>(maximum, new_minimum + CODE_CHUNK_SIZE);
    return api.set_working_set(GetCurrentProcess(), new_minimum, new_maximum, flags) != FALSE;
}

// Locks a newly mapped chunk through its write view and (SetLockedExecView) its executable view,
// with one working set move for both. Called with s_dual_views_mutex held (or by the one-time
// boot probe, before any cache exists), so the moves of the minimum never race. Once per 4 MiB chunk: the executable view's lock maps pages the write
// view's lock already brought in, so it costs page table entries, not memory.
void LockCodeChunk(u8* write, u8* exec) {
    if (!s_lock_code.load(std::memory_order_relaxed)) {
        return;
    }
    const CodeLockApi& api = CodeLocks();
    if (!api) {
        s_code_lock_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const bool both = s_lock_exec_view.load(std::memory_order_relaxed);
    const auto bytes = static_cast<std::ptrdiff_t>((both ? 2 : 1) * CODE_CHUNK_SIZE);
    if (!AdjustMinimumWorkingSet(api, bytes)) {
        s_code_lock_last_error.store(GetLastError(), std::memory_order_relaxed);
        s_code_lock_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!api.lock(write, CODE_CHUNK_SIZE)) {
        const DWORD error = GetLastError();
        (void)AdjustMinimumWorkingSet(api, -bytes);
        s_code_lock_last_error.store(error, std::memory_order_relaxed);
        s_code_lock_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s_locked_code_bytes.fetch_add(CODE_CHUNK_SIZE, std::memory_order_relaxed);
    if (both) {
        if (api.lock(exec, CODE_CHUNK_SIZE)) {
            s_locked_exec_bytes.fetch_add(CODE_CHUNK_SIZE, std::memory_order_relaxed);
        } else {
            s_code_lock_last_error.store(GetLastError(), std::memory_order_relaxed);
            s_exec_lock_failures.fetch_add(1, std::memory_order_relaxed);
            (void)AdjustMinimumWorkingSet(api, -static_cast<std::ptrdiff_t>(CODE_CHUNK_SIZE));
        }
    }
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    s_locked_chunks.fetch_add(1, std::memory_order_relaxed);
    s_chunk_lock_us.fetch_add(us, std::memory_order_relaxed);
    if (us > s_chunk_lock_us_max.load(std::memory_order_relaxed)) {
        s_chunk_lock_us_max.store(us, std::memory_order_relaxed);  // Serialized like the lock.
    }
}

// Before the chunk is unmapped: unlocks whichever of its views were locked, then gives the working
// set minimum back in one move. A chunk that was not locked is left alone.
void UnlockCodeChunk(u8* write, u8* exec) {
    const CodeLockApi& api = CodeLocks();
    if (!api) {
        return;
    }
    std::ptrdiff_t bytes = 0;
    if (api.unlock(exec, CODE_CHUNK_SIZE)) {
        s_locked_exec_bytes.fetch_sub(CODE_CHUNK_SIZE, std::memory_order_relaxed);
        bytes += CODE_CHUNK_SIZE;
    }
    if (api.unlock(write, CODE_CHUNK_SIZE)) {
        s_locked_code_bytes.fetch_sub(CODE_CHUNK_SIZE, std::memory_order_relaxed);
        bytes += CODE_CHUNK_SIZE;
    }
    if (bytes != 0) {
        (void)AdjustMinimumWorkingSet(api, -bytes);
    }
}

// Commits [exec, exec + size) of a single section through its write view. The pages are the
// section's, so the executable view sees them committed too; a runtime that commits per view
// gets them committed there read-only first, then made executable once per commit.
bool CommitSectionPages(u8* exec, u8* write, size_t size) {
    if (DYNARMIC_VIRTUAL_ALLOC(write, size, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(exec, &info, sizeof(info)) == sizeof(info) && info.State == MEM_COMMIT &&
        info.Protect == PAGE_EXECUTE_READ && info.RegionSize >= size) {
        return true;
    }
    DWORD old_protection = 0;
    return DYNARMIC_VIRTUAL_ALLOC(exec, size, MEM_COMMIT, PAGE_READONLY) != nullptr &&
           DYNARMIC_VIRTUAL_PROTECT(exec, size, PAGE_EXECUTE_READ, &old_protection);
}

bool MapDualView(size_t size, DualCodeView& view) {
    view.section = DYNARMIC_CREATE_CODE_SECTION(size, SEC_RESERVE);
    if (view.section == nullptr) {
        return false;
    }
    view.size = size;
    view.exec = static_cast<u8*>(DYNARMIC_MAP_CODE_VIEW(view.section, FILE_MAP_READ | FILE_MAP_EXECUTE, size));
    view.write = static_cast<u8*>(DYNARMIC_MAP_CODE_VIEW(view.section, FILE_MAP_WRITE, size));
    if (view.exec != nullptr && view.write != nullptr) {
        return true;
    }
    if (view.exec != nullptr) {
        UnmapViewOfFile(view.exec);
    }
    if (view.write != nullptr) {
        UnmapViewOfFile(view.write);
    }
    CloseHandle(view.section);
    return false;
}

// Releases the placeholders of a reservation split into chunks up to split_end, and the one
// placeholder past it.
void ReleaseChunkPlaceholders(u8* base, size_t size, size_t split_end) {
    size_t offset = 0;
    for (; offset < split_end; offset += CODE_CHUNK_SIZE) {
        VirtualFree(base + offset, 0, MEM_RELEASE);
    }
    if (offset < size) {
        VirtualFree(base + offset, 0, MEM_RELEASE);
    }
}

// Reserves both views as one placeholder per chunk, nothing mapped yet.
bool ReserveCodeChunks(size_t size, DualCodeView& view) {
    const PlaceholderApi& api = Placeholders();
    if (!api) {
        return false;
    }
    size = (size + CODE_CHUNK_SIZE - 1) & ~(CODE_CHUNK_SIZE - 1);
    const auto reserve = [&]() -> u8* {
        u8* const base = static_cast<u8*>(api.virtual_alloc2(GetCurrentProcess(), nullptr, size,
                                                             MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                                             PAGE_NOACCESS, nullptr, 0));
        if (base == nullptr) {
            return nullptr;
        }
        for (size_t offset = 0; offset + CODE_CHUNK_SIZE < size; offset += CODE_CHUNK_SIZE) {
            if (!VirtualFree(base + offset, CODE_CHUNK_SIZE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                ReleaseChunkPlaceholders(base, size, offset);
                return nullptr;
            }
        }
        return base;
    };
    view.exec = reserve();
    view.write = view.exec != nullptr ? reserve() : nullptr;
    if (view.write == nullptr) {
        if (view.exec != nullptr) {
            ReleaseChunkPlaceholders(view.exec, size, size);
        }
        return false;
    }
    view.section = nullptr;
    view.size = size;
    view.chunks.assign(size / CODE_CHUNK_SIZE, false);
    return true;
}

// Maps chunk `index` in both views: a committed section of its own, executable and writable.
bool MapCodeChunk(DualCodeView& view, size_t index) {
    const PlaceholderApi& api = Placeholders();
    const HANDLE process = GetCurrentProcess();
    const size_t offset = index * CODE_CHUNK_SIZE;
    const HANDLE section = DYNARMIC_CREATE_CODE_SECTION(CODE_CHUNK_SIZE, SEC_COMMIT);
    if (section == nullptr) {
        return false;
    }
    void* const exec = api.map_view_of_file3(section, process, view.exec + offset, 0, CODE_CHUNK_SIZE,
                                             MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READ, nullptr, 0);
    void* const write = exec == nullptr ? nullptr
                                        : api.map_view_of_file3(section, process, view.write + offset, 0,
                                                                CODE_CHUNK_SIZE, MEM_REPLACE_PLACEHOLDER,
                                                                PAGE_READWRITE, nullptr, 0);
    if (exec != nullptr && write == nullptr) {
        api.unmap_view_of_file2(process, exec, MEM_PRESERVE_PLACEHOLDER);
    }
    // The views hold the section; it goes with the last of them.
    CloseHandle(section);
    if (write == nullptr) {
        return false;
    }
    view.chunks[index] = true;
    LockCodeChunk(static_cast<u8*>(write), static_cast<u8*>(exec));
    return true;
}

void UnmapCodeChunk(DualCodeView& view, size_t index) {
    const PlaceholderApi& api = Placeholders();
    const size_t offset = index * CODE_CHUNK_SIZE;
    UnlockCodeChunk(view.write + offset, view.exec + offset);
    api.unmap_view_of_file2(GetCurrentProcess(), view.exec + offset, MEM_PRESERVE_PLACEHOLDER);
    api.unmap_view_of_file2(GetCurrentProcess(), view.write + offset, MEM_PRESERVE_PLACEHOLDER);
    view.chunks[index] = false;
}

void UnmapDualView(DualCodeView& view) {
    if (view.section != nullptr) {
        UnmapViewOfFile(view.exec);
        UnmapViewOfFile(view.write);
        CloseHandle(view.section);
        return;
    }
    for (size_t index = 0; index < view.chunks.size(); ++index) {
        if (view.chunks[index]) {
            UnmapCodeChunk(view, index);
        }
    }
    ReleaseChunkPlaceholders(view.exec, view.size, view.size);
    ReleaseChunkPlaceholders(view.write, view.size, view.size);
}

int CallCodeProbe(const void* code) {
    __try {
        return reinterpret_cast<int (*)()>(const_cast<void*>(code))();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

constexpr std::array<u8, 6> CODE_PROBE{0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3}; // mov eax, 42; ret

// Once per process: maps a small section twice, writes CODE_PROBE through one view and runs it
// through the other, so a runtime that maps but will not execute such views falls back at boot
// instead of faulting in the first block.
bool DualMappingWorks() {
    static const bool works = [] {
        constexpr size_t probe_size = 64 * 1024;
        DualCodeView view{};
        if (!MapDualView(probe_size, view)) {
            return false;
        }
        bool ok = false;
        if (CommitSectionPages(view.exec, view.write, 4096)) {
            std::memcpy(view.write, CODE_PROBE.data(), CODE_PROBE.size());
            ok = CallCodeProbe(view.exec) == 42;
        }
        UnmapDualView(view);
        return ok;
    }();
    return works;
}

// The same for chunks, on the second of two and twice: a split placeholder has to take a view
// again once one was unmapped from it, or no chunk could ever be released.
bool CodeChunksWork() {
    static const bool works = [] {
        DualCodeView view{};
        if (!ReserveCodeChunks(2 * CODE_CHUNK_SIZE, view)) {
            return false;
        }
        bool ok = true;
        for (int pass = 0; pass < 2 && ok; ++pass) {
            ok = MapCodeChunk(view, 1);
            if (ok) {
                std::memcpy(view.write + CODE_CHUNK_SIZE, CODE_PROBE.data(), CODE_PROBE.size());
                ok = CallCodeProbe(view.exec + CODE_CHUNK_SIZE) == 42;
                UnmapCodeChunk(view, 1);
            }
        }
        UnmapDualView(view);
        return ok;
    }();
    return works;
}

u8* AllocDualMapped(size_t size) {
    if (!s_dual_map_allowed.load(std::memory_order_relaxed)) {
        return nullptr;
    }
    DualCodeView view{};
    if (!(CodeChunksWork() && ReserveCodeChunks(size, view)) &&
        !(DualMappingWorks() && MapDualView(size, view))) {
        return nullptr;
    }
    std::scoped_lock lock{s_dual_views_mutex};
    s_dual_views.push_back(std::move(view));
    s_dual_mapped_caches.fetch_add(1, std::memory_order_relaxed);
    return s_dual_views.back().exec;
}

DualCodeView* FindDualView(const u8* exec) {
    const auto it = std::find_if(s_dual_views.begin(), s_dual_views.end(),
                                 [exec](const DualCodeView& view) { return view.exec == exec; });
    return it != s_dual_views.end() ? &*it : nullptr;
}

bool FreeDualMapped(u8* exec) {
    std::scoped_lock lock{s_dual_views_mutex};
    DualCodeView* const view = FindDualView(exec);
    if (view == nullptr) {
        return false;
    }
    UnmapDualView(*view);
    s_dual_views.erase(s_dual_views.begin() + (view - s_dual_views.data()));
    s_dual_mapped_caches.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

u8* DualWriteView(const u8* exec) {
    std::scoped_lock lock{s_dual_views_mutex};
    const DualCodeView* const view = FindDualView(exec);
    return view != nullptr ? view->write : nullptr;
}

// Commits [offset, offset + size) of the cache at exec in both views. Nothing emitted is touched,
// so the code pages stay as they are.
bool CommitDualMapped(const u8* exec, size_t offset, size_t size) {
    std::scoped_lock lock{s_dual_views_mutex};
    DualCodeView* const view = FindDualView(exec);
    if (view == nullptr) {
        return false;
    }
    if (view->section != nullptr) {
        return CommitSectionPages(view->exec + offset, view->write + offset, size);
    }
    for (size_t index = offset / CODE_CHUNK_SIZE; index * CODE_CHUNK_SIZE < offset + size; ++index) {
        if (!view->chunks[index] && !MapCodeChunk(*view, index)) {
            return false;
        }
    }
    return true;
}

// Unmaps the chunks from `offset` (chunk-aligned) on. False for a cache that is one section.
bool ReleaseDualMapped(const u8* exec, size_t offset) {
    std::scoped_lock lock{s_dual_views_mutex};
    DualCodeView* const view = FindDualView(exec);
    if (view == nullptr || view->section != nullptr) {
        return false;
    }
    for (size_t index = offset / CODE_CHUNK_SIZE; index < view->chunks.size(); ++index) {
        if (view->chunks[index]) {
            UnmapCodeChunk(*view, index);
        }
    }
    return true;
}

// Checks that [offset, offset + size) of the cache at exec is mapped in both views, mapping the
// chunks the bookkeeping has not. A chunk held mapped whose write view is not there raises
// DYNARMIC_UWP_WRITE_VIEW_LOST: emitting into it faulted with nothing saying why. False when a
// chunk could not be mapped.
bool VerifyDualMapped(const u8* exec, size_t offset, size_t size) {
    const u8* lost = nullptr;
    MEMORY_BASIC_INFORMATION lost_info{};
    {
        std::scoped_lock lock{s_dual_views_mutex};
        DualCodeView* const view = FindDualView(exec);
        if (view == nullptr || view->section != nullptr) {
            return true;
        }
        for (size_t index = offset / CODE_CHUNK_SIZE;
             index * CODE_CHUNK_SIZE < offset + size && index < view->chunks.size(); ++index) {
            if (!view->chunks[index]) {
                if (!MapCodeChunk(*view, index)) {
                    return false;
                }
                continue;
            }
            const u8* const write = view->write + index * CODE_CHUNK_SIZE;
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(write, &info, sizeof(info)) != sizeof(info) ||
                info.State != MEM_COMMIT || info.Type != MEM_MAPPED) {
                lost = write;
                lost_info = info;
                break;
            }
        }
    }
    if (lost != nullptr) {
#    if defined(DYNARMIC_UWP_APPCONTAINER)
        const ULONG_PTR info[4] = {GetLastError(), reinterpret_cast<ULONG_PTR>(lost),
                                   lost_info.RegionSize, lost_info.State | lost_info.Type};
        RaiseException(DYNARMIC_UWP_WRITE_VIEW_LOST, EXCEPTION_NONCONTINUABLE, 4, info);
#    endif
        return false;
    }
    return true;
}
#endif

#ifdef _WIN32
// Reserve-only allocator for the code region: EnsureMemoryCommitted() commits pages on demand, so
// the process is only charged for code actually emitted. Eden upstream switched to Xbyak's default
// allocator; the Xbox UWP AppContainer needs this one, both for the *FromApp APIs and to stay within
// the Game-mode commit limit.
class CustomXbyakAllocator : public Xbyak::Allocator {
public:
    uint8_t* alloc(size_t size) override {
#    ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
        if (u8* const exec = AllocDualMapped(size)) {
            return exec;
        }
#    endif
        void* p = DYNARMIC_VIRTUAL_ALLOC(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
        if (p == nullptr) {
            using Xbyak::Error;
            XBYAK_THROW(Xbyak::ERR_CANT_ALLOC);
        }
        return static_cast<uint8_t*>(p);
    }

    void free(uint8_t* p) override {
#    ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
        if (FreeDualMapped(p)) {
            return;
        }
#    endif
        VirtualFree(static_cast<void*>(p), 0, MEM_RELEASE);
    }

    bool useProtect() const override { return false; }
};

// This is threadsafe as Xbyak::Allocator does not contain any state; it is a pure interface.
CustomXbyakAllocator s_allocator;
#endif

#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
constexpr size_t WX_PAGE_SIZE = 4096;

const u8* WxPageDown(const u8* ptr) {
    return reinterpret_cast<const u8*>(reinterpret_cast<uintptr_t>(ptr) & ~(WX_PAGE_SIZE - 1));
}

const u8* WxPageUp(const u8* ptr) {
    return WxPageDown(ptr + WX_PAGE_SIZE - 1);
}
#endif

#ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
void ProtectMemory(const void* base, size_t size, bool is_executable,
                   [[maybe_unused]] bool preserve_cfg = false) {
    // The constructor enables writing before anything is committed (committed_size == 0); there is
    // nothing to protect yet, and a zero-sized protect is an error on the UWP path.
    if (size == 0) {
        return;
    }
#    ifdef _WIN32
    const JitProfile::Timer timer{JitProfile::Phase::Protect};
    DWORD oldProtect = 0;
    // The is_executable→PAGE_EXECUTE_READ transition is the call that requires the `codeGeneration`
    // capability under the AppContainer (VirtualProtectFromApp); the W^X invariant means we only ever
    // hold RW or RX, never RWX.
    static std::atomic_bool preserve_cfg_supported{true};
    const bool keep_targets = is_executable && preserve_cfg &&
                              preserve_cfg_supported.load(std::memory_order_relaxed);
    DWORD protection = is_executable ? PAGE_EXECUTE_READ : PAGE_READWRITE;
    if (keep_targets) {
        protection |= PAGE_TARGETS_NO_UPDATE;
    }
    BOOL protected_ok;
    if (is_executable) {
        const JitProfile::Timer rx_timer{keep_targets ? JitProfile::Phase::CfgPreserveRx
                                                    : JitProfile::Phase::CfgInitializeRx};
        protected_ok = DYNARMIC_VIRTUAL_PROTECT(const_cast<void*>(base), size, protection, &oldProtect);
    } else {
        protected_ok = DYNARMIC_VIRTUAL_PROTECT(const_cast<void*>(base), size, protection, &oldProtect);
    }
    if (!protected_ok && keep_targets) {
        const DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED) {
            // Some AppContainer runtimes may reject the modifier. Keep the established
            // RX path and stop retrying the unsupported flag on subsequent blocks.
            preserve_cfg_supported.store(false, std::memory_order_relaxed);
            protection = PAGE_EXECUTE_READ;
            const JitProfile::Timer fallback_timer{JitProfile::Phase::CfgFallback};
            protected_ok = DYNARMIC_VIRTUAL_PROTECT(const_cast<void*>(base), size, protection,
                                                    &oldProtect);
        }
    }
#        if defined(DYNARMIC_UWP_APPCONTAINER)
    if (!protected_ok) {
        RaiseJitMemoryFailure(DYNARMIC_UWP_PROTECT_FAILED, base, size, protection);
    }
#        else
    (void)protected_ok;
#        endif
#    else
    static const size_t pageSize = sysconf(_SC_PAGESIZE);
    const size_t iaddr = reinterpret_cast<size_t>(base);
    const size_t roundAddr = iaddr & ~(pageSize - static_cast<size_t>(1));
    const int mode = is_executable ? (PROT_READ | PROT_EXEC) : (PROT_READ | PROT_WRITE);
    mprotect(reinterpret_cast<void*>(roundAddr), size + (iaddr - roundAddr), mode);
#    endif
}
#endif

static const HostFeature features = []() {
    HostFeature f{};
#ifdef DYNARMIC_ENABLE_CPU_FEATURE_DETECTION
    using Cpu = Xbyak::util::Cpu;
    Xbyak::util::Cpu cpu_info{};
    if (cpu_info.has(Cpu::tSSSE3)) f |= HostFeature::SSSE3;
    if (cpu_info.has(Cpu::tSSE41)) f |= HostFeature::SSE41;
    if (cpu_info.has(Cpu::tSSE42)) f |= HostFeature::SSE42;
    if (cpu_info.has(Cpu::tAVX)) f |= HostFeature::AVX;
    if (cpu_info.has(Cpu::tAVX2)) f |= HostFeature::AVX2;
    if (cpu_info.has(Cpu::tAVX512F)) f |= HostFeature::AVX512F;
    if (cpu_info.has(Cpu::tAVX512CD)) f |= HostFeature::AVX512CD;
    if (cpu_info.has(Cpu::tAVX512VL)) f |= HostFeature::AVX512VL;
    if (cpu_info.has(Cpu::tAVX512BW)) f |= HostFeature::AVX512BW;
    if (cpu_info.has(Cpu::tAVX512DQ)) f |= HostFeature::AVX512DQ;
    if (cpu_info.has(Cpu::tAVX512_BITALG)) f |= HostFeature::AVX512BITALG;
    if (cpu_info.has(Cpu::tAVX512VBMI)) f |= HostFeature::AVX512VBMI;
    if (cpu_info.has(Cpu::tPCLMULQDQ)) f |= HostFeature::PCLMULQDQ;
    if (cpu_info.has(Cpu::tF16C)) f |= HostFeature::F16C;
    if (cpu_info.has(Cpu::tFMA)) f |= HostFeature::FMA;
    if (cpu_info.has(Cpu::tAESNI)) f |= HostFeature::AES;
    if (cpu_info.has(Cpu::tSHA)) f |= HostFeature::SHA;
    if (cpu_info.has(Cpu::tPOPCNT)) f |= HostFeature::POPCNT;
    if (cpu_info.has(Cpu::tBMI1)) f |= HostFeature::BMI1;
    if (cpu_info.has(Cpu::tBMI2)) f |= HostFeature::BMI2;
    if (cpu_info.has(Cpu::tLZCNT)) f |= HostFeature::LZCNT;
    if (cpu_info.has(Cpu::tGFNI)) f |= HostFeature::GFNI;
    if (cpu_info.has(Cpu::tWAITPKG)) f |= HostFeature::WAITPKG;
    if (cpu_info.has(Cpu::tBMI2)) {
        // BMI2 instructions such as pdep and pext have been very slow up until Zen 3.
        // Check for Zen 3 or newer by its family (0x19).
        // See also: https://en.wikichip.org/wiki/amd/cpuid
        if (cpu_info.has(Cpu::tAMD)) {
            std::array<u32, 4> data{};
            cpu_info.getCpuid(1, data.data());
            const u32 family_base = mcl::bit::get_bits<8, 11>(data[0]);
            const u32 family_extended = mcl::bit::get_bits<20, 27>(data[0]);
            const u32 family = family_base + family_extended;
            if (family >= 0x19)
                f |= HostFeature::FastBMI2;
        } else {
            f |= HostFeature::FastBMI2;
        }
    }
#ifdef DYNARMIC_HOST_FEATURE_OVERRIDE
    // Test builds only: DYNARMIC_HOST_FEATURES=<hex mask> hides features to reach the fallbacks.
    if (const char* mask = std::getenv("DYNARMIC_HOST_FEATURES"); mask && *mask)
        f &= static_cast<HostFeature>(std::strtoull(mask, nullptr, 16));
#endif
    return f;
#endif
}();

#ifdef __APPLE__
bool IsUnderRosetta() {
    int result = 0;
    size_t result_size = sizeof(result);
    if (sysctlbyname("sysctl.proc_translated", &result, &result_size, nullptr, 0) == -1) {
        if (errno != ENOENT)
            fmt::print("IsUnderRosetta: Failed to detect Rosetta state, assuming not under Rosetta");
        return false;
    }
    return result != 0;
}
#endif

}  // anonymous namespace

HostFeature GetHostFeatures() {
    return features;
}

BlockOfCode::BlockOfCode(RunCodeCallbacks cb, JitStateInfo jsi, size_t total_code_size, std::function<void(BlockOfCode&)> rcp)
    : Xbyak::CodeGenerator(total_code_size
#ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
        , Xbyak::DontSetProtectRWE
#else
        , nullptr //Allow RWE
#endif
#ifdef _WIN32
        , &s_allocator)
#else
        , nullptr)
#endif
    , constant_pool(*this, CONSTANT_POOL_SIZE)
    , jsi(jsi)
    , cb(std::move(cb))
{
    EnableWriting();
    EnsureMemoryCommitted(PRELUDE_COMMIT_SIZE);
#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
    // Code starts on a page of its own, so the constant pool pages below never need execute.
    align(WX_PAGE_SIZE);
    wx_exec_begin = getCurr<const u8*>();
#endif
    GenRunCode(rcp);
#ifdef _WIN32
    InstallEmitFaultHandler();
#endif
    for (auto& slot : s_live_code_caches) {
        const BlockOfCode* expected = nullptr;
        if (slot.compare_exchange_strong(expected, this, std::memory_order_release)) {
            break;
        }
    }
}

BlockOfCode::~BlockOfCode() {
    for (auto& slot : s_live_code_caches) {
        const BlockOfCode* expected = this;
        if (slot.compare_exchange_strong(expected, nullptr, std::memory_order_release)) {
            break;
        }
    }
#ifdef _WIN32
    // The code region is released with the Xbyak::CodeGenerator base, right after this.
    s_committed_code_bytes.fetch_sub(committed_size, std::memory_order_relaxed);
#endif
}

#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
bool BlockOfCode::AdoptWriteView() {
    u8* const write = DualWriteView(top_);
    if (write == nullptr) {
        return false;
    }
    writeOffset_ = write - top_;
    return true;
}
#endif

bool BlockOfCode::HasHostFeature(HostFeature feature) const noexcept {
    return (GetHostFeatures() & feature) == feature;
}

void BlockOfCode::PreludeComplete() {
    prelude_complete = true;
    code_begin = getCurr();
    ClearCache();
#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
    wx_exec_end = WxPageUp(static_cast<const u8*>(code_begin));
    if (!dual_mapped) {
        ProtectMemory(wx_exec_begin, wx_exec_end - wx_exec_begin, true);
    }
#else
    DisableWriting();
#endif
}

// Paged W^X on Windows. Flipping the whole committed code region on every block, as the other
// platforms do, costs two VirtualProtect calls over tens to hundreds of MiB per compiled block; on
// the UWP build it took ~90% of a guest core while a game booted. Only the pages actually written
// change instead: the page the next block starts in (shared with the end of the previous one),
// patch sites via MakeWritable, and nothing else, since everything past the emitted code is still
// RW. Each JIT instance has its own BlockOfCode, used from one thread, so a page is never RW while
// code in it can run.
void BlockOfCode::EnableWriting() {
#ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
#    ifdef _WIN32
    if (!prelude_complete || wx_writing || dual_mapped) {
        return; // before PreludeComplete everything committed is still RW
    }
    wx_writing = true;
    wx_write_begin = std::max(WxPageDown(getCurr<const u8*>()), wx_exec_begin);
    if (wx_write_begin < wx_exec_end) {
        ProtectMemory(wx_write_begin, wx_exec_end - wx_write_begin, false);
    }
#    else
    ProtectMemory(getCode(), maxSize_, false);
#    endif
#endif
}

void BlockOfCode::DisableWriting() {
#ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
#    ifdef _WIN32
    if (!prelude_complete || !wx_writing || dual_mapped) {
        return;
    }
    wx_writing = false;
    const u8* const write_end = WxPageUp(getCurr<const u8*>());
    // Return adjacent patch pages and the append window to RX in one syscall per
    // contiguous range. Every included page was already RW; gaps remain untouched.
    std::sort(wx_extra_pages.begin(), wx_extra_pages.end());
    RestoreWritableCodeRanges<u8>(wx_extra_pages, wx_write_begin, write_end, WX_PAGE_SIZE,
                                  [this](const u8* begin, size_t size) {
                                      const u8* const end = begin + size;
                                      // New pages must establish normal CFG call targets once.
                                      // Only previously executable pages may preserve that bitmap.
                                      const u8* const initialized_end = std::min(end, wx_exec_end);
                                      if (begin < initialized_end) {
                                          ProtectMemory(begin, initialized_end - begin, true, true);
                                      }
                                      const u8* const new_begin = std::max(begin, wx_exec_end);
                                      if (new_begin < end) {
                                          ProtectMemory(new_begin, end - new_begin, true);
                                      }
                                  });
    wx_exec_end = std::max(wx_exec_end, write_end);
    wx_extra_pages.clear();
#    else
    ProtectMemory(getCode(), maxSize_, true);
#    endif
#endif
}

void BlockOfCode::MakeWritable([[maybe_unused]] CodePtr where, [[maybe_unused]] size_t size) {
#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
    if (!prelude_complete || !wx_writing || dual_mapped) {
        return;
    }
    const u8* const begin = static_cast<const u8*>(where);
    for (const u8* page = WxPageDown(begin); page < begin + size; page += WX_PAGE_SIZE) {
        // Pages from the write window on are RW already.
        if (page < wx_exec_begin || page >= wx_write_begin ||
            std::find(wx_extra_pages.begin(), wx_extra_pages.end(), page) != wx_extra_pages.end()) {
            continue;
        }
        ProtectMemory(page, WX_PAGE_SIZE, false);
        wx_extra_pages.push_back(page);
    }
#endif
}

void BlockOfCode::ClearCache() {
    ASSERT(prelude_complete);
    SetCodePtr(code_begin);
#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
    // Code is emitted from code_begin again: hand the old blocks' pages back to the RW tail.
    const u8* const tail = WxPageUp(static_cast<const u8*>(code_begin));
    if (wx_exec_end != nullptr && tail < wx_exec_end && !dual_mapped) {
        ProtectMemory(tail, wx_exec_end - tail, false);
        wx_exec_end = tail;
    }
    ReleaseMemoryPastCap();
#endif
}

#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
// Under a releasing cap, a cleared cache gives back the memory past the cap (rounded up to a
// chunk), keeping the prelude. Nothing there holds code any more: ClearCache restarted emission at
// code_begin and returned the paged W^X pages past it to RW.
void BlockOfCode::ReleaseMemoryPastCap() {
    if (!s_code_cache_release.load(std::memory_order_relaxed)) {
        return;
    }
    const size_t cap = ScaledCap();
    const size_t prelude = static_cast<const u8*>(code_begin) - top_;
    const size_t keep = (std::max(cap, prelude) + CODE_CHUNK_SIZE - 1) & ~(CODE_CHUNK_SIZE - 1);
    if (cap == SIZE_MAX || committed_size <= keep) {
        return;
    }
    // A cache that cannot release (one section, whose pages cannot be decommitted, or pages still
    // being written) stops trying, or SpaceRemaining would clear it again on every block.
    const bool released = dual_mapped ? ReleaseDualMapped(top_, keep)
                                      : !wx_writing && VirtualFree(top_ + keep, WxPageUp(top_ + committed_size) - (top_ + keep),
                                                                   MEM_DECOMMIT);
    if (!released) {
        release_failed = true;
        return;
    }
    s_committed_code_bytes.fetch_sub(committed_size - keep, std::memory_order_relaxed);
    committed_size = keep;
    mapped_verified_end = 0;
    cap_end = CapEnd(cap);
}
#endif

size_t BlockOfCode::CapEnd(size_t cap) const {
#ifdef _WIN32
    const size_t committed = committed_size;
#else
    const size_t committed = maxSize_;
#endif
    if (cap == SIZE_MAX) {
        return SIZE_MAX;
    }
    return s_code_cache_release.load(std::memory_order_relaxed) ? cap : std::max(cap, committed + CAP_SLACK);
}

size_t BlockOfCode::ScaledCap() const noexcept {
    const size_t cap = s_code_cache_cap.load(std::memory_order_relaxed);
    if (cap == SIZE_MAX || cap > SIZE_MAX / cap_scale) {
        return SIZE_MAX;
    }
    return cap * cap_scale;
}

void BlockOfCode::SetCapScale(size_t scale) noexcept {
    cap_scale = std::max<size_t>(scale, 1);
    cap_epoch = SIZE_MAX; // Forces SpaceRemaining to recompute cap_end.
}

bool BlockOfCode::WritesWithoutProtectionChanges() const noexcept {
#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
#    if defined(_WIN32)
    return dual_mapped;
#    else
    return false;
#    endif
#else
    return true;
#endif
}

size_t BlockOfCode::SpaceRemaining() const {
    ASSERT(prelude_complete);
    const u8* current_ptr = getCurr<const u8*>();
    // Capped, a cache keeps the pages it has: clearing every cache at once recompiled 260k blocks
    // in 30 s. It is cleared only when it fills those, and commits no more.
    if (const size_t epoch = s_code_cache_cap_epoch.load(std::memory_order_acquire); epoch != cap_epoch) {
        cap_epoch = epoch;
        cap_end = CapEnd(ScaledCap());
    }
#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
    // A releasing cap clears a cache holding more than it allows even where its code would fit.
    if (cap_end != SIZE_MAX && committed_size > cap_end + CODE_CHUNK_SIZE && !release_failed &&
        s_code_cache_release.load(std::memory_order_relaxed)) {
        return 0;
    }
#endif
    const u8* const end = &top_[std::min(maxSize_, cap_end)];
    if (current_ptr >= end)
        return 0;
    return end - current_ptr;
}

void BlockOfCode::EnsureMemoryCommitted([[maybe_unused]] size_t codesize) {
    CommitCodeMemory(codesize);
#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
    // The chunks the next codesize bytes go to are checked once each against the memory itself,
    // not the bookkeeping: a write into one not mapped faulted with no trace of why.
    if (dual_mapped && size_ + codesize > mapped_verified_end) {
        const size_t end = std::min<size_t>(maxSize_, size_ + codesize);
        if (!VerifyDualMapped(top_, size_, end - size_)) {
#    if defined(DYNARMIC_UWP_APPCONTAINER)
            RaiseJitMemoryFailure(DYNARMIC_UWP_COMMIT_FAILED, top_ + size_, end - size_, PAGE_READWRITE);
#    endif
            return;
        }
        mapped_verified_end = (end + CODE_CHUNK_SIZE - 1) & ~(CODE_CHUNK_SIZE - 1);
    }
#endif
}

void BlockOfCode::CommitCodeMemory([[maybe_unused]] size_t codesize) {
#ifdef _WIN32
    if (committed_size < size_ + codesize) {
        [[maybe_unused]] const size_t old_committed_size = committed_size;
        // From the code pointer on: grown by codesize alone, a commit that trailed the code
        // pointer would stay behind it.
        committed_size = std::min<size_t>(maxSize_, std::max(committed_size, size_) + codesize);
        s_committed_code_bytes.fetch_add(committed_size - old_committed_size,
                                         std::memory_order_relaxed);
#    ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
        // W^X: commit read-write only; ProtectMemory() flips pages to RX before execution. Only
        // the pages not committed yet: committing again the ones holding emitted code would make
        // them RW.
        const u8* const commit_begin = WxPageUp(top_ + old_committed_size);
        const u8* const commit_end = WxPageUp(top_ + committed_size);
        if (dual_mapped) {
            if (commit_begin < commit_end &&
                !CommitDualMapped(top_, commit_begin - top_, commit_end - commit_begin)) {
#        if defined(DYNARMIC_UWP_APPCONTAINER)
                RaiseJitMemoryFailure(DYNARMIC_UWP_COMMIT_FAILED, commit_begin, commit_end - commit_begin,
                                      PAGE_READWRITE);
#        endif
            }
            return;
        }
        void* const committed = commit_begin >= commit_end
            ? top_
            : DYNARMIC_VIRTUAL_ALLOC(const_cast<u8*>(commit_begin), commit_end - commit_begin,
                                     MEM_COMMIT, PAGE_READWRITE);
#        if defined(DYNARMIC_UWP_APPCONTAINER)
        if (committed == nullptr) {
            RaiseJitMemoryFailure(DYNARMIC_UWP_COMMIT_FAILED, commit_begin, commit_end - commit_begin,
                                  PAGE_READWRITE);
        }
#        else
        (void)committed;
#        endif
#    else
        // RWX fast path — desktop only. Never compiled under DYNARMIC_UWP_APPCONTAINER, which
        // forces DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT on (the AppContainer never grants RWX).
        VirtualAlloc(top_, committed_size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#    endif
    }
#endif
}

int BlockOfCode::DescribeState(char* out, size_t size) const noexcept {
#ifdef _WIN32
    const u8* const next = top_ + size_;
    const u8* const write = next + writeOffset_;
    // State and type of the page a byte emitted now goes to, in both views.
    const auto query = [](const u8* p, MEMORY_BASIC_INFORMATION& info) {
        return VirtualQuery(p, &info, sizeof(info)) == sizeof(info);
    };
    MEMORY_BASIC_INFORMATION w{}, x{};
    const bool w_ok = query(write, w);
    const bool x_ok = query(next, x);
    size_t mapped_end = 0;
    int chunk_mapped = -1;
#    if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
    mapped_end = mapped_verified_end;
    // try_lock: the faulting thread may hold the views' lock.
    if (dual_mapped && s_dual_views_mutex.try_lock()) {
        if (const DualCodeView* const view = FindDualView(top_);
            view != nullptr && view->section == nullptr && size_ / CODE_CHUNK_SIZE < view->chunks.size()) {
            chunk_mapped = view->chunks[size_ / CODE_CHUNK_SIZE] ? 1 : 0;
        }
        s_dual_views_mutex.unlock();
    }
#    endif
    return std::snprintf(out, size,
                         "[jadyx]   code cache %p: top %p write %p size 0x%zx max 0x%zx committed 0x%zx "
                         "verified 0x%zx chunk %zu held %d | write page %s state 0x%lx type 0x%lx prot 0x%lx "
                         "base %p region 0x%zx | exec page %s state 0x%lx type 0x%lx prot 0x%lx\n",
                         static_cast<const void*>(this), static_cast<const void*>(top_),
                         static_cast<const void*>(write), size_, maxSize_, committed_size, mapped_end,
                         size_ / (4 * 1024 * 1024), chunk_mapped, w_ok ? "ok" : "query failed",
                         w.State, w.Type, w.Protect, w.AllocationBase, w.RegionSize,
                         x_ok ? "ok" : "query failed", x.State, x.Type, x.Protect);
#else
    return std::snprintf(out, size, "[jadyx]   code cache %p: size 0x%zx max 0x%zx\n",
                         static_cast<const void*>(this), size_, maxSize_);
#endif
}

BlockOfCode::EmitScope::EmitScope(const BlockOfCode& code) noexcept
        : previous{t_emitting} {
    t_emitting = &code;
}

BlockOfCode::EmitScope::~EmitScope() {
    t_emitting = previous;
    if (previous == nullptr) {
        if (t_emit_fault_retries != 0 && t_emit_fault_retries <= MAX_EMIT_FAULT_RETRIES) {
            s_emit_faults_recovered.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
            const auto took = static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, ::Common::VmActivity::NowUs() - t_emit_fault_start_us));
            s_emit_recover_us_total.fetch_add(took, std::memory_order_relaxed);
            std::uint64_t max = s_emit_recover_us_max.load(std::memory_order_relaxed);
            while (took > max &&
                   !s_emit_recover_us_max.compare_exchange_weak(max, took, std::memory_order_relaxed)) {
            }
#endif
        }
        t_emit_fault_retries = 0;
    }
}

HaltReason BlockOfCode::RunCode(void* jit_state, CodePtr code_ptr) const {
    return run_code(jit_state, code_ptr);
}

HaltReason BlockOfCode::StepCode(void* jit_state, CodePtr code_ptr) const {
    return step_code(jit_state, code_ptr);
}

void BlockOfCode::ReturnFromRunCode(bool mxcsr_already_exited) {
    size_t index = 0;
    if (mxcsr_already_exited)
        index |= MXCSR_ALREADY_EXITED;
    jmp(return_from_run_code[index]);
}

void BlockOfCode::ForceReturnFromRunCode(bool mxcsr_already_exited) {
    size_t index = FORCE_RETURN;
    if (mxcsr_already_exited)
        index |= MXCSR_ALREADY_EXITED;
    jmp(return_from_run_code[index]);
}

void BlockOfCode::GenRunCode(std::function<void(BlockOfCode&)> rcp) {
    Xbyak::Label return_to_caller, return_to_caller_mxcsr_already_exited;

    align();
    run_code = getCurr<RunCodeFuncType>();

    // This serves two purposes:
    // 1. It saves all the registers we as a callee need to save.
    // 2. It aligns the stack so that the code the JIT emits can assume
    //    that the stack is appropriately aligned for CALLs.
    ABI_PushCalleeSaveRegistersAndAdjustStack(*this, sizeof(StackLayout));

    mov(ABI_JIT_PTR, ABI_PARAM1);
    mov(rbx, ABI_PARAM2); // save temporarily in non-volatile register

    if (cb.enable_cycle_counting) {
        cb.GetTicksRemaining.EmitCall(*this);
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)], ABI_RETURN);
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], ABI_RETURN);
    }

    // r14 = page table
    // r13 = fastmem pointer
    rcp(*this);

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller_mxcsr_already_exited, T_NEAR);

    SwitchMxcsrOnEntry();
    jmp(rbx);

    align();
    step_code = getCurr<RunCodeFuncType>();

    ABI_PushCalleeSaveRegistersAndAdjustStack(*this, sizeof(StackLayout));

    mov(ABI_JIT_PTR, ABI_PARAM1);

    if (cb.enable_cycle_counting) {
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)], 1);
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], 1);
    }

    rcp(*this);

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller_mxcsr_already_exited, T_NEAR);
    lock(); or_(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], u32(HaltReason::Step));

    SwitchMxcsrOnEntry();
    jmp(ABI_PARAM2);

    // Dispatcher loop

    align();
    return_from_run_code[0] = getCurr<const void*>();

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller);
    if (cb.enable_cycle_counting) {
        cmp(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], 0);
        jng(return_to_caller);
    }
    cb.LookupBlock.EmitCall(*this);
    jmp(ABI_RETURN);

    align();
    return_from_run_code[MXCSR_ALREADY_EXITED] = getCurr<const void*>();

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller_mxcsr_already_exited);
    if (cb.enable_cycle_counting) {
        cmp(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], 0);
        jng(return_to_caller_mxcsr_already_exited);
    }
    SwitchMxcsrOnEntry();
    cb.LookupBlock.EmitCall(*this);
    jmp(ABI_RETURN);

    align();
    return_from_run_code[FORCE_RETURN] = getCurr<const void*>();
    L(return_to_caller);

    SwitchMxcsrOnExit();
    // fallthrough

    return_from_run_code[MXCSR_ALREADY_EXITED | FORCE_RETURN] = getCurr<const void*>();
    L(return_to_caller_mxcsr_already_exited);

    if (cb.enable_cycle_counting) {
        cb.AddTicks.EmitCall(*this, [this](RegList param) {
            mov(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)]);
            sub(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)]);
        });
    }

    xor_(eax, eax);
    xchg(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], eax);

    ABI_PopCalleeSaveRegistersAndAdjustStack(*this, sizeof(StackLayout));
    ret();

    PerfMapRegister(run_code, getCurr(), "dynarmic_dispatcher");
}

void BlockOfCode::SwitchMxcsrOnEntry() {
    stmxcsr(dword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, save_host_MXCSR)]);
    ldmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
}

void BlockOfCode::SwitchMxcsrOnExit() {
    stmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
    ldmxcsr(dword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, save_host_MXCSR)]);
}

void BlockOfCode::EnterStandardASIMD() {
    stmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
    ldmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_asimd_MXCSR]);
}

void BlockOfCode::LeaveStandardASIMD() {
    stmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_asimd_MXCSR]);
    ldmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
}

void BlockOfCode::UpdateTicks() {
    if (!cb.enable_cycle_counting) {
        return;
    }

    cb.AddTicks.EmitCall(*this, [this](RegList param) {
        mov(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)]);
        sub(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)]);
    });

    cb.GetTicksRemaining.EmitCall(*this);
    mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)], ABI_RETURN);
    mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], ABI_RETURN);
}

void BlockOfCode::LookupBlock() {
    cb.LookupBlock.EmitCall(*this);
}

void BlockOfCode::LoadRequiredFlagsForCondFromRax(IR::Cond cond) {
#ifdef __APPLE__
    static const bool is_rosetta = IsUnderRosetta();
#endif

    // sahf restores SF, ZF, CF
    // add al, 0x7F restores OF

    switch (cond) {
    case IR::Cond::EQ:  // z
    case IR::Cond::NE:  // !z
    case IR::Cond::CS:  // c
    case IR::Cond::CC:  // !c
    case IR::Cond::MI:  // n
    case IR::Cond::PL:  // !n
        sahf();
        break;
    case IR::Cond::VS:  // v
    case IR::Cond::VC:  // !v
        cmp(al, 0x81);
        break;
    case IR::Cond::HI:  // c & !z
    case IR::Cond::LS:  // !c | z
        sahf();
        cmc();
        break;
    case IR::Cond::GE:  // n == v
    case IR::Cond::LT:  // n != v
    case IR::Cond::GT:  // !z & (n == v)
    case IR::Cond::LE:  // z | (n != v)
#ifdef __APPLE__
        if (is_rosetta) {
            shl(al, 3);
            xchg(al, ah);
            push(rax);
            popf();
            break;
        }
#endif
        cmp(al, 0x81);
        sahf();
        break;
    case IR::Cond::AL:
    case IR::Cond::NV:
        break;
    default:
        UNREACHABLE();
    }
}

Xbyak::Address BlockOfCode::Const(const Xbyak::AddressFrame& frame, u64 lower, u64 upper) {
    return constant_pool.GetConstant(*this, frame, lower, upper);
}

CodePtr BlockOfCode::GetCodeBegin() const {
    return code_begin;
}

size_t BlockOfCode::GetTotalCodeSize() const {
    return maxSize_;
}

void* BlockOfCode::AllocateFromCodeSpace(size_t alloc_size) {
    if (size_ + alloc_size >= maxSize_) {
        using Xbyak::Error;
        XBYAK_THROW(Xbyak::ERR_CODE_IS_TOO_BIG);
    }

    EnsureMemoryCommitted(alloc_size);

    void* ret = getCurr<void*>();
    size_ += alloc_size;
    memset(WriteAlias(ret), 0, alloc_size);
    return ret;
}

void BlockOfCode::SetCodePtr(CodePtr code_ptr) {
    // The "size" defines where top_, the insertion point, is.
    size_t required_size = reinterpret_cast<const u8*>(code_ptr) - getCode();
    setSize(required_size);
}

void BlockOfCode::EnsurePatchLocationSize(CodePtr begin, size_t size) {
    size_t current_size = getCurr<const u8*>() - reinterpret_cast<const u8*>(begin);
    ASSERT(current_size <= size);
    nop(size - current_size);
}

}  // namespace Dynarmic::Backend::X64
