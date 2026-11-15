// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Run from the repo root (suite gate unit-view-cast):
//   powershell -File tools\xbox\tests\renderer-suite\run-unit.ps1 -Name view-cast
//
// Checks ViewCast (d3d12_view_cast.h), how a D3D12 view reads or renders an image of another
// typeless family, and ReinterpretSync (d3d12_reinterpreted_copies.h), which keeps the image
// and its copies in other families in step. Both are pure: no device needed.
#include <array>
#include <cstdio>
#include <cstdlib>

#include "video_core/renderer_d3d12/d3d12_reinterpreted_copies.h"
#include "video_core/renderer_d3d12/d3d12_view_cast.h"

namespace {
void Check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        std::exit(1);
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

using namespace D3D12;
using ViewCast::Plan;
using ViewCast::Source;

constexpr DXGI_FORMAT RGBA = DXGI_FORMAT_R8G8B8A8_TYPELESS;
constexpr DXGI_FORMAT BGRA = DXGI_FORMAT_B8G8R8A8_TYPELESS;
constexpr DXGI_FORMAT R11G11B10 = DXGI_FORMAT_R11G11B10_FLOAT; // its own family
constexpr DXGI_FORMAT R32 = DXGI_FORMAT_R32_TYPELESS;

ViewCast::Request Srv(DXGI_FORMAT format, DXGI_FORMAT family, DXGI_FORMAT resource,
                      std::span<const DXGI_FORMAT> copies = {}, bool copyable = true) {
    return {format, family, resource, copyable, copies};
}

void TestPlans() {
    // Same family: the image itself, whatever the copies.
    CHECK((ViewCast::PlanSrv(Srv(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, RGBA, RGBA)) ==
           Plan{Source::Image, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}));

    // RGBA view of a BGRA image (and back): the image, R and B swapped, no copy, sRGB kept.
    CHECK((ViewCast::PlanSrv(Srv(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, RGBA, BGRA)) ==
           Plan{Source::Image, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_UNKNOWN, true}));
    CHECK((ViewCast::PlanSrv(Srv(DXGI_FORMAT_B8G8R8A8_UNORM, BGRA, RGBA)) ==
           Plan{Source::Image, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, true}));
    // Integer RGBA has no BGRA twin: a copy.
    CHECK((ViewCast::PlanSrv(Srv(DXGI_FORMAT_R8G8B8A8_UINT, RGBA, BGRA)) ==
           Plan{Source::Copy, DXGI_FORMAT_R8G8B8A8_UINT, RGBA}));

    // The captured case: R11G11B10 image, rendered as BGRA (copy in BGRA), read as RGBA sRGB.
    // The existing BGRA copy serves the read with R and B swapped; no second copy.
    const std::array bgra_copy{BGRA};
    CHECK((ViewCast::PlanSrv(Srv(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, RGBA, R11G11B10, bgra_copy)) ==
           Plan{Source::Copy, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, BGRA, true}));
    // A copy in the view's own family wins over the twin.
    const std::array both{BGRA, RGBA};
    CHECK((ViewCast::PlanSrv(Srv(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, RGBA, R11G11B10, both)) ==
           Plan{Source::Copy, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, RGBA}));
    // No copy yet: a new one in the view's family.
    CHECK((ViewCast::PlanSrv(Srv(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, RGBA, R11G11B10)) ==
           Plan{Source::Copy, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, RGBA}));
    // A copy in an unrelated family is no help.
    const std::array r32_copy{R32};
    CHECK((ViewCast::PlanSrv(Srv(DXGI_FORMAT_R8G8B8A8_UNORM, RGBA, R11G11B10, r32_copy)) ==
           Plan{Source::Copy, DXGI_FORMAT_R8G8B8A8_UNORM, RGBA}));

    // Not copyable (block-compressed, depth, other texel size): the image's own format.
    CHECK((ViewCast::PlanSrv(Srv(DXGI_FORMAT_R8G8B8A8_UNORM, RGBA, R11G11B10, {}, false)) ==
           Plan{}));
    // ...but the twin still reads without a copy.
    CHECK((ViewCast::PlanSrv(Srv(DXGI_FORMAT_R8G8B8A8_UNORM, RGBA, BGRA, {}, false)).swap_red_blue));

    // Render targets: never swizzled, so a copy in exactly the view's family.
    CHECK((ViewCast::PlanRtv(Srv(DXGI_FORMAT_B8G8R8A8_UNORM, BGRA, BGRA)) ==
           Plan{Source::Image, DXGI_FORMAT_B8G8R8A8_UNORM}));
    CHECK((ViewCast::PlanRtv(Srv(DXGI_FORMAT_R8G8B8A8_UNORM, RGBA, BGRA, bgra_copy)) ==
           Plan{Source::Copy, DXGI_FORMAT_R8G8B8A8_UNORM, RGBA}));
    CHECK((ViewCast::PlanRtv(Srv(DXGI_FORMAT_R8G8B8A8_UNORM, RGBA, R11G11B10, {}, false)) ==
           Plan{}));

    // Twins are each other's, and only the four 8-bit UNORM formats have one.
    for (const DXGI_FORMAT format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                     DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        CHECK(ViewCast::RedBlueTwin(ViewCast::RedBlueTwin(format).format).format == format);
    }
    CHECK(ViewCast::RedBlueTwin(DXGI_FORMAT_R8G8B8A8_SNORM).format == DXGI_FORMAT_UNKNOWN);
    CHECK(ViewCast::RedBlueTwin(DXGI_FORMAT_B8G8R8X8_UNORM).format == DXGI_FORMAT_UNKNOWN);
}

void TestSync() {
    ReinterpretSync sync;
    CHECK(sync.Count() == 0 && !sync.Ahead());
    const std::size_t bgra = sync.Add();
    const std::size_t rgba = sync.Add();
    CHECK(bgra == 0 && rgba == 1);
    // A new copy holds nothing yet, whatever the image's version.
    CHECK(sync.Stale(bgra, 0) && sync.Stale(rgba, 7));

    // Refreshed copies are current until the image changes.
    sync.Refreshed(bgra, 3);
    CHECK(!sync.Stale(bgra, 3) && sync.Stale(bgra, 4));

    // Rendering to one copy: it is never stale, and the other one must wait for a write-back.
    sync.Rendered(bgra);
    CHECK(sync.Ahead() == bgra);
    CHECK(!sync.Stale(bgra, 99));
    CHECK(!sync.WriteBackBefore(bgra));
    CHECK(sync.WriteBackBefore(rgba) == bgra);

    // The write-back bumps the image's version: the written copy matches, the other is stale.
    CHECK(sync.TakeAhead() == bgra);
    CHECK(!sync.Ahead() && !sync.WriteBackBefore(rgba));
    sync.Refreshed(bgra, 4);
    CHECK(!sync.Stale(bgra, 4) && sync.Stale(rgba, 4));
    CHECK(!sync.TakeAhead());

    // A bounded set: no fifth family on one image.
    sync.Add();
    sync.Add();
    CHECK(sync.Full() && sync.Count() == ReinterpretSync::MAX_COPIES);

    // Moving out resets (Image's move assignment exchanges it).
    ReinterpretSync moved = std::exchange(sync, {});
    CHECK(moved.Count() == ReinterpretSync::MAX_COPIES && sync.Count() == 0);
}
} // namespace

int main() {
    TestPlans();
    TestSync();
    std::puts("view-cast: all checks passed");
    return 0;
}
