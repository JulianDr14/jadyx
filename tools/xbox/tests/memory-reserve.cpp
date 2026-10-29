// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// The driver memory reserve: committed memory that a pipeline object out of memory releases and
// that is committed again later, measured on the process's private commit.

#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_memory_reserve.cpp"

namespace {
namespace Reserve = D3D12::MemoryReserve;
constexpr std::int64_t MiB = 1 << 20;

std::int64_t PrivateCommit() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        throw std::runtime_error("GetProcessMemoryInfo failed");
    }
    return static_cast<std::int64_t>(counters.PrivateUsage);
}

void Expect(bool condition, const std::string& what) {
    if (!condition) {
        throw std::runtime_error(what);
    }
}

// Commit moved by at least most of the reserve, in the expected direction.
void ExpectMoved(std::int64_t before, std::int64_t sign, const char* what) {
    const std::int64_t moved = (PrivateCommit() - before) * sign;
    Expect(moved >= 30 * MiB, std::string(what) + ": private commit moved " +
                                  std::to_string(moved / MiB) + " MiB");
}
} // Anonymous namespace

int main() try {
    std::int64_t before = PrivateCommit();
    Reserve::Configure(32 * MiB);
    Expect(Reserve::Held() == 32 * MiB, "configured reserve not held");
    ExpectMoved(before, 1, "configure");

    before = PrivateCommit();
    Expect(Reserve::Release(), "release of a held reserve reported nothing");
    Expect(Reserve::Held() == 0, "released reserve still held");
    ExpectMoved(before, -1, "release");
    Expect(!Reserve::Release(), "second release reported memory");

    before = PrivateCommit();
    Expect(Reserve::Rearm(), "rearm failed");
    Expect(Reserve::Held() == 32 * MiB, "rearmed reserve not held");
    ExpectMoved(before, 1, "rearm");
    Expect(Reserve::Rearm(), "rearm of a held reserve failed");
    Expect(Reserve::Held() == 32 * MiB, "second rearm changed the reserve");

    Expect(Reserve::OutOfMemoryEvents() == 0, "out of memory events before any");
    Reserve::NoteOutOfMemory();
    Reserve::NoteOutOfMemory();
    Expect(Reserve::OutOfMemoryEvents() == 2, "out of memory events not counted");

    // Pipeline workers release while the GPU thread rearms: it ends held or not, never torn.
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 200; ++i) {
                if ((i + t) % 2) {
                    Reserve::Release();
                } else {
                    Reserve::Rearm();
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const std::uint64_t held = Reserve::Held();
    Expect(held == 0 || held == 32 * MiB, "reserve torn: " + std::to_string(held));

    before = PrivateCommit();
    Reserve::Configure(0);
    Expect(Reserve::Held() == 0, "reserve held when off");
    if (held != 0) {
        ExpectMoved(before, -1, "configure off");
    }
    Expect(!Reserve::Release(), "release with the reserve off reported memory");
    Expect(Reserve::Rearm(), "rearm with the reserve off failed");
    Expect(Reserve::Held() == 0, "rearm with the reserve off committed memory");

    std::puts("PASS memory reserve: 32 MiB committed, released, committed again; off holds none; "
              "1600 concurrent releases and rearms");
    return 0;
} catch (const std::exception& error) {
    std::printf("FAIL memory reserve: %s\n", error.what());
    return 1;
}
