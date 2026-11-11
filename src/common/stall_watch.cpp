// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <vector>

#include <fmt/format.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/stall_watch.h"

namespace Common::StallWatch {
namespace {

/// Pipeline workers, boot workers, the GPU thread and the recorder fit with room to spare; a
/// thread past them is simply not watched.
constexpr size_t SLOTS = 64;

struct Slot {
    std::atomic<bool> used{};
    std::atomic<u32> thread_id{};
    std::atomic<u64> start_ms{}; ///< 0 while idle
    std::atomic<Work> work{};
    std::atomic<u64> a{};
    std::atomic<u64> b{};
};

std::array<Slot, SLOTS> slots;

u32 CurrentThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return 0;
#endif
}

/// The calling thread's slot, given back when the thread ends.
struct ThreadSlot {
    int index = -1;
    bool tried = false;

    ~ThreadSlot() {
        if (index >= 0) {
            slots[index].start_ms.store(0, std::memory_order_relaxed);
            slots[index].used.store(false, std::memory_order_release);
        }
    }
};

thread_local ThreadSlot thread_slot;

int AcquireSlot() noexcept {
    if (thread_slot.tried) {
        return thread_slot.index;
    }
    thread_slot.tried = true;
    for (size_t i = 0; i < SLOTS; ++i) {
        bool expected = false;
        if (slots[i].used.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            slots[i].thread_id.store(CurrentThreadId(), std::memory_order_relaxed);
            slots[i].start_ms.store(0, std::memory_order_relaxed);
            thread_slot.index = static_cast<int>(i);
            break;
        }
    }
    return thread_slot.index;
}

} // Anonymous namespace

u64 NowMs() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

const char* WorkName(Work work) noexcept {
    switch (work) {
    case Work::None:
        return "idle";
    case Work::PipelineDxil:
        return "pipeline DXIL";
    case Work::MesaTranslate:
        return "Mesa translation";
    case Work::ValidatorLock:
        return "validator lock";
    case Work::ValidatorSign:
        return "validator sign";
    case Work::DriverPso:
        return "driver PSO";
    case Work::ComputeDxil:
        return "compute DXIL";
    case Work::ComputePso:
        return "driver compute PSO";
    case Work::PipelineWait:
        return "waiting for a pipeline";
    case Work::ComputeWait:
        return "waiting for a compute PSO";
    }
    return "?";
}

Scope::Scope(Work work, u64 a, u64 b) noexcept : slot{AcquireSlot()} {
    if (slot < 0) {
        return;
    }
    Slot& s = slots[slot];
    previous_work = s.work.load(std::memory_order_relaxed);
    previous_a = s.a.load(std::memory_order_relaxed);
    previous_b = s.b.load(std::memory_order_relaxed);
    previous_start_ms = s.start_ms.load(std::memory_order_relaxed);
    if (a == 0 && b == 0 && previous_start_ms != 0) {
        a = previous_a;
        b = previous_b;
    }
    s.work.store(work, std::memory_order_relaxed);
    s.a.store(a, std::memory_order_relaxed);
    s.b.store(b, std::memory_order_relaxed);
    s.start_ms.store(NowMs(), std::memory_order_relaxed);
}

Scope::~Scope() {
    if (slot < 0) {
        return;
    }
    Slot& s = slots[slot];
    s.work.store(previous_work, std::memory_order_relaxed);
    s.a.store(previous_a, std::memory_order_relaxed);
    s.b.store(previous_b, std::memory_order_relaxed);
    s.start_ms.store(previous_start_ms, std::memory_order_relaxed);
}

std::string Describe(u64 min_ms) {
    struct Busy {
        u64 start;
        u32 thread_id;
        Work work;
        u64 a;
        u64 b;
    };
    std::vector<Busy> busy;
    const u64 now = NowMs();
    for (const Slot& s : slots) {
        if (!s.used.load(std::memory_order_acquire)) {
            continue;
        }
        const u64 start = s.start_ms.load(std::memory_order_relaxed);
        if (start == 0 || start > now || now - start < min_ms) {
            continue;
        }
        busy.push_back({start, s.thread_id.load(std::memory_order_relaxed),
                        s.work.load(std::memory_order_relaxed), s.a.load(std::memory_order_relaxed),
                        s.b.load(std::memory_order_relaxed)});
    }
    std::ranges::sort(busy, {}, &Busy::start);
    std::string out;
    for (const Busy& item : busy) {
        if (!out.empty()) {
            out += "; ";
        }
        out += fmt::format("thread {} {} for {:.1f} s", item.thread_id, WorkName(item.work),
                           static_cast<double>(now - item.start) / 1000.0);
        if (item.b != 0) {
            out += fmt::format(" (VS {:016x} PS {:016x})", item.a, item.b);
        } else if (item.a != 0) {
            out += fmt::format(" ({:016x})", item.a);
        }
    }
    return out;
}

u64 OldestStartMs() {
    u64 oldest = 0;
    for (const Slot& s : slots) {
        if (!s.used.load(std::memory_order_acquire)) {
            continue;
        }
        const u64 start = s.start_ms.load(std::memory_order_relaxed);
        if (start != 0 && (oldest == 0 || start < oldest)) {
            oldest = start;
        }
    }
    return oldest;
}

} // namespace Common::StallWatch
