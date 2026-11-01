// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <share.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>

#include "common/commit_census.h"
#include "common/host_memory.h"
#include "common/log_rotation.h"
#include "common/memory_ledger.h"
#include "core/core.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/kernel.h"
#include "dynarmic/interface/code_memory.h"
#include "jadyx/app/boot_diag.h"
#include "jadyx/app/boot_renderer_decls.h"
#include "jadyx/app/boot_state.h"

using namespace winrt;
using namespace Windows::ApplicationModel::Core;

namespace Jadyx {

namespace {

// Best-effort startup diagnostics that survive an early crash (before Eden's own logging is up):
// append to a pullable file in the app's LocalFolder AND emit on the debugger channel. This is how we
// see *where* the headless boot fails on-console when no jadyx_log.txt and no crash dump are produced.
//
// Every line carries milliseconds since launch, so a hang (steps stop, heartbeats continue) reads
// differently from a crash (steps stop, a CRASH line follows, or nothing at all).
//
// The diag path is resolved once, on the UI thread before the boot worker exists, and cached: the
// crash handlers (boot_crash.cpp) must not call into WinRT from a dying process.
std::string g_diag_path;
std::mutex g_diag_mutex;
// Pulse's own log (jadyx_pulse.txt): saves, resumes and every rejection, apart from the diag.
std::string g_pulse_path;
std::mutex g_pulse_mutex;
// A save retries for up to 3 s and throws the same rejection on every attempt: identical
// consecutive rejections are counted under g_pulse_mutex and written as one line.
std::string g_pulse_last_rejection;
unsigned g_pulse_rejection_repeats = 0;

// Under g_pulse_mutex: the line closing a run of repeated rejections (empty when there is none).
std::string TakePulseRepeats() {
    std::string line;
    if (g_pulse_rejection_repeats != 0) {
        line = "[pulse]   (repeated " + std::to_string(g_pulse_rejection_repeats) + " more times)\n";
    }
    g_pulse_last_rejection.clear();
    g_pulse_rejection_repeats = 0;
    return line;
}

// LocalState\jadyx\log, next to jadyx_log.txt. Created here: the diag starts before Eden's
// logging creates the folder. These logs append across launches, so one past 8 MiB moves to its
// .old copy first instead of growing without end.
std::string LogFilePath(const char* name) {
    constexpr std::uintmax_t MAX_LOG_BYTES = 8ULL << 20;
    const std::filesystem::path dir =
        std::filesystem::path{std::wstring{Windows::Storage::ApplicationData::Current().LocalFolder().Path()}} /
        L"jadyx" / L"log";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / name;
    Common::RotateLogIfLarger(path, MAX_LOG_BYTES);
    return winrt::to_string(path.wstring());
}

std::wstring PrewarmStepKey(u64 program_id) {
    wchar_t key[40];
    // v2: the steps stored before Permafrost held the headroom near 1024 MiB were lowered on a
    // single dip below 256 and never raised again, so every game restarts at 192 MiB per core.
    swprintf(key, std::size(key), L"jit_prewarm_v2_%016llx",
             static_cast<unsigned long long>(program_id));
    return key;
}

} // namespace

unsigned long long ElapsedMs() {
    static const ULONGLONG start = GetTickCount64();
    return GetTickCount64() - start;
}

void WriteDiag(const std::string& msg) {
    const std::string line = "[jadyx] [+" + std::to_string(ElapsedMs()) + "ms] " + msg + "\n";
    OutputDebugStringA(line.c_str());
    try {
        std::scoped_lock lock{g_diag_mutex};
        if (g_diag_path.empty()) {
            g_diag_path = LogFilePath("jadyx_diag.txt");
        }
        std::ofstream f(g_diag_path, std::ios::app);
        f << line;
    } catch (...) {
        OutputDebugStringW(L"[jadyx] WriteDiag: could not write diag file\n");
    }
}

void WritePulseLog(const std::string& msg) {
    const std::string line = "[pulse] [+" + std::to_string(ElapsedMs()) + "ms] " + msg + "\n";
    OutputDebugStringA(line.c_str());
    try {
        std::scoped_lock lock{g_pulse_mutex};
        if (g_pulse_path.empty()) {
            g_pulse_path = LogFilePath("jadyx_pulse.txt");
        }
        std::ofstream f(g_pulse_path, std::ios::app);
        f << TakePulseRepeats() << line;
    } catch (...) {
        OutputDebugStringW(L"[jadyx] WritePulseLog: could not write pulse log\n");
    }
}

std::int32_t LoadPrewarmStep(u64 program_id) {
    try {
        const auto values = Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        const winrt::hstring key{PrewarmStepKey(program_id)};
        if (values.HasKey(key)) {
            return winrt::unbox_value<std::int32_t>(values.Lookup(key));
        }
    } catch (...) { /* A missing or invalid value keeps the default budget. */ }
    return 0;
}

void SavePrewarmStep(u64 program_id, std::int32_t step) {
    try {
        Windows::Storage::ApplicationData::Current().LocalSettings().Values().Insert(
            winrt::hstring{PrewarmStepKey(program_id)}, winrt::box_value(step));
    } catch (...) {
        WriteDiag("WARNING: JIT prewarm budget not saved");
    }
}

/// The system's CPU sets as this process sees them: how many logical processors and cores there
/// are, which the system allocated to this app, and which are parked.
std::string CpuSetReport() {
    ULONG length{};
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    std::vector<std::byte> buffer(length);
    auto* const first = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data());
    if (length == 0 || !GetSystemCpuSetInformation(first, length, &length, GetCurrentProcess(), 0)) {
        return "CPU sets unavailable (error " + std::to_string(GetLastError()) + ")";
    }
    unsigned sets{}, allocated{}, to_us{}, parked{}, realtime{};
    std::vector<BYTE> cores;
    std::string list;
    for (ULONG offset = 0; offset < length;) {
        const auto& info = *reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        offset += info.Size;
        if (info.Type != CpuSetInformation) {
            continue;
        }
        const auto& set = info.CpuSet;
        ++sets;
        allocated += set.Allocated;
        to_us += set.AllocatedToTargetProcess;
        parked += set.Parked;
        realtime += set.RealTime;
        if (std::find(cores.begin(), cores.end(), set.CoreIndex) == cores.end()) {
            cores.push_back(set.CoreIndex);
        }
        list += " " + std::to_string(set.Id) + ":c" + std::to_string(set.CoreIndex) + "/lp" +
                std::to_string(set.LogicalProcessorIndex) + (set.Parked ? "P" : "") +
                (set.Allocated ? "A" : "") + (set.AllocatedToTargetProcess ? "T" : "") +
                (set.RealTime ? "R" : "");
    }
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    return std::to_string(system.dwNumberOfProcessors) + " logical processors; " +
           std::to_string(sets) + " CPU sets on " + std::to_string(cores.size()) + " cores, " +
           std::to_string(allocated) + " allocated (" + std::to_string(to_us) + " to this app), " +
           std::to_string(parked) + " parked, " + std::to_string(realtime) +
           " real-time; id:core/logical [P parked, A allocated, T to us, R real-time]:" + list;
}

std::string MemoryReport() {
    try {
        using winrt::Windows::System::MemoryManager;
        // Commit is what runs out: section pages committed but never touched count there and
        // not in AppMemoryUsage (0.2.53 died at 2.8 GiB of usage).
        const auto report = MemoryManager::GetAppMemoryReport();
        const std::string stats = Common::HostMemoryCommitStats();
        return "app memory " + std::to_string(MemoryManager::AppMemoryUsage() >> 20) + " MiB of " +
               std::to_string(MemoryManager::AppMemoryUsageLimit() >> 20) + " MiB limit, commit " +
               std::to_string(report.TotalCommitUsage() >> 20) + " of " +
               std::to_string(report.TotalCommitLimit() >> 20) + " MiB, peak private commit " +
               std::to_string(report.PeakPrivateCommitUsage() >> 20) + " MiB" +
               (g_test_memory_limit.load() == 0 ? "" : ", PC test budget " +
                    std::to_string(g_test_memory_limit.load() >> 20) + " MiB, headroom " +
                    std::to_string((g_test_memory_limit.load() > report.TotalCommitUsage()
                        ? g_test_memory_limit.load() - report.TotalCommitUsage() : 0) >> 20) + " MiB") +
               (stats.empty() ? "" : ", " + stats);
    } catch (...) {
        return "app memory: MemoryManager unavailable";
    }
}

// Free space where Jadyx Portal can put games: LocalState and Dev Mode's storage on D:.
std::string StorageReport() {
    std::string out = "storage:";
    const auto describe = [&out](const char* name, const std::wstring& path) {
        ULARGE_INTEGER available{}, total{};
        if (!GetDiskFreeSpaceExW(path.c_str(), &available, &total, nullptr)) {
            out += std::string(" ") + name + " unavailable (" + std::to_string(GetLastError()) + ");";
            return;
        }
        out += std::string(" ") + name + " " + std::to_string(available.QuadPart >> 20) + " of " +
               std::to_string(total.QuadPart >> 20) + " MiB free;";
    };
    try {
        describe("local", std::wstring{winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()});
    } catch (...) {
        out += " local unavailable;";
    }
    describe("developer", L"D:\\DevelopmentFiles\\");
    return out;
}

// The app's commit split by owner: the emulated DRAM, the JIT's code, the renderer's accounts
// (Common::MemoryLedger) and the rest, which no account covers: driver objects such as compiled
// PSOs, the smaller GPU resources, the heaps and the executable.
std::string MemoryOwners() {
    u64 commit{};
    try {
        commit = winrt::Windows::System::MemoryManager::GetAppMemoryReport().TotalCommitUsage();
    } catch (...) {
        return "MemoryManager unavailable";
    }
    const std::optional<u64> dram = Common::EmulatedDramCommittedBytes();
    const u64 jit = Dynarmic::CommittedCodeBytes();
    const u64 known = dram.value_or(0) + jit + Common::MemoryLedgerCommittedBytes();
    return "emulated DRAM " + (dram ? std::to_string(*dram >> 20) + " MiB" : std::string("n/a")) +
           ", JIT code " + std::to_string(jit >> 20) + " MiB (" +
           std::to_string(Dynarmic::DualMappedCodeCaches()) + " caches dual-mapped), " +
           Common::DescribeMemoryLedger() +
           "; rest " + std::to_string((commit > known ? commit - known : 0) >> 20) +
           " MiB of " + std::to_string(commit >> 20) +
           " MiB commit (driver objects, small GPU resources, heaps, executable); " +
           Common::DescribeCommitCensus(Common::WalkAddressSpaceCommit(), commit);
}

// What the guest kernel has handed out against what the emulated DRAM commits. Pages the game
// freed stay committed on the host until they are allocated again, so the difference is about what
// decommitting the free pages could return (the kernel's own regions, outside the pools, are small).
std::string GuestMemoryReport(Core::System& system) {
    using Pool = Kernel::KMemoryManager::Pool;
    auto& manager = system.Kernel().MemoryManager();
    u64 in_use{};
    std::string pools;
    constexpr std::array<std::pair<Pool, const char*>, 4> POOLS{{
        {Pool::Application, "application"},
        {Pool::Applet, "applet"},
        {Pool::System, "system"},
        {Pool::SystemNonSecure, "system non-secure"},
    }};
    for (const auto& [pool, name] : POOLS) {
        const u64 size = manager.GetSize(pool);
        const u64 free = manager.GetFreeSize(pool);
        const u64 used = size > free ? size - free : 0;
        in_use += used;
        pools += std::string(pools.empty() ? "" : ", ") + name + " " + std::to_string(used >> 20) +
                 " of " + std::to_string(size >> 20) + " MiB";
    }
    const std::optional<u64> committed = Common::EmulatedDramCommittedBytes();
    if (!committed) {
        return "in use " + std::to_string(in_use >> 20) + " MiB (" + pools + ")";
    }
    return "in use " + std::to_string(in_use >> 20) + " MiB, DRAM committed " +
           std::to_string(*committed >> 20) + " MiB, freed but committed about " +
           std::to_string((*committed > in_use ? *committed - in_use : 0) >> 20) + " MiB (" +
           pools + ")";
}

u64 TrimFreeGuestMemory(Core::System& system, u64 budget) {
    using Pool = Kernel::KMemoryManager::Pool;
    constexpr u64 STEP = 64ULL << 20;
    auto& manager = system.Kernel().MemoryManager();
    u64 total = 0;
    for (const Pool pool : {Pool::Application, Pool::Applet, Pool::System, Pool::SystemNonSecure}) {
        for (;;) {
            const u64 step = (std::min)(STEP, budget > total ? budget - total : 0);
            if (step == 0) {
                return total;
            }
            const u64 released = manager.TrimFree(pool, step);
            total += released;
            if (released < step) {
                break;
            }
        }
    }
    return total;
}

// The PC has no app memory limit of its own: memory_limit_mib (boot.cfg) puts the process in a
// job that refuses commit beyond it, as the Series does, however the app was launched. Applied
// once per process, since a job's limit cannot be lifted; where the platform's limit is already
// at or below it (the console), nothing changes.
void ApplyProcessMemoryLimit(u32 limit_mib) {
    static u32 applied_mib = 0;
    if (limit_mib == 0) {
        return;
    }
    if (applied_mib != 0) {
        if (applied_mib != limit_mib) {
            WriteDiag("memory limit: kept " + std::to_string(applied_mib) + " MiB; " +
                      std::to_string(limit_mib) + " MiB needs a new process");
        }
        return;
    }
    const u64 bytes = u64{limit_mib} << 20;
    try {
        const u64 platform = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
        if (platform <= bytes) {
            WriteDiag("memory limit: the platform's " + std::to_string(platform >> 20) +
                      " MiB already applies");
            applied_mib = limit_mib;
            return;
        }
    } catch (...) {
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(bytes);
    const bool ok = job != nullptr &&
                    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                            sizeof(limits)) &&
                    AssignProcessToJobObject(job, GetCurrentProcess());
    const DWORD error = ok ? 0 : GetLastError();
    if (job != nullptr) {
        CloseHandle(job); // The process keeps the job alive.
    }
    if (!ok) {
        WriteDiag("memory limit: FAILED to cap the process at " + std::to_string(limit_mib) +
                  " MiB (error " + std::to_string(error) + ")");
        return;
    }
    applied_mib = limit_mib;
    WriteDiag("memory limit: process commit capped at " + std::to_string(limit_mib) + " MiB");
}

// Pulse store gate: ends this process where it stands, as a crash or a power loss would (no
// unwinding, no flush of anything still in the process), and asks the system to start the app
// again. Without a restart (refused, or not supported here) the next manual launch resumes.
[[noreturn]] void RestartApp() {
    try {
        const auto reason = CoreApplication::RequestRestartAsync(L"").get();
        if (reason != AppRestartFailureReason::RestartPending) {
            WriteDiag("restart refused (" + std::to_string(static_cast<int>(reason)) +
                      "): launch the app again to continue");
        }
    } catch (...) {
        WriteDiag("restart unavailable: launch the app again to continue");
    }
    TerminateProcess(GetCurrentProcess(), 73);
    for (;;) Sleep(INFINITE);
}

// Where the app's memory is: committed bytes by kind over the whole address space, then the
// largest allocations (base, committed MiB, kind), whose sizes tell their owners apart.
std::string LargestAllocations() {
    struct Allocation {
        uintptr_t base;
        u64 committed;
        DWORD type;
        DWORD protect; // As allocated: write-combined pages are GPU upload memory, executable JIT.
    };
    std::vector<Allocation> allocations;
    u64 by_type[3]{}; // private, mapped, image
    uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION info{};
    while (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) != 0) {
        const auto base = reinterpret_cast<uintptr_t>(info.AllocationBase);
        if (info.State == MEM_COMMIT) {
            by_type[info.Type == MEM_PRIVATE ? 0 : info.Type == MEM_MAPPED ? 1 : 2] +=
                info.RegionSize;
            if (allocations.empty() || allocations.back().base != base) {
                allocations.push_back({base, 0, info.Type, info.AllocationProtect});
            }
            allocations.back().committed += info.RegionSize;
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= address) {
            break;
        }
        address = next;
    }
    std::sort(allocations.begin(), allocations.end(),
              [](const Allocation& a, const Allocation& b) { return a.committed > b.committed; });
    // Committed private memory by allocation size: many equal pieces are one owner's pool.
    constexpr u64 BUCKETS[] = {1ULL << 20, 8ULL << 20, 24ULL << 20, 40ULL << 20, 1ULL << 62};
    u64 bucket_bytes[std::size(BUCKETS)]{};
    u32 bucket_count[std::size(BUCKETS)]{};
    for (const Allocation& allocation : allocations) {
        if (allocation.type != MEM_PRIVATE) {
            continue;
        }
        size_t b = 0;
        while (allocation.committed >= BUCKETS[b]) {
            ++b;
        }
        bucket_bytes[b] += allocation.committed;
        ++bucket_count[b];
    }
    std::string text = "private " + std::to_string(by_type[0] >> 20) + " MiB, mapped " +
                       std::to_string(by_type[1] >> 20) + " MiB, image " +
                       std::to_string(by_type[2] >> 20) + " MiB; private by size: <1M " +
                       std::to_string(bucket_count[0]) + "x=" +
                       std::to_string(bucket_bytes[0] >> 20) + "M, 1-8M " +
                       std::to_string(bucket_count[1]) + "x=" +
                       std::to_string(bucket_bytes[1] >> 20) + "M, 8-24M " +
                       std::to_string(bucket_count[2]) + "x=" +
                       std::to_string(bucket_bytes[2] >> 20) + "M, 24-40M " +
                       std::to_string(bucket_count[3]) + "x=" +
                       std::to_string(bucket_bytes[3] >> 20) + "M, >=40M " +
                       std::to_string(bucket_count[4]) + "x=" +
                       std::to_string(bucket_bytes[4] >> 20) + "M; largest:";
    for (size_t i = 0; i < allocations.size() && i < 12; ++i) {
        const DWORD protect = allocations[i].protect;
        char entry[64];
        std::snprintf(entry, sizeof(entry), " %llx=%lluM%s%s",
                      static_cast<unsigned long long>(allocations[i].base),
                      static_cast<unsigned long long>(allocations[i].committed >> 20),
                      allocations[i].type == MEM_PRIVATE  ? ""
                      : allocations[i].type == MEM_MAPPED ? "(map)"
                                                          : "(img)",
                      (protect & PAGE_WRITECOMBINE) ? "(wc)"
                      : (protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))
                          ? "(x)"
                          : "");
        text += entry;
    }
    // Write-combined and executable private memory in total: GPU upload heaps and JIT code.
    u64 write_combined = 0;
    u64 executable = 0;
    for (const Allocation& allocation : allocations) {
        if (allocation.type != MEM_PRIVATE) continue;
        if (allocation.protect & PAGE_WRITECOMBINE) write_combined += allocation.committed;
        else if (allocation.protect &
                 (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))
            executable += allocation.committed;
    }
    text += "; write-combined " + std::to_string(write_combined >> 20) + " MiB, executable " +
            std::to_string(executable >> 20) + " MiB";
    return text;
}

// The process heaps: allocations still live (busy) against what they keep committed, after
// returning their free pages to the system. Live bytes left after a game are leaked objects; a
// committed figure far above them is fragmentation.
std::string HeapReport() {
    std::vector<HANDLE> heaps(64);
    const DWORD count = GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data());
    heaps.resize(std::min<DWORD>(count, static_cast<DWORD>(heaps.size())));
    u64 busy = 0;
    u64 committed = 0;
    // Live bytes by block size: many blocks of one size are one kind of object.
    std::unordered_map<u64, std::pair<u64, u64>> by_size; // size -> count, bytes
    for (const HANDLE heap : heaps) {
        HeapCompact(heap, 0);
        if (!HeapLock(heap)) {
            continue;
        }
        PROCESS_HEAP_ENTRY entry{};
        while (HeapWalk(heap, &entry)) {
            if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                busy += entry.cbData;
                auto& [blocks, bytes] = by_size[entry.cbData];
                ++blocks;
                bytes += entry.cbData;
            } else if (entry.wFlags & PROCESS_HEAP_REGION) {
                committed += entry.Region.dwCommittedSize;
            }
        }
        HeapUnlock(heap);
    }
    std::vector<std::pair<u64, std::pair<u64, u64>>> sizes(by_size.begin(), by_size.end());
    std::sort(sizes.begin(), sizes.end(),
              [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
    std::string text = std::to_string(heaps.size()) + " heaps, live " +
                       std::to_string(busy >> 20) + " MiB, region commit " +
                       std::to_string(committed >> 20) + " MiB; live by block size:";
    for (size_t i = 0; i < sizes.size() && i < 10; ++i) {
        text += " " + std::to_string(sizes[i].first) + "B x" +
                std::to_string(sizes[i].second.first) + "=" +
                std::to_string(sizes[i].second.second >> 10) + "K";
    }
    return text;
}

// The title's memory budget as the OS enforces it: Microsoft documents Game-mode UWP titles as capped
// (5 GB on the Xbox resource page), and exceeding it makes allocations fail rather than paging.
bool QueryAppMemory(u64& used, u64& limit) {
    try {
        using winrt::Windows::System::MemoryManager;
        used = MemoryManager::AppMemoryUsage();
        limit = MemoryManager::AppMemoryUsageLimit();
        const u64 test_limit = g_test_memory_limit.load(std::memory_order_relaxed);
        if (test_limit != 0) {
            limit = (std::min)(limit, test_limit);
            used = (std::max)(used, MemoryManager::GetAppMemoryReport().TotalCommitUsage());
        }
        return true;
    } catch (...) {
        return false;
    }
}

// The app's limit, the system's commit and the GPU budget the renderer last read.
Common::MemoryCeilings QueryMemoryCeilings() {
    using Common::MemoryCeiling;
    Common::MemoryCeilings ceilings;
    if (auto& app = ceilings[MemoryCeiling::App]; !QueryAppMemory(app.used, app.limit)) {
        app = {};
    }
    MEMORYSTATUSEX status{.dwLength = sizeof(MEMORYSTATUSEX)};
    if (GlobalMemoryStatusEx(&status)) {
        ceilings[MemoryCeiling::System] = {status.ullTotalPageFile - status.ullAvailPageFile,
                                           status.ullTotalPageFile};
    }
    if (auto& gpu = ceilings[MemoryCeiling::Gpu]; !D3D12::LastGpuMemory(gpu.used, gpu.limit)) {
        gpu = {};
    }
    return ceilings;
}

// Last-chance writer for the crash handlers: no WinRT, no lock (the faulting thread may hold it),
// no allocation beyond what fopen needs.
void WriteDiagRaw(const char* line, bool debugger_channel) {
    if (debugger_channel) {
        OutputDebugStringA(line); // raises DBG_PRINTEXCEPTION_C internally: never from the VEH
    }
    if (g_diag_path.empty()) {
        return;
    }
    // Shared, as WriteDiag's stream: fopen_s opens without sharing, so it failed and lost the
    // crash report whenever another process had the diag open (a tail -F watching it).
    if (std::FILE* const f = _fsopen(g_diag_path.c_str(), "a", _SH_DENYNO)) {
        std::fputs(line, f);
        std::fclose(f);
    }
}

void WritePulseRejection(const char* what) {
    std::scoped_lock lock{g_pulse_mutex};
    if (g_pulse_path.empty()) return;
    if (g_pulse_last_rejection == what) {
        ++g_pulse_rejection_repeats;
        return;
    }
    const std::string repeats = TakePulseRepeats();
    g_pulse_last_rejection = what;
    char line[512];
    std::snprintf(line, sizeof(line), "[pulse] [+%llums] rejected: %s | thread %lu\n",
                  ElapsedMs(), what, GetCurrentThreadId());
    if (std::FILE* const f = _fsopen(g_pulse_path.c_str(), "a", _SH_DENYNO)) {
        std::fputs(repeats.c_str(), f);
        std::fputs(line, f);
        std::fclose(f);
    }
}

} // namespace Jadyx
