// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
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
#include <thread>

#include <windows.h>

#include "common/bug_tracker.h"
#include "common/common_types.h"
#include "common/dynamic_library.h"
#include "common/host_memory.h"
#include "common/logging.h"
#include "common/thread.h"
#include "dynarmic/interface/code_memory.h"
#include "jadyx/app/boot_crash.h"
#include "jadyx/app/boot_diag.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace Jadyx {

namespace {

std::string FormatFaultStack(const CONTEXT& fault, const char* prefix);
void ReportStackOverflow(const CONTEXT& fault);

// Unhandled SEH (access violation, illegal instruction, breakpoint from a soft assert, ...) on any
// thread. /EHsc catch(...) does not see these, which is why the boot worker's handlers stay silent.
// Reports the faulting address as an RVA into jadyx.exe so it can be resolved with the build's PDB.
LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    const auto base = reinterpret_cast<uintptr_t>(&__ImageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew);
    const auto addr = reinterpret_cast<uintptr_t>(rec->ExceptionAddress);

    char where[160];
    HMODULE module = nullptr;
    if (addr >= base && addr < base + nt->OptionalHeader.SizeOfImage) {
        std::snprintf(where, sizeof(where), "jadyx.exe+0x%llx",
                      static_cast<unsigned long long>(addr - base));
    } else if (rec->ExceptionCode != EXCEPTION_STACK_OVERFLOW &&
               GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  static_cast<LPCSTR>(rec->ExceptionAddress), &module)) {
        // A DLL: its name and offset (the driver's compiler, spirv_to_dxil, the runtime, ...).
        char path[MAX_PATH] = "";
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = std::strrchr(path, '\\');
        std::snprintf(where, sizeof(where), "%s+0x%llx (0x%llx)", name ? name + 1 : path,
                      static_cast<unsigned long long>(
                          addr - reinterpret_cast<std::uintptr_t>(module)),
                      static_cast<unsigned long long>(addr));
    } else {
        std::snprintf(where, sizeof(where), "0x%llx (outside jadyx.exe: JIT code or a DLL)",
                      static_cast<unsigned long long>(addr));
    }

    char line[400];
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const ULONG_PTR kind = rec->ExceptionInformation[0];
        const char* what = kind == 0 ? "read of" : kind == 1 ? "write to" : "execute at";
        std::snprintf(line, sizeof(line),
                      "[jadyx] [+%llums] CRASH: access violation at %s (%s 0x%llx), thread %lu "
                      "(%s)\n",
                      ElapsedMs(), where, what,
                      static_cast<unsigned long long>(rec->ExceptionInformation[1]),
                      GetCurrentThreadId(), Common::GetCurrentThreadName());
    } else {
        std::snprintf(line, sizeof(line),
                      "[jadyx] [+%llums] CRASH: exception 0x%08lx at %s, thread %lu (%s)\n",
                      ElapsedMs(), static_cast<unsigned long>(rec->ExceptionCode), where,
                      GetCurrentThreadId(), Common::GetCurrentThreadName());
    }
    WriteDiagRaw(line);
    if (rec->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        // No stack left here for the walk and the flushes below: the reporter thread does them.
        ReportStackOverflow(*info->ContextRecord);
        return EXCEPTION_CONTINUE_SEARCH;
    }
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

// A stack overflow leaves the faulting thread a few KiB: on the console the CRASH line was the
// last thing written, with no stack and the log never flushed. A thread started with the handlers
// walks the faulting stack and flushes while the faulting thread waits for it.
struct OverflowReport {
    CONTEXT context;
    HANDLE ready{};
    HANDLE done{}; // manual reset: the first-chance and the unhandled handler both wait on it
    std::atomic<bool> taken{false};
};
OverflowReport g_overflow;

// The recursion fills the top of the stack with one function; the caller that started it is at
// the bottom. Keeps both ends and the depth.
void WriteOverflowStack(const CONTEXT& fault) {
    constexpr size_t TOP = 12;
    constexpr size_t BOTTOM = 36;
    constexpr size_t MAX_DEPTH = 1u << 20;
    void* top[TOP];
    void* bottom[BOTTOM];
    size_t depth = 0;
    CONTEXT context = fault;
    while (depth < MAX_DEPTH && context.Rip != 0) {
        void* const frame = reinterpret_cast<void*>(context.Rip);
        if (depth < TOP) {
            top[depth] = frame;
        }
        bottom[depth % BOTTOM] = frame;
        ++depth;
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
    const DWORD64 stack_used = context.Rsp > fault.Rsp ? context.Rsp - fault.Rsp : 0;
    char line[160];
    std::snprintf(line, sizeof(line),
                  "[jadyx] stack overflow: %zu frames, %llu KiB of stack walked\n", depth,
                  static_cast<unsigned long long>(stack_used >> 10));
    WriteDiagRaw(line);
    WriteDiagRaw(FormatFrames("[jadyx] stack overflow top:", top,
                              static_cast<USHORT>((std::min)(depth, TOP)))
                     .c_str());
    if (depth > TOP) {
        void* ordered[BOTTOM];
        const size_t count = (std::min)(depth - TOP, BOTTOM);
        for (size_t i = 0; i < count; ++i) {
            ordered[i] = bottom[(depth - count + i) % BOTTOM];
        }
        WriteDiagRaw(FormatFrames("[jadyx] stack overflow bottom:", ordered,
                                  static_cast<USHORT>(count))
                         .c_str());
    }
}

void RunOverflowReporter() {
    WaitForSingleObject(g_overflow.ready, INFINITE);
    WriteOverflowStack(g_overflow.context);
    Common::BugTracker::DrainForCrash();
    Common::Log::Stop();
    SetEvent(g_overflow.done);
}

void ReportStackOverflow(const CONTEXT& fault) {
    if (g_overflow.ready == nullptr) {
        return;
    }
    if (!g_overflow.taken.exchange(true)) {
        std::memcpy(&g_overflow.context, &fault, sizeof(CONTEXT));
        SetEvent(g_overflow.ready);
    }
    WaitForSingleObject(g_overflow.done, 10000);
}

void StartOverflowReporter() {
    g_overflow.ready = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    g_overflow.done =
        CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
    if (g_overflow.ready == nullptr || g_overflow.done == nullptr) {
        g_overflow.ready = nullptr;
        return;
    }
    std::thread(RunOverflowReporter).detach();
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
// Registers and stack for access violations in jadyx.exe itself; few, since each walks a stack.
std::atomic<int> g_fault_detail_lines{0};
constexpr int MAX_FAULT_DETAILS = 4;

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
void DescribeAddress(std::uintptr_t addr, char* out, std::size_t out_size, bool name_module) {
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
    // A DLL's name: "image RX" alone left a driver crash unattributed.
    char module_name[96] = "";
    if (name_module && mbi.Type == MEM_IMAGE && mbi.AllocationBase != nullptr) {
        char path[MAX_PATH] = "";
        if (GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), path, MAX_PATH) != 0) {
            const char* name = std::strrchr(path, '\\');
            std::snprintf(module_name, sizeof(module_name), " %s+0x%llx",
                          name ? name + 1 : path,
                          static_cast<unsigned long long>(
                              addr - reinterpret_cast<std::uintptr_t>(mbi.AllocationBase)));
        }
    }
    std::snprintf(out, out_size, "0x%llx [%s %s %s, alloc base 0x%llx]%s",
                  static_cast<unsigned long long>(addr), state, type,
                  mbi.State == MEM_COMMIT ? ProtectName(mbi.Protect) : "-",
                  static_cast<unsigned long long>(
                      reinterpret_cast<std::uintptr_t>(mbi.AllocationBase)),
                  module_name);
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
    // A stack overflow leaves this thread a few KiB: no module lookup there, the reporter names it.
    const bool stack_overflow = rec->ExceptionCode == EXCEPTION_STACK_OVERFLOW;
    char at[256];
    DescribeAddress(reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress), at, sizeof(at),
                    !stack_overflow);

    char line[512];
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const ULONG_PTR kind = rec->ExceptionInformation[0];
        const char* what = kind == 0 ? "read of" : kind == 1 ? "write to" : "execute at";
        char target[160];
        DescribeAddress(static_cast<std::uintptr_t>(rec->ExceptionInformation[1]), target,
                        sizeof(target), true);
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
    } else if (rec->ExceptionCode == 0xE0DA0003 && rec->NumberParameters >= 4) {
        std::snprintf(line, sizeof(line),
                      "[jadyx] [+%llums] JIT MEMORY: write view of a mapped code chunk is gone, "
                      "base 0x%llx, region 0x%llx, state|type 0x%llx | thread %lu\n",
                      ElapsedMs(), static_cast<unsigned long long>(rec->ExceptionInformation[1]),
                      static_cast<unsigned long long>(rec->ExceptionInformation[2]),
                      static_cast<unsigned long long>(rec->ExceptionInformation[3]),
                      GetCurrentThreadId());
    } else {
        std::snprintf(line, sizeof(line),
                      "[jadyx] [+%llums] FIRST-CHANCE exception 0x%08lx | at %s | params %lu "
                      "[0x%llx 0x%llx] | thread %lu (%s)\n",
                      ElapsedMs(), static_cast<unsigned long>(rec->ExceptionCode), at,
                      static_cast<unsigned long>(rec->NumberParameters),
                      static_cast<unsigned long long>(
                          rec->NumberParameters > 0 ? rec->ExceptionInformation[0] : 0),
                      static_cast<unsigned long long>(
                          rec->NumberParameters > 1 ? rec->ExceptionInformation[1] : 0),
                      GetCurrentThreadId(), Common::GetCurrentThreadName());
    }
    std::memcpy(t_last_exception, line, sizeof(t_last_exception));
    if (stack_overflow) {
        // Past the line budget too: the unhandled filter did not run for the first one seen on
        // the console, so this is the only report it gets.
        WriteDiagRaw(line, /*debugger_channel=*/false);
        ReportStackOverflow(*ep->ContextRecord);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_first_chance_lines.fetch_add(1, std::memory_order_relaxed) < MAX_FIRST_CHANCE_LINES) {
        WriteDiagRaw(line, /*debugger_channel=*/false);
    }
    // An access violation in our own code (not a fastmem fault in JIT code) can kill the process
    // before the unhandled filter runs: keep its registers and stack here.
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        std::strncmp(at, "jadyx.exe+", 10) == 0 &&
        g_fault_detail_lines.fetch_add(1, std::memory_order_relaxed) < MAX_FAULT_DETAILS) {
        const CONTEXT& c = *ep->ContextRecord;
        std::snprintf(line, sizeof(line),
                      "[jadyx]   regs rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx "
                      "rsp %llx rbp %llx r8 %llx r9 %llx r12 %llx r13 %llx r14 %llx r15 %llx\n",
                      c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rsp, c.Rbp, c.R8, c.R9, c.R12,
                      c.R13, c.R14, c.R15);
        WriteDiagRaw(line, /*debugger_channel=*/false);
        WriteDiagRaw(FormatFaultStack(c, "[jadyx]   fault stack:").c_str(),
                     /*debugger_channel=*/false);
        // A fault emitting JIT code: where each code cache writes next, and that memory's state.
        char caches[2048];
        if (Dynarmic::DescribeCodeCaches(caches, sizeof(caches)) != 0) {
            WriteDiagRaw(caches, /*debugger_channel=*/false);
        }
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
    StartOverflowReporter();
    InstallFirstChanceLogger();
    SetUnhandledExceptionFilter(OnUnhandledException);
    std::signal(SIGABRT, OnAbort);
    std::set_terminate(OnTerminate);
}

} // namespace Jadyx
