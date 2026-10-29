// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_memory_reserve.h"

namespace D3D12::MemoryReserve {
namespace {
std::mutex mutex;
std::unique_ptr<std::uint8_t[]> block; ///< mutex
std::uint64_t configured{};            ///< mutex
std::atomic<std::uint64_t> held{};
std::atomic<std::uint64_t> out_of_memory{};

// Large CRT allocations come straight from the OS and go back to it when freed; touching every
// page makes them count against the commit the driver would otherwise have taken.
bool Commit() {
    block.reset(new (std::nothrow) std::uint8_t[configured]);
    if (!block) {
        return false;
    }
    std::memset(block.get(), 0xA5, configured);
    held.store(configured, std::memory_order_relaxed);
    return true;
}
} // Anonymous namespace

void Configure(std::uint64_t bytes) {
    std::scoped_lock lock{mutex};
    block.reset();
    held.store(0, std::memory_order_relaxed);
    configured = bytes;
    if (configured != 0 && !Commit()) {
        LOG_WARNING(Render, "D3D12: could not commit the {} MiB memory reserve", bytes >> 20);
    }
}

bool Release() {
    std::scoped_lock lock{mutex};
    if (!block) {
        return false;
    }
    block.reset();
    held.store(0, std::memory_order_relaxed);
    LOG_WARNING(Render, "D3D12: released the {} MiB memory reserve for the driver",
                configured >> 20);
    return true;
}

bool Rearm() {
    std::scoped_lock lock{mutex};
    if (block || configured == 0) {
        return true;
    }
    if (!Commit()) {
        return false;
    }
    LOG_INFO(Render, "D3D12: memory reserve of {} MiB committed again", configured >> 20);
    return true;
}

std::uint64_t Held() {
    return held.load(std::memory_order_relaxed);
}

std::uint64_t OutOfMemoryEvents() {
    return out_of_memory.load(std::memory_order_relaxed);
}

void NoteOutOfMemory() {
    out_of_memory.fetch_add(1, std::memory_order_relaxed);
}

} // namespace D3D12::MemoryReserve
