// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Commit census: private, section and image commit from one walk of the address space, and the
// commit left outside it.

#include <cstdio>
#include <stdexcept>
#include <string>

#include "common/commit_census.cpp"

namespace {
constexpr u64 MiB = 1ULL << 20;

void Expect(bool condition, const std::string& what) {
    if (!condition) {
        throw std::runtime_error(what);
    }
}
} // Anonymous namespace

int main() try {
    const Common::AddressSpaceCommit before = Common::WalkAddressSpaceCommit();
    Expect(before.regions != 0 && before.image_bytes != 0, "empty walk");

    void* const heap = VirtualAlloc(nullptr, 64 * MiB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Expect(heap != nullptr, "VirtualAlloc failed");
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(32 * MiB), nullptr);
    Expect(section != nullptr, "CreateFileMapping failed");
    void* const view = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    Expect(view != nullptr, "MapViewOfFile failed");

    const Common::AddressSpaceCommit after = Common::WalkAddressSpaceCommit();
    const u64 grew_private = after.private_bytes - before.private_bytes;
    const u64 grew_mapped = after.mapped_bytes - before.mapped_bytes;
    Expect(grew_private >= 64 * MiB && grew_private < 80 * MiB,
           "private grew " + std::to_string(grew_private >> 20) + " MiB for 64");
    Expect(grew_mapped >= 32 * MiB && grew_mapped < 40 * MiB,
           "sections grew " + std::to_string(grew_mapped >> 20) + " MiB for 32");
    Expect(after.image_bytes == before.image_bytes, "images changed");

    // Whatever the commit holds past the views is what the GPU driver and the kernel keep.
    const u64 viewed = after.private_bytes + after.mapped_bytes;
    const std::string line = Common::DescribeCommitCensus(after, viewed + 100 * MiB);
    Expect(line.find("outside the address space 100 MiB") != std::string::npos, line);
    const std::string under = Common::DescribeCommitCensus(after, viewed / 2);
    Expect(under.find("outside the address space 0 MiB") != std::string::npos, under);

    UnmapViewOfFile(view);
    CloseHandle(section);
    VirtualFree(heap, 0, MEM_RELEASE);
    const Common::AddressSpaceCommit freed = Common::WalkAddressSpaceCommit();
    Expect(after.private_bytes - freed.private_bytes >= 64 * MiB, "private not given back");
    Expect(after.mapped_bytes - freed.mapped_bytes >= 32 * MiB, "section not given back");

    std::printf("PASS commit census: +64 MiB private and +32 MiB section seen and given back; %s\n",
                line.c_str());
    return 0;
} catch (const std::exception& error) {
    std::printf("FAIL commit census: %s\n", error.what());
    return 1;
}
