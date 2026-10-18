// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// The rule that lets a Fermi2D blit skip uploading its destination: the blit must rewrite every
// texel of the target. Every texel it leaves alone would show stale contents instead of guest data.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <utility>
#include <vector>
#include "video_core/texture_cache/blit_coverage.h"

using VideoCommon::BlitRect;
using VideoCommon::BlitRewritesTarget;
using VideoCommon::CoversWholeTarget;
using VideoCommon::IsUnscaledBlit;

namespace {
void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

bool SameExtents(const BlitRect& src, const BlitRect& dst) {
    return src.x1 - src.x0 == dst.x1 - dst.x0 && src.y1 - src.y0 == dst.y1 - dst.y0;
}

/// Brute force over the D3D12 backend's two ways of drawing a blit. Same signed extents are a
/// CopyTextureRegion, valid only with the source box inside its level; anything else is a
/// triangle over the destination's bounding box. Returns whether every target texel was written.
bool BackendWritesEveryTexel(const BlitRect& src, const BlitRect& dst, std::int32_t src_width,
                             std::int32_t src_height, std::int32_t width, std::int32_t height) {
    std::vector<bool> written(static_cast<size_t>(width * height));
    const auto fill = [&](std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1) {
        for (std::int32_t y = std::max(y0, 0); y < std::min(y1, height); ++y) {
            for (std::int32_t x = std::max(x0, 0); x < std::min(x1, width); ++x) {
                written[static_cast<size_t>(y * width + x)] = true;
            }
        }
    };
    if (SameExtents(src, dst)) {
        if (src.x1 <= src.x0 || src.y1 <= src.y0 || src.x0 < 0 || src.y0 < 0 ||
            src.x1 > src_width || src.y1 > src_height) {
            return false; // an invalid copy box guarantees nothing
        }
        fill(dst.x0, dst.y0, dst.x1, dst.y1);
    } else {
        if (src.x1 == src.x0 || src.y1 == src.y0) {
            return false; // nothing to sample
        }
        fill(std::min(dst.x0, dst.x1), std::min(dst.y0, dst.y1), std::max(dst.x0, dst.x1),
             std::max(dst.y0, dst.y1));
    }
    return std::find(written.begin(), written.end(), false) == written.end();
}
} // namespace

int main() {
    Check(IsUnscaledBlit({0, 0, 64, 32}, {0, 0, 64, 32}, 64, 32), "plain copy");
    Check(IsUnscaledBlit({8, 8, 72, 40}, {0, 0, 64, 32}, 128, 128), "offset source");
    Check(!IsUnscaledBlit({0, 0, 64, 32}, {0, 0, 128, 64}, 64, 32), "upscale");
    Check(!IsUnscaledBlit({64, 0, 0, 32}, {0, 0, 64, 32}, 64, 32), "horizontal flip");
    Check(!IsUnscaledBlit({0, 0, 64, 32}, {64, 0, 0, 32}, 64, 32), "flipped destination");
    Check(!IsUnscaledBlit({0, 0, 65, 32}, {0, 0, 65, 32}, 64, 32), "source past its level");
    Check(!IsUnscaledBlit({-1, 0, 63, 32}, {0, 0, 64, 32}, 64, 32), "source before its level");
    Check(!IsUnscaledBlit({0, 0, 0, 32}, {0, 0, 0, 32}, 64, 32), "empty");
    Check(CoversWholeTarget({0, 0, 64, 32}, 64, 32), "whole target");
    Check(!CoversWholeTarget({0, 0, 63, 32}, 64, 32), "one column short");
    Check(!CoversWholeTarget({1, 0, 65, 32}, 64, 32), "shifted");
    Check(!CoversWholeTarget({-1, -1, 65, 33}, 64, 32), "past the edges is refused");

    Check(BlitRewritesTarget({0, 0, 64, 32}, {0, 0, 64, 32}, 64, 32, 64, 32), "exact copy");
    Check(BlitRewritesTarget({0, 0, 32, 16}, {0, 0, 64, 32}, 32, 16, 64, 32), "upscale draws");
    Check(BlitRewritesTarget({0, 0, 128, 64}, {0, 0, 64, 32}, 128, 64, 64, 32), "downscale");
    Check(BlitRewritesTarget({64, 0, 0, 32}, {0, 0, 64, 32}, 64, 32, 64, 32),
          "mirrored source draws");
    Check(BlitRewritesTarget({0, 0, 40, 20}, {0, 0, 64, 32}, 16, 16, 64, 32),
          "scaled source past its level is clamped");
    Check(!BlitRewritesTarget({8, 0, 72, 32}, {0, 0, 64, 32}, 64, 32, 64, 32),
          "copy reading past its level");
    Check(!BlitRewritesTarget({0, 0, 64, 32}, {0, 0, 64, 31}, 64, 32, 64, 32), "partial target");
    Check(!BlitRewritesTarget({0, 0, 64, 32}, {64, 0, 0, 32}, 64, 32, 64, 32), "mirrored target");
    Check(!BlitRewritesTarget({0, 0, 0, 32}, {0, 0, 64, 32}, 64, 32, 64, 32), "empty source");

    // Never claims a rewrite that the backend model denies.
    std::mt19937 rng{99};
    int copies = 0, draws = 0;
    for (int i = 0; i < 300000; ++i) {
        const std::int32_t width = 1 + rng() % 16, height = 1 + rng() % 16;
        const std::int32_t src_width = 1 + rng() % 24, src_height = 1 + rng() % 24;
        // Near the edges, where an off-by-one would lose a row or a column.
        const auto near = [&](std::int32_t edge) {
            return edge + static_cast<std::int32_t>(rng() % 3) - 1;
        };
        // Half of them whole, so both paths get claimed often enough to be worth checking.
        const BlitRect dst = rng() % 2 ? BlitRect{0, 0, width, height}
                                       : BlitRect{near(0), near(0), near(width), near(height)};
        BlitRect src;
        if (rng() % 2) { // same extents, placed anywhere around the source level
            src.x0 = static_cast<std::int32_t>(rng() % 30) - 4;
            src.y0 = static_cast<std::int32_t>(rng() % 30) - 4;
            src.x1 = src.x0 + (dst.x1 - dst.x0);
            src.y1 = src.y0 + (dst.y1 - dst.y0);
        } else {
            src = {near(0), near(0), near(src_width), near(src_height)};
            if (rng() % 4 == 0) {
                std::swap(src.x0, src.x1);
            }
        }
        if (BlitRewritesTarget(src, dst, src_width, src_height, width, height)) {
            Check(BackendWritesEveryTexel(src, dst, src_width, src_height, width, height),
                  "claimed rewrite leaves texels");
            ++(SameExtents(src, dst) ? copies : draws);
        }
    }
    Check(copies > 2000 && draws > 2000, "both paths exercised");
    std::printf("PASS blit-coverage: 300000 random blits; %d whole copies and %d whole draws "
                "verified texel by texel\n",
                copies, draws);
    return 0;
}
