// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/commit_census.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <chrono>

namespace Common {

AddressSpaceCommit WalkAddressSpaceCommit() {
    AddressSpaceCommit space;
#ifdef _WIN32
    const auto start = std::chrono::steady_clock::now();
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto* address = static_cast<u8*>(system.lpMinimumApplicationAddress);
    auto* const end = static_cast<u8*>(system.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION info{};
    while (address < end && VirtualQuery(address, &info, sizeof(info)) == sizeof(info)) {
        if (info.State == MEM_COMMIT) {
            ++space.regions;
            switch (info.Type) {
            case MEM_PRIVATE:
                space.private_bytes += info.RegionSize;
                break;
            case MEM_MAPPED:
                space.mapped_bytes += info.RegionSize;
                break;
            case MEM_IMAGE:
                space.image_bytes += info.RegionSize;
                break;
            default:
                break;
            }
        }
        auto* const next = static_cast<u8*>(info.BaseAddress) + info.RegionSize;
        if (next <= address) {
            break;
        }
        address = next;
    }
    space.walk_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count());
#endif
    return space;
}

std::string DescribeCommitCensus(const AddressSpaceCommit& space, u64 app_commit) {
    const u64 viewed = space.private_bytes + space.mapped_bytes;
    return "commit census: private " + std::to_string(space.private_bytes >> 20) +
           " MiB, sections " + std::to_string(space.mapped_bytes >> 20) + " MiB, images " +
           std::to_string(space.image_bytes >> 20) + " MiB; outside the address space " +
           std::to_string((app_commit > viewed ? app_commit - viewed : 0) >> 20) + " MiB of " +
           std::to_string(app_commit >> 20) + " MiB commit (GPU driver and kernel); " +
           std::to_string(space.regions) + " regions in " + std::to_string(space.walk_us) + " us";
}

} // namespace Common
