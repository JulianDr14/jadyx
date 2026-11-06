// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// In-process sampling profiler gate: a busy thread is seen running with its whole call chain,
// a blocked one waiting, a thread running on a fiber stack (as the emulated cores do) and one
// with a deep stack are unwound too, and sampling never deadlocks a thread that allocates.
// Build and run (from a Developer prompt, at the repository root):
//   cl /nologo /std:c++20 /EHsc /O2 /Zi /utf-8 /DNOMINMAX /DFMT_HEADER_ONLY /Isrc
//      /I.cache\cpm\fmt\12.1.0\include
//      tools\xbox\tests\sampling-profiler.cpp src\common\windows\sampling_profiler.cpp
//      /Fe:build-uwp\diagnostics\sampling-profiler\sampling-profiler.exe
//      /Fo:build-uwp\diagnostics\sampling-profiler\ /link /DEBUG
//   build-uwp\diagnostics\sampling-profiler\sampling-profiler.exe
//   python tools\xbox\profile-report.py profile.txt
//      --exe build-uwp\diagnostics\sampling-profiler\sampling-profiler.exe

#include <windows.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "common/windows/sampling_profiler.h"

// The profiler logs slow captures; the gate does not link the logging backend.
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned int, const char*, fmt::string_view,
                       const fmt::format_args&) {}
} // namespace Common::Log

namespace {

std::atomic<bool> running{true};
volatile unsigned sink;

__declspec(noinline) unsigned BusyLeaf(unsigned seed) {
    unsigned value = seed;
    for (int i = 0; i < 2000; ++i) {
        value = value * 1664525u + 1013904223u;
    }
    return value;
}

__declspec(noinline) unsigned BusyMiddle(unsigned depth, unsigned seed) {
    if (depth == 0) {
        return BusyLeaf(seed);
    }
    volatile char pad[64];
    pad[0] = static_cast<char>(depth);
    return BusyMiddle(depth - 1, seed + pad[0]) + 1;
}

__declspec(noinline) void BusyThread() {
    Common::SamplingProfiler::RegisterCurrentThread("busy");
    while (running.load(std::memory_order_relaxed)) {
        sink = BusyMiddle(6, sink);
    }
}

__declspec(noinline) unsigned DeepRecursion(unsigned depth) {
    // About 300 KiB of stack in all: past the copy limit's comfort zone, below its cap.
    volatile char frame[1024];
    frame[0] = static_cast<char>(depth);
    if (depth == 0) {
        return BusyLeaf(frame[0]);
    }
    return DeepRecursion(depth - 1) + frame[0];
}

__declspec(noinline) void DeepThread() {
    Common::SamplingProfiler::RegisterCurrentThread("deep");
    while (running.load(std::memory_order_relaxed)) {
        sink = DeepRecursion(290);
    }
}

std::mutex wait_mutex;
std::condition_variable wait_cv;

__declspec(noinline) void SleeperThread() {
    Common::SamplingProfiler::RegisterCurrentThread("sleeper");
    std::unique_lock lock{wait_mutex};
    wait_cv.wait(lock, [] { return !running.load(); });
}

__declspec(noinline) void AllocatorThread() {
    // Suspended mid-allocation, the heap lock is held: the profiler must not need it then.
    Common::SamplingProfiler::RegisterCurrentThread("allocator");
    while (running.load(std::memory_order_relaxed)) {
        auto block = std::make_unique<std::vector<int>>(1 + sink % 4096);
        sink = static_cast<unsigned>(block->size());
    }
}

void* main_fiber;

__declspec(noinline) void WINAPI FiberWork(void*) {
    while (running.load(std::memory_order_relaxed)) {
        sink = BusyMiddle(3, sink);
    }
    SwitchToFiber(main_fiber);
}

__declspec(noinline) void FiberThread() {
    Common::SamplingProfiler::RegisterCurrentThread("fiber");
    main_fiber = ConvertThreadToFiber(nullptr);
    void* const fiber = CreateFiber(256 * 1024, FiberWork, nullptr);
    SwitchToFiber(fiber);
    DeleteFiber(fiber);
    ConvertFiberToThread();
}

/// Code outside every module, as a JIT block is: loop while *rcx != 0; ret.
constexpr unsigned char JIT_LOOP[] = {0x48, 0x8B, 0x01, 0x48, 0x85, 0xC0, 0x75, 0xF8, 0xC3};
constexpr u64 JIT_GUEST_PC = 0x80001000;
std::atomic<unsigned long long> jit_flag{1};

/// A block that calls a thunk for its load: call thunk; test rax, rax; jnz block; ret, and at +16
/// the thunk: mov rax, [rcx]; ret.
constexpr unsigned char JIT_CALLER[] = {0xE8, 0x0B, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0,
                                        0x75, 0xF6, 0xC3};
constexpr unsigned char JIT_THUNK[] = {0x48, 0x8B, 0x01, 0xC3};
constexpr size_t JIT_CALLER_OFFSET = 256;
constexpr size_t JIT_THUNK_OFFSET = JIT_CALLER_OFFSET + 16;
constexpr u64 JIT_CALLER_PC = 0x80002000;

__declspec(noinline) void JitThread(const char* name, const void* code) {
    Common::SamplingProfiler::RegisterCurrentThread(name);
    reinterpret_cast<void (*)(std::atomic<unsigned long long>*)>(const_cast<void*>(code))(&jit_flag);
}

} // Anonymous namespace

int main() {
    const std::string path = "profile.txt";
    void* const jit_code =
        VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(jit_code != nullptr);
    std::memcpy(jit_code, JIT_LOOP, sizeof(JIT_LOOP));
    // A stale block at the same address: the later one wins.
    Common::SamplingProfiler::NoteJitBlock(jit_code, sizeof(JIT_LOOP), 0x70000000, 0x70000004);
    Common::SamplingProfiler::NoteJitBlock(jit_code, sizeof(JIT_LOOP), JIT_GUEST_PC,
                                           JIT_GUEST_PC + 8);
    auto* const code_bytes = static_cast<unsigned char*>(jit_code);
    std::memcpy(code_bytes + JIT_CALLER_OFFSET, JIT_CALLER, sizeof(JIT_CALLER));
    std::memcpy(code_bytes + JIT_THUNK_OFFSET, JIT_THUNK, sizeof(JIT_THUNK));
    Common::SamplingProfiler::NoteJitBlock(code_bytes + JIT_CALLER_OFFSET, sizeof(JIT_CALLER),
                                           JIT_CALLER_PC, JIT_CALLER_PC + 8);
    Common::SamplingProfiler::NoteJitThunks(code_bytes + JIT_THUNK_OFFSET, sizeof(JIT_THUNK));
    Common::SamplingProfiler::SetGuestCodeReader(
        [](u64 address, void* out, size_t size) {
            std::memset(out, 0xAB, size);
            return address == JIT_GUEST_PC;
        },
        0x80000000);
    std::vector<std::thread> threads;
    threads.emplace_back(JitThread, "jit", jit_code);
    threads.emplace_back(JitThread, "caller", code_bytes + JIT_CALLER_OFFSET);
    threads.emplace_back(BusyThread);
    threads.emplace_back(DeepThread);
    threads.emplace_back(SleeperThread);
    threads.emplace_back(AllocatorThread);
    threads.emplace_back(FiberThread);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Common::SamplingProfiler::Start({.hz = 1000, .window_seconds = 2, .output = path});
    std::this_thread::sleep_for(std::chrono::milliseconds(4500));
    Common::SamplingProfiler::Stop();

    running = false;
    jit_flag = 0;
    wait_cv.notify_all();
    for (auto& thread : threads) {
        thread.join();
    }
    Common::SamplingProfiler::SetGuestCodeReader({}, 0);
    Common::SamplingProfiler::ClearJitBlocks();
    VirtualFree(jit_code, 0, MEM_RELEASE);

    std::ifstream file{path};
    std::string line;
    unsigned long long jit_guest_samples = 0, thunk_samples = 0, caller_samples = 0;
    bool stale_block = false, guest_code = false, guest_base = false;
    unsigned windows = 0;
    unsigned long long busy_running = 0, sleeper_waiting = 0, fiber_running = 0;
    unsigned long long deep_running = 0, allocator_running = 0, sleeper_running = 0;
    size_t busy_max_depth = 0, deep_max_depth = 0, fiber_max_depth = 0;
    while (std::getline(file, line)) {
        if (line.starts_with("window ")) {
            ++windows;
            continue;
        }
        if (line.starts_with("guest jit ")) {
            std::istringstream tokens{line};
            std::string tag, thread, pc, end;
            unsigned long long self{}, calls{};
            tokens >> tag >> thread >> pc >> end >> self >> calls;
            jit_guest_samples += pc == "0x80001000" && end == "0x80001008" ? self : 0;
            stale_block |= pc == "0x70000000";
            continue;
        }
        if (line.starts_with("guestthunk caller 0x80002000 0x80002008 ")) {
            thunk_samples += std::stoull(line.substr(line.rfind(' ') + 1));
            continue;
        }
        if (line.starts_with("guest caller 0x80002000 0x80002008 ")) {
            std::istringstream tokens{line};
            std::string tag, thread, pc, end;
            unsigned long long self{}, calls{};
            tokens >> tag >> thread >> pc >> end >> self >> calls;
            caller_samples += self;
            continue;
        }
        guest_base |= line == "guestbase 0x80000000";
        guest_code |= line == "guestcode 0x80001000 abababababababab";
        if (!line.starts_with("stack ")) {
            continue;
        }
        std::istringstream tokens{line};
        std::string tag, thread, state, frames;
        unsigned long long count{};
        tokens >> tag >> thread >> state >> count >> frames;
        const size_t depth = static_cast<size_t>(std::count(frames.begin(), frames.end(), ';')) + 1;
        if (thread == "busy" && state == "R") {
            busy_running += count;
            busy_max_depth = std::max(busy_max_depth, depth);
        } else if (thread == "sleeper" && state == "W") {
            sleeper_waiting += count;
        } else if (thread == "sleeper") {
            sleeper_running += count;
        } else if (thread == "fiber" && state == "R") {
            fiber_running += count;
            fiber_max_depth = std::max(fiber_max_depth, depth);
        } else if (thread == "deep" && state == "R") {
            deep_running += count;
            deep_max_depth = std::max(deep_max_depth, depth);
        } else if (thread == "allocator" && state == "R") {
            allocator_running += count;
        }
    }
    std::cout << "windows " << windows << ", busy " << busy_running << " samples (depth up to "
              << busy_max_depth << "), deep " << deep_running << " (depth up to " << deep_max_depth
              << "), fiber " << fiber_running << " (depth up to " << fiber_max_depth
              << "), allocator " << allocator_running << ", sleeper waiting " << sleeper_waiting
              << ", JIT guest samples " << jit_guest_samples << ", in the thunk " << thunk_samples
              << ", in its caller " << caller_samples << "\n";
    assert(windows >= 2);
    // Samples in code outside the modules name the guest block, the latest one at that address.
    assert(jit_guest_samples > 1000 && !stale_block && guest_base && guest_code);
    // A sample in a thunk goes to the block that called it, through the return address.
    assert(thunk_samples > 100 && caller_samples > 100);
    // About 1000 samples a second for 4.5 s; leave room for a loaded machine.
    assert(busy_running > 2000);
    // BusyLeaf + 7 BusyMiddle + BusyThread + the thread's start frames.
    assert(busy_max_depth >= 10);
    assert(deep_running > 1000 && deep_max_depth >= 90);
    assert(fiber_running > 1000 && fiber_max_depth >= 5);
    assert(allocator_running > 1000);
    // Blocked: its samples are waiting ones, though being suspended makes it run a little.
    assert(sleeper_waiting > 0 && sleeper_running * 20 < sleeper_waiting);
    std::cout << "PASS\n";
    return 0;
}
