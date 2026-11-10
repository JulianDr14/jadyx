// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/semaphore_trace.h"
#include "core/arm/arm_interface.h"
#include "core/arm/debug.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/guest_probe.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_light_lock.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/memory.h"

namespace Kernel::GuestProbe {

namespace Detail {
std::atomic<u32> pending_cores{};
} // namespace Detail

namespace {

constexpr std::size_t CoreCount = Core::Hardware::NUM_CPU_CORES;
constexpr int StackFrames = 10;

struct Sample {
    bool valid{};
    u64 thread_id{};
    Svc::ThreadContext context{};
};

std::mutex g_samples_mutex;
std::array<Sample, CoreCount> g_samples; ///< Under g_samples_mutex.
std::mutex g_report_mutex;               ///< One report at a time.

std::string Hex(const u8* bytes, std::size_t size) {
    std::string text;
    for (std::size_t i = 0; i < size; ++i) {
        text += fmt::format("{:02x}", bytes[i]);
    }
    return text;
}

/// Reads guest memory without flushing GPU-cached pages. The probe runs on its own host thread,
/// so it must never reach the rasterizer caches (not thread-safe) nor the page table lock; every
/// guest read in this file goes through here.
bool ReadGuest(Core::Memory::Memory& memory, u64 address, void* out, std::size_t size) {
    return address != 0 && address + size > address &&
           memory.IsValidVirtualAddressRange(address, size) &&
           memory.ReadBlockUnsafe(address, out, size);
}

/// Raw frame-pointer walk: guest addresses only. Symbolication (module scan, symbol tables) reads
/// through the rasterizer and takes kernel locks, which crashed the report on a frozen game.
std::string Stack(KProcess* process, const Svc::ThreadContext& context) {
    auto& memory = process->GetMemory();
    std::string text = fmt::format("{:x} | {:x}", context.pc, context.lr);
    u64 fp = context.fp;
    for (int count = 2; count < StackFrames; ++count) {
        std::array<u64, 2> record{};
        if (fp % 8 != 0 || !ReadGuest(memory, fp, record.data(), sizeof(record))) {
            return text;
        }
        text += fmt::format(" | {:x}", record[1]);
        if (record[0] <= fp) {
            return text; // frames grow up the stack; anything else is a broken chain
        }
        fp = record[0];
    }
    return text + " | ...";
}

/// The nnsdk thread name, read with ReadGuest (Core::GetThreadName reads through the rasterizer).
std::string ThreadLabel(const KThread* thread) {
    std::string name;
    KProcess* process = thread->GetOwnerProcess();
    if (process != nullptr && process->Is64Bit()) {
        auto& memory = process->GetMemory();
        u64 thread_type = 0;
        u16 version = 0;
        u64 name_pointer = 0;
        std::array<char, 64> buffer{};
        const u64 argument = thread->GetArgument();
        if (ReadGuest(memory, GetInteger(thread->GetTlsAddress()) + 0x1f8, &thread_type, 8) &&
            thread_type != 0 && (argument == 0 || argument == thread_type) &&
            ReadGuest(memory, thread_type + 0x46, &version, 2) &&
            ReadGuest(memory, thread_type + (version == 1 ? 0x1a0 : 0x1a8), &name_pointer, 8) &&
            ReadGuest(memory, name_pointer, buffer.data(), buffer.size() - 1)) {
            name = buffer.data();
        }
    }
    return fmt::format("0x{:x} \"{}\"", thread->GetThreadId(), name);
}

/// Full detail of the guest thread a core ran: registers, call stack, the code around pc and
/// the memory its registers point at (a spin loop's flag or counter shows up there).
std::string DescribeCore(KProcess* process, std::size_t core, const Sample& sample) {
    const auto& ctx = sample.context;
    auto& memory = process->GetMemory();
    std::string text = fmt::format("  core {}: thread 0x{:x} pc={:x} lr={:x} sp={:x} fp={:x}\n",
                                   core, sample.thread_id, ctx.pc, ctx.lr, ctx.sp, ctx.fp);
    for (std::size_t i = 0; i < ctx.r.size(); i += 6) {
        text += "   ";
        for (std::size_t j = i; j < i + 6 && j < ctx.r.size(); ++j) {
            text += fmt::format(" x{}={:x}", j, ctx.r[j]);
        }
        text += "\n";
    }
    std::array<u32, 12> code{};
    if (ReadGuest(memory, ctx.pc - 24, code.data(), sizeof(code))) {
        text += "    code @pc-24:";
        for (std::size_t i = 0; i < code.size(); ++i) {
            text += i == 6 ? fmt::format(" [{:08x}]", code[i]) : fmt::format(" {:08x}", code[i]);
        }
        text += "\n";
    }
    text += "    stack: " + Stack(process, ctx) + "\n";
    std::string pointed;
    for (std::size_t i = 0; i < ctx.r.size(); ++i) {
        std::array<u8, 16> bytes{};
        if (ctx.r[i] % 4 == 0 && ReadGuest(memory, ctx.r[i], bytes.data(), bytes.size())) {
            pointed += fmt::format(" x{}->{}", i, Hex(bytes.data(), bytes.size()));
        }
    }
    if (!pointed.empty()) {
        text += "    memory:" + pointed + "\n";
    }
    return text;
}

std::string DescribeThreads(KernelCore& kernel, KProcess* process) {
    std::vector<KThread*> threads;
    {
        KScopedLightLock lk{process->GetListLock()};
        for (auto& thread : process->GetThreadList()) {
            threads.push_back(&thread);
        }
    }
    std::string text = fmt::format(" threads ({}):\n", threads.size());
    for (KThread* thread : threads) {
        const auto& ctx = thread->GetContext();
        text += fmt::format("  {} prio {} core {} ideal {} mask {:x} {} pc={:x} lr={:x}",
                            ThreadLabel(thread), thread->GetPriority(), thread->GetActiveCore(),
                            thread->GetPhysicalIdealCoreForDebugging(),
                            thread->GetAffinityMask().GetAffinityMask(),
                            Core::GetThreadState(thread), ctx.pc, ctx.lr);
        if (thread->GetState() == ThreadState::Waiting) {
            if (const u64 key = GetInteger(thread->GetAddressKey()); key != 0) {
                text += fmt::format(" mutex_key={:x}", key);
            }
            if (const u64 key = thread->GetAddressArbiterKey(); key != 0) {
                text += fmt::format(" arbiter_key={:x}", key);
            }
            text += "\n    stack: " + Stack(process, ctx);
        }
        text += "\n";
    }
    return text;
}

} // namespace

void Detail::Record(std::size_t core, Core::ArmInterface& interface, KThread& thread) {
    {
        std::scoped_lock lk{g_samples_mutex};
        Sample& sample = g_samples[core];
        sample.valid = true;
        sample.thread_id = thread.GetThreadId();
        interface.GetContext(sample.context);
    }
    pending_cores.fetch_and(~(1u << core), std::memory_order_relaxed);
}

std::string Report(KernelCore& kernel, int rounds, std::chrono::milliseconds gap) {
    std::scoped_lock report_lock{g_report_mutex};
    KProcess* process = kernel.ApplicationProcess();
    if (process == nullptr) {
        return "guest probe: no application process";
    }
    std::string text = "guest probe:\n";
    // Host pointers of what the running guest threads' registers point at, to match against the
    // GPU semaphore writes (both views of guest memory share the host pointer).
    std::vector<const void*> watch;
    for (int round = 0; round < rounds; ++round) {
        {
            std::scoped_lock lk{g_samples_mutex};
            g_samples = {};
        }
        Detail::pending_cores.store((1u << CoreCount) - 1, std::memory_order_relaxed);
        for (std::size_t core = 0; core < CoreCount; ++core) {
            kernel.PhysicalCore(core).Interrupt();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{250};
        while (Detail::pending_cores.load(std::memory_order_relaxed) != 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        Detail::pending_cores.store(0, std::memory_order_relaxed);
        std::array<Sample, CoreCount> samples;
        {
            std::scoped_lock lk{g_samples_mutex};
            samples = g_samples;
        }
        text += fmt::format(" round {}/{}:\n", round + 1, rounds);
        for (std::size_t core = 0; core < CoreCount; ++core) {
            const Sample& sample = samples[core];
            if (!sample.valid) {
                // Read without the scheduler lock: a frozen guest may hold it.
                const KThread* current = kernel.Scheduler(core).GetSchedulerCurrentThread();
                text += fmt::format("  core {}: no guest code ran in 250 ms (idle or in a system "
                                    "call), scheduler thread {}\n",
                                    core,
                                    current && current->GetOwnerProcess() == process ?
                                        ThreadLabel(current) : std::string("none/idle"));
            } else if (round == 0) {
                text += DescribeCore(process, core, sample);
                auto& memory = process->GetMemory();
                for (const u64 value : sample.context.r) {
                    u32 word{};
                    if (value % 4 == 0 && ReadGuest(memory, value, &word, sizeof(word))) {
                        watch.push_back(memory.GetPointerSilent(value));
                    }
                }
            } else {
                text += fmt::format("  core {}: thread 0x{:x} pc={:x} lr={:x} x0={:x} x1={:x}\n",
                                    core, sample.thread_id, sample.context.pc, sample.context.lr,
                                    sample.context.r[0], sample.context.r[1]);
            }
        }
        if (round + 1 < rounds) {
            std::this_thread::sleep_for(gap);
        }
    }
    text += " register targets (host):";
    for (const void* pointer : watch) {
        text += fmt::format(" {}", pointer);
    }
    text += "\n";
    return text + DescribeThreads(kernel, process) + Common::SemaphoreTrace::Describe(watch, 16);
}

} // namespace Kernel::GuestProbe
