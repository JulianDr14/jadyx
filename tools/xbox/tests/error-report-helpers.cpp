// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Pure helpers behind the error-report fixes: host-contiguous page runs (GetSpan), the secure
// applet region, the software keyboard config layout, nvdrv nodes without a backend and the
// image size limits that keep garbage texture headers away from the host allocator.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "common/page_run.h"
#include "core/hle/kernel/board/nintendo/nx/secure_applet_memory.h"
#include "core/hle/service/am/frontend/applet_software_keyboard_layout.h"
#include "core/hle/service/nvdrv/nvdrv_unbacked_devices.h"
#include "video_core/texture_cache/image_limits.h"

namespace {
int failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Page table stand-ins: page is host pointer minus guest address, type 0 is unmapped.
struct RawEntry {
    std::uintptr_t page;
    int type;
};
struct Entry {
    RawEntry raw;
    RawEntry Raw() const {
        return raw;
    }
};

constexpr int Unmapped = 0;
constexpr int Direct = 1;
constexpr int Debug = 2;

bool IsDirect(const RawEntry& raw) {
    return raw.type == Direct;
}

void TestPageRun() {
    std::vector<Entry> entries(8, Entry{{0x1000, Direct}});
    Check(Common::IsHostContiguousRun(entries, 0, 7, IsDirect), "one mapping is one run");
    Check(Common::IsHostContiguousRun(entries, 3, 3, IsDirect), "a single page is a run");

    // Two mapping calls that landed back to back in host memory store the same value, so the
    // run spans both; the old block-id check rejected this.
    entries[4].raw = {0x1000, Direct};
    Check(Common::IsHostContiguousRun(entries, 2, 6, IsDirect), "adjacent mappings join");

    // A hole in the middle: the old first/last-only check let this through.
    entries[4].raw = {0x9000, Direct};
    Check(!Common::IsHostContiguousRun(entries, 2, 6, IsDirect), "host gap in the middle");
    Check(Common::IsHostContiguousRun(entries, 5, 7, IsDirect), "run after the gap");

    entries[4].raw = {0x1000, Unmapped};
    Check(!Common::IsHostContiguousRun(entries, 2, 6, IsDirect), "unmapped page in the middle");
    entries[4].raw = {0x1000, Debug};
    Check(!Common::IsHostContiguousRun(entries, 2, 6, IsDirect), "debug page in the middle");
    entries[0].raw = {0x1000, Unmapped};
    Check(!Common::IsHostContiguousRun(entries, 0, 0, IsDirect), "unmapped first page");
}

void TestSecureAppletMemory() {
    using Kernel::Board::Nintendo::Nx::SecureAppletMemory;
    using Status = SecureAppletMemory::Status;
    constexpr std::uint64_t base = 0x80000000;
    constexpr std::size_t size = 4 << 20;
    constexpr std::size_t page = 0x1000;

    SecureAppletMemory none;
    none.Initialize(0, 0, page);
    std::uint64_t out = 0;
    Check(none.Allocate(&out, page) == Status::OutOfMemory, "a board without the region fails");

    SecureAppletMemory region;
    region.Initialize(base, size, page);
    Check(region.Allocate(&out, 0x800) == Status::InvalidSize, "unaligned size");
    Check(region.Allocate(&out, size + page) == Status::OutOfMemory, "larger than the region");
    Check(!region.InUse(), "failed requests leave it free");

    out = 0;
    Check(region.Allocate(&out, size) == Status::Ok && out == base, "first applet gets the base");
    Check(region.InUse(), "lent");
    Check(region.Allocate(&out, page) == Status::OutOfMemory, "second applet is refused");

    Check(!region.Free(base + page, size), "free at the wrong address");
    Check(!region.Free(base, size + page), "free with a size past the region");
    Check(region.InUse(), "bad frees keep it lent");
    Check(region.Free(base, size), "matching free");
    Check(!region.InUse(), "returned");
    Check(!region.Free(base, size), "double free");

    Check(region.Allocate(&out, page) == Status::Ok && out == base, "smaller request reuses it");
    Check(region.Free(base, page), "free of the smaller request");
}

void TestSwkbdLayout() {
    using namespace Service::AM::Frontend;
    Check(SwkbdConfigLayoutForSize(0x3E0) == SwkbdConfigLayout::Old, "0x3E0 is the 1.0 layout");
    Check(SwkbdConfigLayoutForSize(0x400) == SwkbdConfigLayout::Old2, "0x400 is the 3.0 layout");
    Check(SwkbdConfigLayoutForSize(0x4C8) == SwkbdConfigLayout::New, "0x4C8 is the 6.0 layout");
    // The report: library revision 0x10000E with a 1224-byte (0x4C8) storage.
    Check(SwkbdConfigLayoutForSize(1224) == SwkbdConfigLayout::New, "the reported 1224 bytes");
    Check(SwkbdConfigLayoutForSize(0x500) == SwkbdConfigLayout::New, "larger reads as newest");
    Check(SwkbdConfigLayoutForSize(0x3DF) == SwkbdConfigLayout::Truncated, "short storage");
    Check(SwkbdConfigLayoutForSize(0x410) == SwkbdConfigLayout::Truncated, "between two layouts");
}

void TestUnbackedDevices() {
    using Service::Nvidia::IsUnbackedDevice;
    Check(IsUnbackedDevice("/dev/nverpt-ctrl"), "nverpt-ctrl");
    Check(IsUnbackedDevice("/dev/nvhost-prof-gpu"), "nvhost-prof-gpu keeps its old answer");
    Check(IsUnbackedDevice("/dev/nvhost-dbg-gpu"), "nvhost-dbg-gpu");
    Check(!IsUnbackedDevice("/dev/nvmap"), "emulated nodes open normally");
    Check(!IsUnbackedDevice("/dev/nverpt-ctrl2"), "exact names only");
    Check(!IsUnbackedDevice(""), "empty name");
}

void TestImageLimits() {
    using VideoCommon::FitsImageLimits;
    // The three reported images.
    Check(!FitsImageLimits({.is_3d = true, .width = 286331153, .height = 536940048, .depth = 1223}),
          "garbage 3D header");
    Check(!FitsImageLimits({.width = 65536, .height = 65536, .layers = 6}), "65536 cube");
    Check(!FitsImageLimits({.width = 1024, .height = 32768}), "32768-high 2D");

    Check(FitsImageLimits({.width = 16384, .height = 16384, .layers = 2048, .levels = 15}),
          "2D at every limit");
    Check(FitsImageLimits({.is_3d = true, .width = 2048, .height = 2048, .depth = 2048}),
          "3D at every limit");
    Check(!FitsImageLimits({.is_3d = true, .width = 4096, .height = 16, .depth = 16}),
          "3D past its own limit");
    Check(!FitsImageLimits({.width = 64, .height = 64, .layers = 2049}), "too many layers");
    Check(!FitsImageLimits({.width = 64, .height = 64, .levels = 16}), "too many levels");
    // 3D images carry no layers and 2D images no depth: the other field is ignored.
    Check(FitsImageLimits({.is_3d = true, .width = 8, .height = 8, .depth = 8, .layers = 9999}),
          "layers ignored on 3D");
    Check(FitsImageLimits({.width = 8, .height = 8, .depth = 9999}), "depth ignored on 2D");
    Check(FitsImageLimits({.width = 0, .height = 0, .layers = 0, .levels = 0}),
          "zero sizes are left to the backends");
}
} // namespace

int main() {
    TestPageRun();
    TestSecureAppletMemory();
    TestSwkbdLayout();
    TestUnbackedDevices();
    TestImageLimits();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("error-report helpers: all checks passed\n");
    return 0;
}
