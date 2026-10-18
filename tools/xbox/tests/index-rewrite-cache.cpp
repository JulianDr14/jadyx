// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_index_rewrite_cache.h"

using D3D12::IndexRewriteCache;

namespace {
void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

IndexRewriteCache::Key KeyAt(std::uint64_t offset, std::uint64_t buffer = 1) {
    return {.buffer_id = buffer, .offset = offset, .count = 6, .format = 2, .topology = 7};
}

/// What BufferCacheRuntime does per draw, minus the GPU: reuse while the generation holds, else
/// compare the guest bytes, else rewrite (quads to triangles).
struct Draws {
    IndexRewriteCache cache;
    std::vector<std::uint32_t> scratch;
    int rewrites = 0, reuses = 0, rechecks = 0;

    const IndexRewriteCache::Entry& Draw(const IndexRewriteCache::Key& key,
                                         const std::vector<std::uint8_t>& guest,
                                         std::uint64_t generation) {
        IndexRewriteCache::Entry* entry = cache.Find(key);
        if (entry && entry->generation == generation) {
            ++reuses;
            return *entry;
        }
        if (entry && entry->source == guest) {
            entry->generation = generation;
            ++rechecks;
            return *entry;
        }
        Rewrite(guest, scratch);
        entry = &cache.Obtain(key);
        entry->generation = generation;
        entry->source.assign(guest.begin(), guest.end());
        entry->SetRewrite(scratch, false);
        cache.Account();
        ++rewrites;
        return *entry;
    }

    static void Rewrite(const std::vector<std::uint8_t>& guest, std::vector<std::uint32_t>& out) {
        out.clear();
        const std::size_t count = guest.size() / 2;
        const auto at = [&](std::size_t i) {
            std::uint16_t v;
            std::memcpy(&v, guest.data() + i * 2, 2);
            return std::uint32_t{v};
        };
        for (std::size_t quad = 0; quad + 4 <= count; quad += 4) {
            for (const std::size_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
                out.push_back(at(quad + i));
            }
        }
    }
};

std::vector<std::uint8_t> QuadIndices(std::uint32_t quads, std::uint16_t salt) {
    std::vector<std::uint8_t> bytes(quads * 4 * 2);
    for (std::uint32_t i = 0; i < quads * 4; ++i) {
        const auto v = static_cast<std::uint16_t>(i + salt);
        std::memcpy(bytes.data() + i * 2, &v, 2);
    }
    return bytes;
}
} // namespace

int main() {
    // Narrowing and the upload state a new rewrite resets.
    {
        IndexRewriteCache::Entry entry;
        entry.upload_tick = 5;
        const std::uint32_t list[]{1, 0x1234, 0xFFFF};
        entry.SetRewrite(list, false);
        Check(entry.count == 3 && entry.rewrite.size() == 6 && !entry.wide, "narrow size");
        std::uint16_t second;
        std::memcpy(&second, entry.rewrite.data() + 2, 2);
        Check(second == 0x1234, "narrow value");
        Check(entry.upload_tick == ~std::uint64_t{}, "new rewrite drops the old upload");
        entry.SetRewrite(list, true);
        Check(entry.rewrite.size() == 12 && entry.wide, "wide size");
    }

    // Lookups, recency and eviction by count.
    {
        IndexRewriteCache cache;
        Check(cache.Find(KeyAt(0)) == nullptr, "empty miss");
        cache.Obtain(KeyAt(0)).count = 1;
        cache.Account();
        Check(cache.Find(KeyAt(0)) && cache.Find(KeyAt(0))->count == 1, "hit");
        Check(!cache.Find(KeyAt(0, 2)), "another buffer misses");
        auto other_format = KeyAt(0);
        other_format.format = 4;
        Check(!cache.Find(other_format), "another format misses");
        for (std::uint64_t i = 1; i <= IndexRewriteCache::MAX_ENTRIES; ++i) {
            cache.Obtain(KeyAt(i * 2)).count = 2;
            cache.Account();
            if (i == 10) {
                Check(cache.Find(KeyAt(0)) != nullptr, "touch the first one");
            }
        }
        Check(cache.Size() == IndexRewriteCache::MAX_ENTRIES, "count bound");
        Check(cache.Find(KeyAt(0)) != nullptr, "recently used survives");
        Check(cache.Find(KeyAt(2)) == nullptr, "least recently used is evicted");
    }

    // Eviction by bytes; obtaining an existing entry recharges it instead of adding twice.
    {
        IndexRewriteCache cache{3 * (sizeof(IndexRewriteCache::Entry) + 1000)};
        for (std::uint64_t i = 0; i < 3; ++i) {
            auto& entry = cache.Obtain(KeyAt(i));
            entry.source.resize(1000);
            cache.Account();
        }
        Check(cache.Size() == 3, "three fit");
        auto& again = cache.Obtain(KeyAt(1));
        again.source.resize(1000);
        cache.Account();
        Check(cache.Size() == 3, "recharging does not evict");
        auto& big = cache.Obtain(KeyAt(9));
        big.source.resize(1000);
        cache.Account();
        Check(cache.Size() == 3 && !cache.Find(KeyAt(0)), "bytes evict the oldest");
        auto& huge = cache.Obtain(KeyAt(10));
        huge.source.resize(10000);
        cache.Account();
        Check(cache.Size() == 1 && cache.Find(KeyAt(10)), "an oversized entry stays alone");
    }

    // The validation rules.
    {
        Draws draws;
        auto guest = QuadIndices(4, 0);
        const auto& first = draws.Draw(KeyAt(64), guest, 1);
        Check(first.count == 24 && draws.rewrites == 1, "first draw rewrites");
        draws.Draw(KeyAt(64), guest, 1);
        Check(draws.reuses == 1, "same generation reuses");
        draws.Draw(KeyAt(64), guest, 2);
        Check(draws.rechecks == 1 && draws.rewrites == 1, "same bytes after a write recheck");
        draws.Draw(KeyAt(64), guest, 2);
        Check(draws.reuses == 2, "recheck renews the generation");
        guest = QuadIndices(4, 100);
        const auto& changed = draws.Draw(KeyAt(64), guest, 3);
        std::uint16_t value;
        std::memcpy(&value, changed.rewrite.data(), 2);
        Check(draws.rewrites == 2 && value == 100, "new bytes rewrite");
        Check(draws.cache.Size() == 1, "a rewrite of the same range reuses its entry");
    }

    // Cost per frame of ~1850 rewritten draws of 150 quads: rewriting all of them every frame
    // (before) against reusing them (a steady frame) and rechecking them (a frame where each
    // buffer was written elsewhere).
    {
        constexpr int DRAWS = 1850;
        constexpr int FRAMES = 30;
        std::vector<std::vector<std::uint8_t>> guests;
        for (int i = 0; i < DRAWS; ++i) {
            guests.push_back(QuadIndices(150, static_cast<std::uint16_t>(i)));
        }
        std::vector<std::uint32_t> scratch;
        std::vector<std::uint8_t> upload(150 * 6 * 2);
        using Clock = std::chrono::steady_clock;
        const auto ms_per_frame = [](Clock::duration d) {
            return std::chrono::duration<double, std::milli>(d).count() / FRAMES;
        };
        auto start = Clock::now();
        for (int frame = 0; frame < FRAMES; ++frame) {
            for (int i = 0; i < DRAWS; ++i) {
                Draws::Rewrite(guests[i], scratch);
                for (std::size_t j = 0; j < scratch.size(); ++j) {
                    const auto narrow = static_cast<std::uint16_t>(scratch[j]);
                    std::memcpy(upload.data() + j * 2, &narrow, 2);
                }
            }
        }
        const double before = ms_per_frame(Clock::now() - start);

        Draws draws;
        for (int i = 0; i < DRAWS; ++i) {
            draws.Draw(KeyAt(static_cast<std::uint64_t>(i) * 4096), guests[i], 1);
        }
        start = Clock::now();
        for (int frame = 0; frame < FRAMES; ++frame) {
            for (int i = 0; i < DRAWS; ++i) {
                const auto& entry = draws.Draw(KeyAt(static_cast<std::uint64_t>(i) * 4096),
                                               guests[i], 1);
                std::memcpy(upload.data(), entry.rewrite.data(), entry.rewrite.size());
            }
        }
        const double reused = ms_per_frame(Clock::now() - start);
        start = Clock::now();
        for (int frame = 0; frame < FRAMES; ++frame) {
            for (int i = 0; i < DRAWS; ++i) {
                const auto& entry = draws.Draw(KeyAt(static_cast<std::uint64_t>(i) * 4096),
                                               guests[i], 2 + frame);
                std::memcpy(upload.data(), entry.rewrite.data(), entry.rewrite.size());
            }
        }
        const double rechecked = ms_per_frame(Clock::now() - start);
        Check(draws.rewrites == DRAWS, "the benchmark never rewrites twice");
        std::printf("%d draws/frame: rewrite %.3f ms, reuse %.3f ms, recheck %.3f ms\n", DRAWS,
                    before, reused, rechecked);
    }

    std::puts("index rewrite cache: narrowing, LRU, byte budget, validation PASS");
}
