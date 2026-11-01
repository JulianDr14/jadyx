// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <share.h>
#include <string>
#include <string_view>

#include <windows.h>

#include "common/bug_tracker.h"
#include "common/common_types.h"
#include "common/dynamic_library.h"
#include "common/host_memory.h"
#include "common/logging.h"
#include "jadyx/app/boot_crash.h"
#include "jadyx/app/boot_diag.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace Jadyx {

namespace {

std::string FormatFaultStack(const CONTEXT& fault, const char* prefix);

// Unhandled SEH (access violation, illegal instruction, breakpoint from a soft assert, ...) on any
// thread. /EHsc catch(...) does not see these, which is why the boot worker's handlers stay silent.
// Reports the faulting address as an RVA into jadyx.exe so it can be resolved with the build's PDB.
LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    const auto base = reinterpret_cast<uintptr_t>(&__ImageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew);
    const auto addr = reinterpret_cast<uintptr_t>(rec->ExceptionAddress);

    char where[64];
    if (addr >= base && addr < base + nt->OptionalHeader.SizeOfImage) {
        std::snprintf(where, sizeof(where), "jadyx.exe+0x%llx",
                      static_cast<unsigned long long>(addr - base));
    } else {
        std::snprintf(where, sizeof(where), "0x%llx (outside jadyx.exe: JIT code or a DLL)",
                      static_cast<unsigned long long>(addr));
    }

    char line[320];
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const ULONG_PTR kind = rec->ExceptionInformation[0];
        const char* what = kind == 0 ? "read of" : kind == 1 ? "write to" : "execute at";
        std::snprintf(line, sizeof(line),
                      "[jadyx] [+%llums] CRASH: access violation at %s (%s 0x%llx), thread %lu\n",
                      ElapsedMs(), where, what,
                      static_cast<unsigned long long>(rec->ExceptionInformation[1]),
                      GetCurrentThreadId());
    } else {
        std::snprintf(line, sizeof(line),
                      "[jadyx] [+%llums] CRASH: exception 0x%08lx at %s, thread %lu\n",
                      ElapsedMs(), static_cast<unsigned long>(rec->ExceptionCode), where,
                      GetCurrentThreadId());
    }
    WriteDiagRaw(line);
    // Before the stack: out of commit, the unwind can overflow the stack and lose both.
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        char fault[512];
        Common::FormatBackingFault(fault, sizeof(fault), rec->ExceptionInformation[1]);
        if (fault[0] != '\0') {
            std::snprintf(line, sizeof(line), "[jadyx] [+%llums] ", ElapsedMs());
            WriteDiagRaw(line);
            WriteDiagRaw(fault);
            WriteDiagRaw("\n");
        }
    }
    WriteDiagRaw(FormatFaultStack(*info->ContextRecord, "[jadyx] crash stack:").c_str());
    Common::BugTracker::DrainForCrash(); // first occurrences still queued for jadyx_graphics_bugs.log
    Common::Log::Stop(); // flush jadyx_log.txt
    return EXCEPTION_CONTINUE_SEARCH; // let the OS finish the crash (and WER take its dump)
}

// Fastmem faults are expected and use up the line budget within seconds, so every thread also
// keeps its latest exception; OnAbort prints it, since a fault dynarmic cannot place aborts there.
thread_local char t_last_exception[512];

std::string FormatStack(const char* prefix);

// Eden's fatal ASSERT/UNREACHABLE and the default std::terminate both end in abort(). SIGABRT is
// process-wide in the UCRT, so this sees it from any thread.
void OnAbort(int) {
    char line[200];
    std::snprintf(line, sizeof(line),
                  "[jadyx] [+%llums] CRASH: abort() on thread %lu - fatal ASSERT/UNREACHABLE "
                  "(see jadyx\\log\\jadyx_log.txt) or an exception escaped a thread\n",
                  ElapsedMs(), GetCurrentThreadId());
    WriteDiagRaw(line);
    if (t_last_exception[0] != '\0') {
        WriteDiagRaw("[jadyx] last exception on this thread:\n");
        WriteDiagRaw(t_last_exception);
    }
    if (const std::string stats = Common::HostMemoryCommitStats(); !stats.empty()) {
        WriteDiagRaw(("[jadyx] " + stats + "\n").c_str());
    }
    // An exception escaping a worker thread leaves nothing else behind.
    WriteDiagRaw(FormatStack("[jadyx] abort stack:").c_str());
    // Without this the async logger loses whatever it had queued (the last lines before a
    // std::terminate on a worker thread, which never passes through AssertFatalImpl).
    Common::BugTracker::DrainForCrash();
    Common::Log::Stop();
}

// The calling thread's stack, as RVAs into jadyx.exe (resolve them with the build's PDB) or
// module+offset elsewhere, after prefix, as one line.
std::string FormatStack(const char* prefix) {
    void* frames[48];
    const USHORT count = RtlCaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)),
                                                  frames, nullptr);
    return FormatFrames(prefix, frames, count);
}

// The faulting thread's stack, unwound from the fault's context: one captured inside the handler
// would start in the exception dispatcher. A frame without unwind data (JIT code) is taken as a
// leaf, so the walk may stop or skip there.
std::string FormatFaultStack(const CONTEXT& fault, const char* prefix) {
    CONTEXT context = fault;
    void* frames[48];
    USHORT count = 0;
    while (count < std::size(frames) && context.Rip != 0) {
        frames[count++] = reinterpret_cast<void*>(context.Rip);
        DWORD64 image_base{};
        if (RUNTIME_FUNCTION* const function =
                RtlLookupFunctionEntry(context.Rip, &image_base, nullptr)) {
            void* handler_data{};
            DWORD64 establisher{};
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                             &handler_data, &establisher, nullptr);
        } else {
            MEMORY_BASIC_INFORMATION region{};
            if (context.Rsp == 0 ||
                VirtualQuery(reinterpret_cast<const void*>(context.Rsp), &region,
                             sizeof(region)) == 0 ||
                region.State != MEM_COMMIT) {
                break;
            }
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += 8;
        }
    }
    return FormatFrames(prefix, frames, count);
}

} // namespace

std::string FormatFrames(const char* prefix, void* const* frames, USHORT count) {
    const auto base = reinterpret_cast<std::uintptr_t>(&__ImageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew);
    std::string stack = prefix;
    for (USHORT i = 0; i < count; ++i) {
        const auto addr = reinterpret_cast<std::uintptr_t>(frames[i]);
        char frame[160];
        HMODULE module = nullptr;
        char module_path[MAX_PATH] = "";
        if (addr >= base && addr < base + nt->OptionalHeader.SizeOfImage) {
            std::snprintf(frame, sizeof(frame), " +0x%llx",
                          static_cast<unsigned long long>(addr - base));
        } else if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      static_cast<LPCSTR>(frames[i]), &module) &&
                   GetModuleFileNameA(module, module_path, MAX_PATH) != 0) {
            // Another module (spirv_to_dxil.dll, the runtime, ...): its name and offset.
            const char* name = std::strrchr(module_path, '\\');
            std::snprintf(frame, sizeof(frame), " %s+0x%llx", name ? name + 1 : module_path,
                          static_cast<unsigned long long>(
                              addr - reinterpret_cast<std::uintptr_t>(module)));
        } else {
            std::snprintf(frame, sizeof(frame), " 0x%llx", static_cast<unsigned long long>(addr));
        }
        stack += frame;
    }
    stack += '\n';
    return stack;
}

// Terminate handlers are per-thread in the MSVC runtime; installed on the boot worker, this names the
// exception that escaped (e.g. std::bad_alloc from the 4 GiB backing reservation).
[[noreturn]] void OnTerminate() {
    char line[320];
    const char* what = "unknown (not a std::exception)";
    std::string msg;
    if (const std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            msg = ex.what();
            what = msg.c_str();
        } catch (...) {
        }
    } else {
        what = "no active exception (std::terminate called directly)";
    }
    std::snprintf(line, sizeof(line), "[jadyx] [+%llums] CRASH: std::terminate: %s\n",
                  ElapsedMs(), what);
    WriteDiagRaw(line);
    std::abort();
}

namespace {

// ---- First-chance logging ------------------------------------------------------------------
// The unhandled filter above never runs for a fault inside JIT code when the unwinder cannot walk
// back out of it: the process just disappears (observed on-console: log stops ~300 ms after
// system.Run(), no CRASH line, no heartbeat). A vectored handler runs BEFORE any unwinding, so it
// sees every fault. It is installed LAST in the chain, so the demand-commit handlers (DRAM backing,
// VirtualBuffer) resolve their own expected faults first and never reach it.
std::atomic<int> g_first_chance_lines{0};
constexpr int MAX_FIRST_CHANCE_LINES = 32;

const char* ProtectName(DWORD protect) {
    switch (protect & 0xFF) {
    case PAGE_NOACCESS:          return "NOACCESS";
    case PAGE_READONLY:          return "R";
    case PAGE_READWRITE:         return "RW";
    case PAGE_WRITECOPY:         return "WC";
    case PAGE_EXECUTE:           return "X";
    case PAGE_EXECUTE_READ:      return "RX";
    case PAGE_EXECUTE_READWRITE: return "RWX";
    case PAGE_EXECUTE_WRITECOPY: return "XWC";
    default:                     return "?";
    }
}

// "jadyx.exe+0xRVA" inside the image; otherwise the region's state/type/protection, which is what
// tells JIT code (private RX), a code page left RW (W^X failure) and unmapped memory apart.
void DescribeAddress(std::uintptr_t addr, char* out, std::size_t out_size) {
    const auto base = reinterpret_cast<std::uintptr_t>(&__ImageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew);
    if (addr >= base && addr < base + nt->OptionalHeader.SizeOfImage) {
        std::snprintf(out, out_size, "jadyx.exe+0x%llx",
                      static_cast<unsigned long long>(addr - base));
        return;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) {
        std::snprintf(out, out_size, "0x%llx [VirtualQuery failed]",
                      static_cast<unsigned long long>(addr));
        return;
    }
    const char* state = mbi.State == MEM_COMMIT    ? "commit"
                        : mbi.State == MEM_RESERVE ? "reserved"
                                                   : "free";
    const char* type = mbi.Type == MEM_PRIVATE ? "private"
                       : mbi.Type == MEM_MAPPED ? "mapped"
                       : mbi.Type == MEM_IMAGE  ? "image"
                                                : "-";
    std::snprintf(out, out_size, "0x%llx [%s %s %s, alloc base 0x%llx]",
                  static_cast<unsigned long long>(addr), state, type,
                  mbi.State == MEM_COMMIT ? ProtectName(mbi.Protect) : "-",
                  static_cast<unsigned long long>(
                      reinterpret_cast<std::uintptr_t>(mbi.AllocationBase)));
}

// C++ throws are not failures by themselves, but one that escapes a thread other than the boot
// worker ends the process through a per-thread terminate handler nobody installed: no CRASH line,
// just a WER event. Name them (type and what()) so the log says what was thrown.
std::atomic<int> g_cxx_throw_lines{0};
constexpr int MAX_CXX_THROW_LINES = 16;

void LogCxxThrow(const EXCEPTION_RECORD* rec) {
    // MSVC x64 throw: [1] = thrown object, [2] = ThrowInfo, [3] = image base of the RVAs.
    if (rec->NumberParameters < 4) {
        return;
    }
    const auto object = static_cast<std::uintptr_t>(rec->ExceptionInformation[1]);
    const auto* throw_info = reinterpret_cast<const s32*>(rec->ExceptionInformation[2]);
    const auto image = static_cast<std::uintptr_t>(rec->ExceptionInformation[3]);
    if (throw_info == nullptr || image == 0) {
        return;
    }
    // ThrowInfo { attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray }
    const auto* types = reinterpret_cast<const s32*>(image + throw_info[3]);
    const char* first_name = "?";
    const char* what = "";
    for (s32 i = 0; i < types[0]; ++i) {
        // CatchableType { properties, pType, mdisp, pdisp, vdisp, sizeOrOffset, copyFunction }
        const auto* type = reinterpret_cast<const s32*>(image + types[1 + i]);
        // TypeDescriptor { pVFTable, spare, name[] }
        const char* name = reinterpret_cast<const char*>(image + type[1] + 2 * sizeof(void*));
        if (i == 0) {
            first_name = name;
        }
        if (std::string_view{name} == ".?AVexception@std@@" && object != 0) {
            what = reinterpret_cast<const std::exception*>(object + type[2])->what();
        }
    }
    // Pulse rejects by throwing and retries for up to 3 s: each attempt is one line of its own log
    // (the reason it waited on), never the diag's budget or a stack.
    if (std::string_view{first_name}.find("@Pulse@") != std::string_view::npos) {
        WritePulseRejection(what);
        return;
    }
    // The platform throws dozens of _com_error at startup and handles them itself; they used up
    // the budget before anything interesting was thrown.
    if (std::string_view{first_name} == ".?AV_com_error@@" ||
        g_cxx_throw_lines.fetch_add(1, std::memory_order_relaxed) >= MAX_CXX_THROW_LINES) {
        return;
    }
    char line[512];
    std::snprintf(line, sizeof(line), "[jadyx] [+%llums] C++ throw %s: %s | thread %lu\n",
                  ElapsedMs(), first_name, what, GetCurrentThreadId());
    WriteDiagRaw(line, /*debugger_channel=*/false);
    // Where it was thrown from: a caught exception (a bad_alloc a cache recovers from) is
    // otherwise untraceable.
    WriteDiagRaw(FormatStack("[jadyx]   thrown at:").c_str(), /*debugger_channel=*/false);
}

LONG NTAPI FirstChanceLogger(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    switch (rec->ExceptionCode) {
    case 0xE06D7363: // C++ throw
        LogCxxThrow(rec);
        return EXCEPTION_CONTINUE_SEARCH;
    case 0x406D1388: // SetThreadDescription-by-exception (thread naming)
    case 0x40010006: // DBG_PRINTEXCEPTION_C (OutputDebugStringA)
    case 0x4001000A: // DBG_PRINTEXCEPTION_WIDE_C (OutputDebugStringW)
        return EXCEPTION_CONTINUE_SEARCH;
    default:
        break;
    }
    char at[160];
    DescribeAddress(reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress), at, sizeof(at));

    char line[512];
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const ULONG_PTR kind = rec->ExceptionInformation[0];
        const char* what = kind == 0 ? "read of" : kind == 1 ? "write to" : "execute at";
        char target[160];
        DescribeAddress(static_cast<std::uintptr_t>(rec->ExceptionInformation[1]), target,
                        sizeof(target));
        std::snprintf(line, sizeof(line),
                      "[jadyx] [+%llums] FIRST-CHANCE access violation: %s %s | at %s | "
                      "thread %lu\n",
                      ElapsedMs(), what, target, at, GetCurrentThreadId());
    } else if ((rec->ExceptionCode == 0xE0DA0001 || rec->ExceptionCode == 0xE0DA0002) &&
               rec->NumberParameters >= 4) {
        std::snprintf(line, sizeof(line),
                      "[jadyx] [+%llums] JIT MEMORY: %s failed, GetLastError=%llu, base 0x%llx, "
                      "size 0x%llx, protection %s | thread %lu\n",
                      ElapsedMs(),
                      rec->ExceptionCode == 0xE0DA0001 ? "VirtualProtectFromApp"
                                                       : "VirtualAllocFromApp(MEM_COMMIT)",
                      static_cast<unsigned long long>(rec->ExceptionInformation[0]),
                      static_cast<unsigned long long>(rec->ExceptionInformation[1]),
                      static_cast<unsigned long long>(rec->ExceptionInformation[2]),
                      ProtectName(static_cast<DWORD>(rec->ExceptionInformation[3])),
                      GetCurrentThreadId());
    } else {
        std::snprintf(line, sizeof(line),
                      "[jadyx] [+%llums] FIRST-CHANCE exception 0x%08lx | at %s | params %lu "
                      "[0x%llx 0x%llx] | thread %lu\n",
                      ElapsedMs(), static_cast<unsigned long>(rec->ExceptionCode), at,
                      static_cast<unsigned long>(rec->NumberParameters),
                      static_cast<unsigned long long>(
                          rec->NumberParameters > 0 ? rec->ExceptionInformation[0] : 0),
                      static_cast<unsigned long long>(
                          rec->NumberParameters > 1 ? rec->ExceptionInformation[1] : 0),
                      GetCurrentThreadId());
    }
    std::memcpy(t_last_exception, line, sizeof(t_last_exception));
    if (g_first_chance_lines.fetch_add(1, std::memory_order_relaxed) < MAX_FIRST_CHANCE_LINES) {
        WriteDiagRaw(line, /*debugger_channel=*/false);
    }
    return EXCEPTION_CONTINUE_SEARCH; // observe only
}

// Resolved by name, never imported: importing the errorhandling-l1-1-1 api-set makes the app fail
// to activate on-console (see host_memory.cpp).
void InstallFirstChanceLogger() {
    using PFN_AddVectoredExceptionHandler = PVOID(WINAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER);
    static Common::DynamicLibrary kernelbase("Kernelbase");
    PFN_AddVectoredExceptionHandler add_veh{};
    if (kernelbase.IsOpen() && kernelbase.GetSymbol("AddVectoredExceptionHandler", &add_veh) &&
        add_veh(/*first=*/0, FirstChanceLogger) != nullptr) {
        WriteDiag("first-chance exception logger installed");
    } else {
        WriteDiag("WARNING: could not install the first-chance exception logger");
    }
}

} // namespace

void InstallCrashHandlers() {
    InstallFirstChanceLogger();
    SetUnhandledExceptionFilter(OnUnhandledException);
    std::signal(SIGABRT, OnAbort);
    std::set_terminate(OnTerminate);
}

} // namespace Jadyx
