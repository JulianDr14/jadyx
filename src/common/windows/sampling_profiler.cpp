// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/windows/sampling_profiler.h"
#include "common/windows/sampling_capture_backoff.h"

namespace Common::SamplingProfiler {
namespace {

constexpr size_t MAX_FRAMES = 96;
/// How late a frame has to be for every thread to be sampled, and every how many ticks.
constexpr u64 STALL_NS = 120'000'000;
constexpr u32 STALL_STRIDE = 8;
/// Elapsed context capture, including time waiting for suspension to take effect.
constexpr u64 SLOW_CAPTURE_NS = CaptureBackoff::SLOW_NS;

std::atomic<u64> g_last_frame_ns{};

u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

/// Most stacks are far smaller; a deeper one is unwound as far as the copy goes.
constexpr size_t MAX_STACK_COPY = 256 * 1024;
/// Committed space after the copy, so an unwind that reads past a truncated one reads stale
/// bytes instead of faulting.
constexpr size_t COPY_SLACK = 64 * 1024;
/// Distinct stacks kept per window; past it, samples only count towards their thread.
constexpr size_t MAX_STACKS = 100'000;
/// A thread that did not run since the last tick is sampled once in this many ticks, and the
/// sample weighs as many: where threads wait matters, but they wait in few places, and most
/// threads of the process wait most of the time.
constexpr u32 WAITING_STRIDE = 32;
/// Cycles a thread must have used since the previous tick to count as running: being suspended
/// costs the thread a few thousand cycles of its own (the suspend APC runs on it).
constexpr ULONG64 RUNNING_CYCLES = 50'000;

struct ThreadEntry {
    std::string name;
    HANDLE handle{};
    DWORD id{};
    ULONG_PTR stack_low{};
    ULONG_PTR stack_high{};

    // Owned by the sampler thread.
    ULONG64 last_cycles{};
    u64 window_cpu_start{}; ///< Kernel plus user time at the window's start, in 100 ns.
    u64 running_samples{};
    u64 waiting_samples{};
    u32 waiting_ticks{};
    CaptureBackoff capture_backoff;
    u64 capture_skips{};
    u64 slow_captures{};
    std::atomic<bool> dead{};
};

std::mutex registry_mutex;
std::vector<std::unique_ptr<ThreadEntry>> registry;
u64 registry_generation{};

/// The first session of the process starts the file; later ones (the next game from the library)
/// append to it. Windows are numbered across sessions.
bool output_started{};
u64 window_counter{};

/// Blocks with their bytes in the file: enough to read what the hottest code does.
constexpr size_t GUEST_CODE_BLOCKS = 64;
constexpr size_t GUEST_CODE_BYTES = 512;

struct JitBlock {
    u64 host;
    u64 host_size;
    u64 guest_pc;
    u64 guest_end;
};
/// In compile order: a later block at the same host address replaced the earlier one.
std::mutex jit_blocks_mutex;
std::vector<JitBlock> jit_blocks;
std::vector<std::pair<u64, u64>> jit_thunks; // start, end; one per JIT

/// The first JIT's thunks, to name the instruction of a sample: every JIT lays them out alike.
std::vector<u8> jit_thunk_code;
/// And their layout, by offset.
std::vector<JitThunk> jit_thunk_layout;

/// The thunk that holds `offset` into the thunks.
bool FindJitThunk(u64 offset, JitThunk& thunk) {
    std::scoped_lock lock{jit_blocks_mutex};
    const auto next = std::ranges::upper_bound(jit_thunk_layout, offset, {}, &JitThunk::offset);
    if (next == jit_thunk_layout.begin()) {
        return false;
    }
    thunk = *std::prev(next);
    return true;
}

/// offset: how far into its JIT's thunks pc is.
bool InJitThunks(u64 pc, u64* offset = nullptr) {
    std::scoped_lock lock{jit_blocks_mutex};
    for (const std::pair<u64, u64>& range : jit_thunks) {
        if (pc >= range.first && pc < range.second) {
            if (offset) {
                *offset = pc - range.first;
            }
            return true;
        }
    }
    return false;
}

std::mutex guest_reader_mutex;
GuestCodeReader guest_reader;
u64 guest_code_base{};
GuestBlockClassifier guest_classifier;

constexpr u64 GUEST_PAGE_BITS = 12;
constexpr u64 GUEST_BLOCK_PAGE_BITS = 9; // 2 MiB blocks

u64 CpuTime100ns(HANDLE thread) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(thread, &creation, &exit, &kernel, &user)) {
        return 0;
    }
    const auto value = [](const FILETIME& time) {
        return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return value(kernel) + value(user);
}

std::string SanitizeName(std::string_view name) {
    std::string result{name.empty() ? std::string_view{"unnamed"} : name};
    for (char& c : result) {
        if (c == ' ' || c == ';' || c == '\t' || c == '\n' || c == '\r') {
            c = '_';
        }
    }
    return result;
}

bool Exited(HANDLE thread) {
    DWORD exit_code{};
    return GetExitCodeThread(thread, &exit_code) && exit_code != STILL_ACTIVE;
}

/// RtlVirtualUnwind on the stack copy. A frame whose unwind data reads outside the copy faults
/// here instead of taking the process down; the unwind then stops at that frame.
bool VirtualUnwindGuarded(DWORD64 image_base, DWORD64 pc, PRUNTIME_FUNCTION function,
                          CONTEXT* context) {
    __try {
        PVOID handler_data{};
        DWORD64 establisher_frame{};
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, function, context, &handler_data,
                         &establisher_frame, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/// Suspends the thread only to read its registers and copy its stack into `copy`
/// (MAX_STACK_COPY + COPY_SLACK bytes, or null for registers only): no allocation, lock or
/// logging may happen in between, since the thread may hold the heap lock or any other. A thread
/// that cannot be suspended is marked dead.
bool CaptureThread(ThreadEntry& entry, u8* copy, CONTEXT& context, size_t& copied,
                   ULONG_PTR& origin) {
    copied = 0;
    if (SuspendThread(entry.handle) == static_cast<DWORD>(-1)) {
        entry.dead = true;
        return false;
    }
    // GetThreadContext also waits for the suspension to take effect.
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    const bool ok = GetThreadContext(entry.handle, &context) != 0;
    if (ok && copy) {
        const ULONG_PTR sp = static_cast<ULONG_PTR>(context.Rsp);
        ULONG_PTR top{};
        // A thread caught probing its stack has its pointer at or below the guard page, and
        // reading that page raises STATUS_GUARD_PAGE_VIOLATION here. Only copy from a plainly
        // committed region. A fiber's stack (the emulated cores) ends with its region too.
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(sp), &info, sizeof(info)) &&
            info.State == MEM_COMMIT && (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0) {
            top = reinterpret_cast<ULONG_PTR>(info.BaseAddress) + info.RegionSize;
            if (sp >= entry.stack_low && sp < entry.stack_high) {
                top = std::min(top, entry.stack_high);
            }
        }
        if (top > sp) {
            copied = std::min<size_t>(top - sp, MAX_STACK_COPY);
            std::memcpy(copy, reinterpret_cast<const void*>(sp), copied);
            origin = sp;
        }
    }
    ResumeThread(entry.handle);
    return ok;
}

/// Runs after the thread was resumed, on the copy only.
size_t UnwindCopy(CONTEXT context, const u8* copy, size_t copied, ULONG_PTR origin,
                  DWORD64* frames) {
    if (copied == 0) {
        frames[0] = context.Rip;
        return 1;
    }
    const ULONG_PTR copy_base = reinterpret_cast<ULONG_PTR>(copy);
    const ULONG_PTR copy_end = copy_base + copied;
    // Registers that point into the stack are moved to the copy before every step: the stack
    // pointer, and a frame pointer restored from the stack by the previous step. Only those
    // steer the unwind, so the copy itself is never rewritten.
    const auto relocate_registers = [&] {
        for (DWORD64* reg : {&context.Rsp, &context.Rbp, &context.Rbx, &context.Rsi, &context.Rdi,
                             &context.R12, &context.R13, &context.R14, &context.R15}) {
            if (*reg >= origin && *reg < origin + copied) {
                *reg = *reg - origin + copy_base;
            }
        }
    };
    size_t depth = 0;
    while (depth < MAX_FRAMES) {
        relocate_registers();
        const DWORD64 pc = context.Rip;
        if (pc == 0) {
            break;
        }
        frames[depth++] = pc;
        const DWORD64 sp = context.Rsp;
        if (sp < copy_base || sp + sizeof(DWORD64) > copy_end) {
            break;
        }
        DWORD64 image_base{};
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &image_base, nullptr);
        if (function == nullptr) {
            // A leaf function without unwind data: the return address is on top.
            std::memcpy(&context.Rip, reinterpret_cast<const void*>(sp), sizeof(DWORD64));
            context.Rsp = sp + sizeof(DWORD64);
        } else if (!VirtualUnwindGuarded(image_base, pc, function, &context)) {
            break;
        }
        if (context.Rsp <= sp) {
            break;
        }
    }
    return depth;
}

class Sampler {
public:
    explicit Sampler(const Options& options_) : options{options_} {
        options.hz = std::clamp<u32>(options.hz, 10, 1000);
        options.window_seconds = std::max<u32>(options.window_seconds, 1);
        // Reserved once: the copy is written while the target is suspended.
        stack_copy = static_cast<u8*>(VirtualAlloc(nullptr, MAX_STACK_COPY + COPY_SLACK,
                                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (std::FILE* file = OpenOutput(output_started ? L"a" : L"w")) {
            output_started = true;
            std::fputs("# jadyx sampling profile v1\n", file);
            WriteModuleIdentity(file);
            std::fclose(file);
        }
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // Sleeps and condition variables wake on the system timer, every 15.6 ms unless its
        // resolution was raised; a high-resolution waitable timer keeps the rate either way.
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        if (timer == nullptr) {
            timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        thread = std::thread([this] { Run(); });
    }

    ~Sampler() {
        SetEvent(stop_event);
        thread.join();
        if (timer) {
            CloseHandle(timer);
        }
        CloseHandle(stop_event);
        if (stack_copy) {
            VirtualFree(stack_copy, 0, MEM_RELEASE);
        }
    }

private:
    struct StackRecord {
        u32 thread;
        bool waiting;
        u32 depth;
        size_t first_frame;
        u64 count;
        bool stalled;
    };

    void Run() {
        if (auto* set_description = reinterpret_cast<decltype(&SetThreadDescription)>(
                reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"KernelBase.dll"),
                                                       "SetThreadDescription")))) {
            set_description(GetCurrentThread(), L"SamplingProfiler");
        }
        // It runs for microseconds per tick: above the emulator's threads, so the rate holds.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const auto interval = std::chrono::nanoseconds(1'000'000'000LL / options.hz);
        auto next = std::chrono::steady_clock::now();
        StartWindow();
        for (;;) {
            next += interval;
            const auto now = std::chrono::steady_clock::now();
            if (now - next > 10 * interval) {
                next = now; // Fell behind (the process was suspended): do not catch up.
            }
            if (!WaitUntil(next)) {
                break;
            }
            Tick();
            if (std::chrono::steady_clock::now() - window_start >=
                std::chrono::seconds(options.window_seconds)) {
                WriteWindow();
                StartWindow();
            }
        }
        WriteWindow();
    }

    /// False once Stop was called.
    bool WaitUntil(std::chrono::steady_clock::time_point deadline) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= remaining.zero()) {
            return WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0;
        }
        LARGE_INTEGER due{}; // Relative, in 100 ns units.
        due.QuadPart = -std::max<LONGLONG>(
            1, std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
        if (timer == nullptr || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            return WaitForSingleObject(stop_event, 1) != WAIT_OBJECT_0;
        }
        const HANDLE handles[] = {stop_event, timer};
        return WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0;
    }

    std::FILE* OpenOutput(const wchar_t* mode) const {
        std::FILE* file{};
        return _wfopen_s(&file, options.output.c_str(), mode) == 0 ? file : nullptr;
    }

    void RefreshThreads() {
        std::scoped_lock lock{registry_mutex};
        if (registry_generation == seen_generation) {
            return;
        }
        seen_generation = registry_generation;
        for (size_t index = threads.size(); index < registry.size(); ++index) {
            ThreadEntry& entry = *registry[index];
            entry.capture_backoff = {};
            entry.window_cpu_start = CpuTime100ns(entry.handle);
            QueryThreadCycleTime(entry.handle, &entry.last_cycles);
        }
        threads.clear();
        for (const auto& entry : registry) {
            threads.push_back(entry.get());
        }
    }

    void StartWindow() {
        RefreshThreads();
        window_start = std::chrono::steady_clock::now();
        window_ticks = 0;
        sampler_cpu_start = CpuTime100ns(GetCurrentThread());
        suspended_ns = 0;
        suspensions = 0;
        max_suspended_ns = 0;
        dropped_samples = 0;
        stacks.clear();
        stack_index.clear();
        frame_pool.clear();
        thunk_samples.clear();
        thunk_rips.clear();
        thunk_pages.clear();
        for (ThreadEntry* entry : threads) {
            entry->window_cpu_start = CpuTime100ns(entry->handle);
            entry->running_samples = 0;
            entry->waiting_samples = 0;
            entry->capture_skips = 0;
            entry->slow_captures = 0;
        }
    }

    void Tick() {
        RefreshThreads();
        ++window_ticks;
        // A frame late by far: every thread, blocked or not, so the stacks show what holds it up.
        const u64 frame = g_last_frame_ns.load(std::memory_order_relaxed);
        if (frame != 0 && NowNs() - frame > STALL_NS) {
            if (window_ticks % STALL_STRIDE == 0) {
                for (u32 index = 0; index < threads.size(); ++index) {
                    if (!threads[index]->dead) {
                        Sample(index, *threads[index], false, 1, true);
                    }
                }
            }
            return;
        }
        for (u32 index = 0; index < threads.size(); ++index) {
            ThreadEntry& entry = *threads[index];
            if (entry.dead) {
                continue;
            }
            ULONG64 cycles{};
            if (!QueryThreadCycleTime(entry.handle, &cycles)) {
                entry.dead = true;
                continue;
            }
            const bool ran = cycles - entry.last_cycles >= RUNNING_CYCLES;
            entry.last_cycles = cycles;
            u32 weight = 1;
            if (!ran) {
                if (++entry.waiting_ticks < WAITING_STRIDE) {
                    continue;
                }
                weight = WAITING_STRIDE;
            }
            entry.waiting_ticks = 0;
            Sample(index, entry, !ran, weight, false);
            // Without the suspension's own cycles, or a blocked thread would look running.
            QueryThreadCycleTime(entry.handle, &entry.last_cycles);
        }
    }

    bool Capture(ThreadEntry& entry, CONTEXT& context, size_t& copied, ULONG_PTR& origin) {
        LARGE_INTEGER start{}, end{};
        QueryPerformanceCounter(&start);
        const bool ok = CaptureThread(entry, stack_copy, context, copied, origin);
        if (entry.dead) {
            return false;
        }
        QueryPerformanceCounter(&end);
        const u64 elapsed = static_cast<u64>(end.QuadPart - start.QuadPart) * 1'000'000'000ULL /
                            static_cast<u64>(qpc_frequency.QuadPart);
        suspended_ns += elapsed;
        max_suspended_ns = std::max(max_suspended_ns, elapsed);
        ++suspensions;
        return ok;
    }

    size_t Unwind(CONTEXT context, size_t copied, ULONG_PTR origin, DWORD64* frames) {
        return UnwindCopy(context, stack_copy, copied, origin, frames);
    }

    void Sample(u32 thread_index, ThreadEntry& entry, bool waiting, u32 weight, bool stalled) {
        CONTEXT context{};
        size_t copied{};
        ULONG_PTR origin{};
        const u64 capture_start = NowNs();
        if (!entry.capture_backoff.Ready(capture_start)) {
            ++entry.capture_skips;
            return;
        }
        const bool captured = Capture(entry, context, copied, origin);
        // All policy, counters and logging run after Capture has resumed the target.
        const u64 capture_end = NowNs();
        const u64 took = capture_end - capture_start;
        entry.capture_backoff.Observe(capture_end, took);
        if (took > SLOW_CAPTURE_NS) {
            ++entry.slow_captures;
            LOG_WARNING(Common, "profiler: context capture for {} took {} ms; sampling backs off",
                        entry.name, took / 1'000'000);
        }
        if (!captured) {
            return;
        }
        if (!stalled) {
            (waiting ? entry.waiting_samples : entry.running_samples) += weight;
        }
        // A thunk keeps the return address on top of the stack, or under the register it saved.
        // The thread runs again by now, but these are the copy's words.
        u64 thunk_offset{};
        if (!waiting && !stalled && copied >= 2 * sizeof(u64) &&
            InJitThunks(context.Rip, &thunk_offset)) {
            u64 top[2];
            std::memcpy(top, stack_copy, sizeof(top));
            thunk_samples[{thread_index, top[0], top[1]}] += weight;
            thunk_rips[thunk_offset] += weight;
            // The guest address is still in its register anywhere in the thunk.
            JitThunk thunk{};
            if (FindJitThunk(thunk_offset, thunk)) {
                const DWORD64 registers[16] = {
                    context.Rax, context.Rcx, context.Rdx, context.Rbx, context.Rsp, context.Rbp,
                    context.Rsi, context.Rdi, context.R8,  context.R9,  context.R10, context.R11,
                    context.R12, context.R13, context.R14, context.R15};
                const u64 page = registers[thunk.vaddr_reg & 15] >> GUEST_PAGE_BITS;
                auto& counts = thunk_pages[(page << 1) | (thunk.write ? 1 : 0)];
                counts[0] += weight;
                // Stopped on the instruction after the entry load: it was waiting for the entry.
                if (thunk_offset == thunk.after_entry) {
                    counts[1] += weight;
                }
            }
        }
        DWORD64 frames[MAX_FRAMES];
        const size_t depth = Unwind(context, copied, origin, frames);
        u64 hash = 0xcbf29ce484222325ULL ^ (static_cast<u64>(thread_index) << 1) ^
                   static_cast<u64>(waiting ? 1 : 0) ^ (stalled ? 0x5555ULL << 16 : 0);
        for (size_t i = 0; i < depth; ++i) {
            hash = (hash ^ frames[i]) * 0x100000001b3ULL;
        }
        for (u64 probe = hash;; ++probe) {
            const auto it = stack_index.find(probe);
            if (it == stack_index.end()) {
                if (stacks.size() >= MAX_STACKS) {
                    dropped_samples += weight;
                    return;
                }
                stack_index.emplace(probe, stacks.size());
                stacks.push_back({thread_index, waiting, static_cast<u32>(depth), frame_pool.size(),
                                  weight, stalled});
                frame_pool.insert(frame_pool.end(), frames, frames + depth);
                return;
            }
            StackRecord& record = stacks[it->second];
            if (record.thread == thread_index && record.waiting == waiting &&
                record.stalled == stalled &&
                record.depth == depth &&
                std::equal(frames, frames + depth, frame_pool.begin() + record.first_frame)) {
                record.count += weight;
                return;
            }
        }
    }

    /// The module of an address, or [jit] for code outside every module (Dynarmic's code cache).
    const std::string& ModuleName(DWORD64 pc, DWORD64& base) {
        PVOID image_base{};
        RtlPcToFileHeader(reinterpret_cast<PVOID>(pc), &image_base);
        base = reinterpret_cast<DWORD64>(image_base);
        auto [it, inserted] = module_names.try_emplace(base);
        if (inserted) {
            if (image_base == nullptr) {
                it->second = "[jit]";
            } else {
                wchar_t path[MAX_PATH]{};
                const DWORD length =
                    GetModuleFileNameW(static_cast<HMODULE>(image_base), path, MAX_PATH);
                std::wstring_view view{path, length};
                if (const size_t slash = view.find_last_of(L"\\/"); slash != view.npos) {
                    view.remove_prefix(slash + 1);
                }
                for (const wchar_t c : view) {
                    it->second.push_back(c < 0x80 && c != L' ' ? static_cast<char>(c) : '_');
                }
                if (it->second.empty()) {
                    it->second = fmt::format("module_{:x}", base);
                }
            }
        }
        return it->second;
    }

    /// The executable's link time stamp and size, so the PC can tell a PDB of another build.
    static void WriteModuleIdentity(std::FILE* file) {
        const auto* base = reinterpret_cast<const u8*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        std::fputs(fmt::format("executable timestamp 0x{:08x} size 0x{:x}\n",
                               nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage)
                       .c_str(),
                   file);
    }

    void WriteWindow() {
        std::FILE* file = OpenOutput(L"a");
        if (!file) {
            return;
        }
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - window_start).count();
        const u64 sampler_cpu = CpuTime100ns(GetCurrentThread()) - sampler_cpu_start;
        std::fputs(
            fmt::format("window {} seconds {:.3f} hz {} ticks {} sampler_cpu_ms {:.1f} "
                        "suspensions {} suspended_us_avg {:.1f} suspended_us_max {:.1f} "
                        "stacks {} dropped_samples {}\n",
                        ++window_counter, seconds, options.hz, window_ticks,
                        static_cast<double>(sampler_cpu) / 1e4, suspensions,
                        suspensions ? static_cast<double>(suspended_ns) / 1e3 / suspensions : 0.0,
                        static_cast<double>(max_suspended_ns) / 1e3, stacks.size(),
                        dropped_samples)
                .c_str(),
            file);
        u64 skipped = 0, slow = 0;
        for (const ThreadEntry* entry : threads) {
            skipped += entry->capture_skips;
            slow += entry->slow_captures;
        }
        std::fputs(fmt::format("capture_coverage cooldown_skips {} slow_captures {}\n",
                               skipped, slow).c_str(), file);
        // Names can change while a thread lives (RegisterCurrentThread again).
        std::vector<std::string> names;
        {
            std::scoped_lock lock{registry_mutex};
            for (ThreadEntry* entry : threads) {
                names.push_back(entry->name);
            }
        }
        for (u32 index = 0; index < threads.size(); ++index) {
            ThreadEntry* const entry = threads[index];
            const u64 cpu = CpuTime100ns(entry->handle);
            const u64 used = cpu >= entry->window_cpu_start ? cpu - entry->window_cpu_start : 0;
            if (Exited(entry->handle)) {
                entry->dead = true;
            }
            if (used == 0 && entry->running_samples == 0 && entry->waiting_samples == 0 &&
                entry->capture_skips == 0 && entry->slow_captures == 0) {
                continue;
            }
            std::fputs(fmt::format("thread {} id {} cpu_ms {:.1f} running {} waiting {}{} "
                                   "capture_skips {} slow_captures {}\n",
                                   names[index], entry->id, static_cast<double>(used) / 1e4,
                                   entry->running_samples, entry->waiting_samples,
                                   entry->dead ? " exited" : "",
                                   entry->capture_skips, entry->slow_captures)
                           .c_str(),
                       file);
        }
        std::vector<size_t> order(stacks.size());
        for (size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::ranges::sort(order, [this](size_t a, size_t b) {
            return stacks[a].count > stacks[b].count;
        });
        std::string line;
        for (const size_t index : order) {
            const StackRecord& record = stacks[index];
            line.clear();
            fmt::format_to(std::back_inserter(line), "stack {} {} {} ", names[record.thread],
                           record.stalled ? 'S' : record.waiting ? 'W' : 'R', record.count);
            // Root first, as flame graphs fold them.
            for (size_t i = record.depth; i-- > 0;) {
                const DWORD64 pc = frame_pool[record.first_frame + i];
                DWORD64 base{};
                const std::string& module = ModuleName(pc, base);
                if (base == 0) {
                    line += module;
                } else {
                    fmt::format_to(std::back_inserter(line), "{}+0x{:x}", module, pc - base);
                }
                if (i != 0) {
                    line += ';';
                }
            }
            line += '\n';
            std::fputs(line.c_str(), file);
        }
        WriteGuestCode(file, names);
        std::fputs("end\n", file);
        std::fclose(file);
    }

    /// How the thunk samples spread over the guest's 2 MiB blocks, by what the page table holds
    /// for the block now: "thunkblock load|store class samples waiting-for-entry pages blocks".
    void WriteThunkBlocks(std::FILE* file) {
        std::scoped_lock lock{guest_reader_mutex};
        if (!guest_classifier || thunk_pages.empty()) {
            return;
        }
        struct Row {
            u64 samples{};
            u64 at_entry{};
            u64 pages{};
            std::unordered_set<u64> blocks;
        };
        static constexpr std::array<const char*, static_cast<size_t>(GuestBlockClass::Count)>
            class_names{"slow", "uniform", "holes", "deltas", "outside"};
        std::array<std::array<Row, class_names.size()>, 2> rows;
        for (const auto& [key, counts] : thunk_pages) {
            const bool write = (key & 1) != 0;
            const u64 page = key >> 1;
            const auto block_class = guest_classifier(page << GUEST_PAGE_BITS, write);
            Row& row = rows[write ? 1 : 0][std::min(static_cast<size_t>(block_class),
                                                    class_names.size() - 1)];
            row.samples += counts[0];
            row.at_entry += counts[1];
            ++row.pages;
            row.blocks.insert(page >> GUEST_BLOCK_PAGE_BITS);
        }
        for (size_t write = 0; write < rows.size(); ++write) {
            for (size_t index = 0; index < class_names.size(); ++index) {
                const Row& row = rows[write][index];
                if (row.samples == 0) {
                    continue;
                }
                std::fputs(fmt::format("thunkblock {} {} {} {} {} {}\n", write ? "store" : "load",
                                       class_names[index], row.samples, row.at_entry, row.pages,
                                       row.blocks.size())
                               .c_str(),
                           file);
            }
        }
    }

    /// Which guest code the running samples in JIT code ran: "self" with the leaf in the block,
    /// "calls" inside a callback it made into the emulator.
    void WriteGuestCode(std::FILE* file, const std::vector<std::string>& names) {
        std::vector<JitBlock> blocks;
        {
            std::scoped_lock lock{jit_blocks_mutex};
            blocks = jit_blocks;
        }
        if (blocks.empty()) {
            return;
        }
        std::ranges::stable_sort(blocks, {}, &JitBlock::host);
        const auto find_block = [&blocks](u64 pc) -> const JitBlock* {
            const auto next = std::ranges::upper_bound(blocks, pc, {}, &JitBlock::host);
            if (next == blocks.begin()) {
                return nullptr;
            }
            const JitBlock& block = *std::prev(next);
            return pc < block.host + block.host_size ? &block : nullptr;
        };
        struct Hits {
            u64 end{};
            u64 self{};
            u64 calls{};
        };
        std::map<std::pair<u32, u64>, Hits> hits;
        for (const StackRecord& record : stacks) {
            if (record.waiting || record.stalled) {
                continue;
            }
            for (size_t i = 0; i < record.depth; ++i) {
                const DWORD64 pc = frame_pool[record.first_frame + i];
                DWORD64 base{};
                ModuleName(pc, base);
                if (base != 0) {
                    continue;
                }
                if (i == 0 && InJitThunks(pc)) {
                    break; // counted by thunk_samples, for the block that called the thunk
                }
                const JitBlock* const block = find_block(pc);
                // Host code that is no block (Dynarmic's dispatcher, its fallbacks) stays at pc 0.
                Hits& entry = hits[{record.thread, block ? block->guest_pc : 0}];
                entry.end = block ? block->guest_end : 0;
                (i == 0 ? entry.self : entry.calls) += record.count;
                break;
            }
        }
        std::map<u64, std::pair<u64, u64>> totals; // guest pc -> samples, end
        for (const auto& [key, entry] : hits) {
            std::fputs(fmt::format("guest {} 0x{:x} 0x{:x} {} {}\n", names[key.first], key.second,
                                   entry.end, entry.self, entry.calls)
                           .c_str(),
                       file);
            if (key.second != 0) {
                auto& total = totals[key.second];
                total.first += entry.self + entry.calls;
                total.second = entry.end;
            }
        }
        // Where in the thunks the samples fell, and the thunks' code to disassemble them with.
        for (const auto& [offset, samples] : thunk_rips) {
            std::fputs(fmt::format("thunkrip 0x{:x} {}\n", offset, samples).c_str(), file);
        }
        if (!thunk_rips.empty()) {
            std::string hex;
            {
                std::scoped_lock lock{jit_blocks_mutex};
                hex.reserve(jit_thunk_code.size() * 2);
                for (const u8 byte : jit_thunk_code) {
                    hex.push_back("0123456789abcdef"[byte >> 4]);
                    hex.push_back("0123456789abcdef"[byte & 15]);
                }
            }
            std::fputs("thunkcode ", file);
            std::fputs(hex.c_str(), file);
            std::fputs("\n", file);
        }
        WriteThunkBlocks(file);
        std::map<std::pair<u32, u64>, std::pair<u64, u64>> thunk_hits; // caller: samples, end
        for (const auto& [key, samples] : thunk_samples) {
            const auto& [thread_index, first, second] = key;
            const JitBlock* block = find_block(first);
            if (!block) {
                block = find_block(second);
            }
            auto& entry = thunk_hits[{thread_index, block ? block->guest_pc : 0}];
            entry.first += samples;
            entry.second = block ? block->guest_end : 0;
        }
        for (const auto& [key, entry] : thunk_hits) {
            std::fputs(fmt::format("guestthunk {} 0x{:x} 0x{:x} {}\n", names[key.first], key.second,
                                   entry.second, entry.first)
                           .c_str(),
                       file);
            if (key.second != 0) {
                auto& total = totals[key.second];
                total.first += entry.first;
                total.second = entry.second;
            }
        }
        std::scoped_lock lock{guest_reader_mutex};
        if (!guest_reader) {
            return;
        }
        std::fputs(fmt::format("guestbase 0x{:x}\n", guest_code_base).c_str(), file);
        std::vector<std::pair<u64, u64>> hottest; // samples, guest pc
        for (const auto& [pc, total] : totals) {
            hottest.emplace_back(total.first, pc);
        }
        std::ranges::sort(hottest, std::greater{});
        hottest.resize(std::min(hottest.size(), GUEST_CODE_BLOCKS));
        u8 bytes[GUEST_CODE_BYTES];
        for (const auto& [samples, pc] : hottest) {
            const u64 end = totals[pc].second;
            const size_t size = end > pc ? std::min<u64>(end - pc, sizeof(bytes)) : 0;
            if (size == 0 || !guest_reader(pc, bytes, size)) {
                continue;
            }
            std::string line = fmt::format("guestcode 0x{:x} ", pc);
            for (size_t i = 0; i < size; ++i) {
                fmt::format_to(std::back_inserter(line), "{:02x}", bytes[i]);
            }
            line += '\n';
            std::fputs(line.c_str(), file);
        }
    }

    Options options;
    std::thread thread;
    HANDLE stop_event{};
    HANDLE timer{};
    LARGE_INTEGER qpc_frequency{[] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return frequency;
    }()};

    u8* stack_copy{};
    std::vector<ThreadEntry*> threads;
    u64 seen_generation{~u64{}};

    std::chrono::steady_clock::time_point window_start;
    u64 window_ticks{};
    u64 sampler_cpu_start{};
    u64 suspended_ns{};
    u64 max_suspended_ns{};
    u64 suspensions{};
    u64 dropped_samples{};
    std::vector<StackRecord> stacks;
    std::unordered_map<u64, size_t> stack_index;
    std::vector<DWORD64> frame_pool;
    std::map<std::tuple<u32, u64, u64>, u64> thunk_samples; // thread, top two stack words
    std::map<u64, u64> thunk_rips; // offset into the thunks: samples
    /// Guest page and load/store bit: samples, and those waiting for the page table entry.
    std::unordered_map<u64, std::array<u64, 2>> thunk_pages;
    std::unordered_map<DWORD64, std::string> module_names;
};

std::mutex sampler_mutex;
std::unique_ptr<Sampler> sampler;

} // Anonymous namespace

void RegisterCurrentThread(std::string_view name) {
    const DWORD id = GetCurrentThreadId();
    std::scoped_lock lock{registry_mutex};
    for (const auto& entry : registry) {
        // Thread ids are reused: an entry with this id is this thread only while its own lives.
        if (entry->id == id && !entry->dead && !Exited(entry->handle)) {
            entry->name = SanitizeName(name);
            return;
        }
    }
    HANDLE handle{};
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                         FALSE, 0)) {
        return;
    }
    auto entry = std::make_unique<ThreadEntry>();
    entry->name = SanitizeName(name);
    entry->handle = handle;
    entry->id = id;
    GetCurrentThreadStackLimits(&entry->stack_low, &entry->stack_high);
    registry.push_back(std::move(entry));
    ++registry_generation;
}

void Start(const Options& options) {
    std::scoped_lock lock{sampler_mutex};
    if (sampler) {
        return;
    }
    {
        // Threads of earlier sessions (the library runs several games in one process).
        std::scoped_lock registry_lock{registry_mutex};
        std::erase_if(registry, [](const std::unique_ptr<ThreadEntry>& entry) {
            const bool exited = entry->dead || Exited(entry->handle);
            if (exited) {
                CloseHandle(entry->handle);
            }
            return exited;
        });
        ++registry_generation;
    }
    sampler = std::make_unique<Sampler>(options);
}

void Stop() {
    std::scoped_lock lock{sampler_mutex};
    sampler.reset();
}

bool Running() {
    std::scoped_lock lock{sampler_mutex};
    return sampler != nullptr;
}

void NoteJitBlock(const void* host, size_t host_size, u64 guest_pc, u64 guest_end) {
    std::scoped_lock lock{jit_blocks_mutex};
    jit_blocks.push_back({reinterpret_cast<u64>(host), host_size, guest_pc, guest_end});
}

void NoteJitThunks(const void* host, size_t host_size, const JitThunk* thunks, size_t count) {
    const u64 start = reinterpret_cast<u64>(host);
    std::scoped_lock lock{jit_blocks_mutex};
    jit_thunks.emplace_back(start, start + host_size);
    if (jit_thunk_code.empty()) {
        const u8* const bytes = static_cast<const u8*>(host);
        jit_thunk_code.assign(bytes, bytes + host_size);
        jit_thunk_layout.assign(thunks, thunks + count);
        std::ranges::sort(jit_thunk_layout, {}, &JitThunk::offset);
    }
}

void ClearJitBlocks() {
    std::scoped_lock lock{jit_blocks_mutex};
    jit_blocks = {};
    jit_thunks = {};
    jit_thunk_code = {};
    jit_thunk_layout = {};
}

void SetGuestCodeReader(GuestCodeReader reader, u64 code_base) {
    std::scoped_lock lock{guest_reader_mutex};
    guest_reader = std::move(reader);
    guest_code_base = code_base;
}

void SetGuestBlockClassifier(GuestBlockClassifier classifier) {
    std::scoped_lock lock{guest_reader_mutex};
    guest_classifier = std::move(classifier);
}

void NoteFrame() {
    g_last_frame_ns.store(NowNs(), std::memory_order_relaxed);
}

u64 LastFrameNs() {
    return g_last_frame_ns.load(std::memory_order_relaxed);
}

u64 SteadyNs() {
    return NowNs();
}

std::vector<ThreadStack> CaptureStacks() {
    // One copy buffer for the process, taken once: a freeze report is rare. The registry lock is
    // held throughout so no entry goes away; nothing allocates while a thread is suspended, only
    // after it was resumed.
    static u8* const copy = static_cast<u8*>(
        VirtualAlloc(nullptr, MAX_STACK_COPY + COPY_SLACK, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    std::vector<ThreadStack> stacks;
    std::scoped_lock lock{registry_mutex};
    stacks.reserve(registry.size());
    DWORD64 frames[MAX_FRAMES];
    for (const auto& entry : registry) {
        // Suspending the calling thread would never return.
        if (entry->id == GetCurrentThreadId() || entry->dead || Exited(entry->handle)) {
            continue;
        }
        CONTEXT context{};
        size_t copied{};
        ULONG_PTR origin{};
        if (!CaptureThread(*entry, copy, context, copied, origin)) {
            continue;
        }
        const size_t depth = UnwindCopy(context, copy, copied, origin, frames);
        stacks.push_back({entry->name, entry->id, {frames, frames + depth}});
    }
    return stacks;
}

} // namespace Common::SamplingProfiler
