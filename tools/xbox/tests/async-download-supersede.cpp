// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// A GPU write marked after an async download was committed must survive that download's pop:
// the pop may neither write its older bytes over guest memory nor clear the newer dirty mark.
// Replays BufferCache's commit/mark/pop range-set steps on the real range sets.
#undef NDEBUG
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <vector>
#include "common/range_sets.inc"

using DAddr = std::uint64_t;

struct Cache {
    std::vector<std::uint32_t> guest = std::vector<std::uint32_t>(64);
    std::vector<std::uint32_t> host = std::vector<std::uint32_t>(64);
    Common::RangeSet<DAddr> gpu_modified;
    Common::OverlapRangeSet<DAddr> async_downloads;
    struct Pending {
        DAddr address;
        std::vector<std::uint32_t> bytes;
    };
    std::deque<Pending> pending;
    std::deque<Common::RangeSet<DAddr>> superseded;
    bool supersede = true;

    // MarkWrittenBuffer, with the GPU write itself.
    void GpuWrite(DAddr word, std::uint32_t value) {
        host[word] = value;
        if (supersede && !superseded.empty()) {
            async_downloads.ForEachInRange(word * 4, 4, [&](DAddr start, DAddr end, int) {
                for (auto& set : superseded) {
                    set.Add(start, end - start);
                }
            });
        }
        gpu_modified.Add(word * 4, 4);
    }
    // CommitAsyncFlushesHigh: snapshot host words now, land them later.
    void Commit(DAddr word, std::size_t words) {
        async_downloads.Add(word * 4, words * 4);
        pending.push_back({word * 4, {host.begin() + word, host.begin() + word + words}});
        superseded.emplace_back();
    }
    // PopAsyncBuffers.
    void Pop() {
        const Pending download = pending.front();
        const Common::RangeSet<DAddr> stale = std::move(superseded.front());
        pending.pop_front();
        superseded.pop_front();
        const auto for_each_current = [&](DAddr start, DAddr end, auto&& func) {
            DAddr cursor = start;
            stale.ForEachInRange(start, end - start, [&](DAddr lower, DAddr upper) {
                if (lower > cursor) {
                    func(cursor, lower);
                }
                cursor = (std::max)(cursor, upper);
            });
            if (cursor < end) {
                func(cursor, end);
            }
        };
        const std::size_t size = download.bytes.size() * 4;
        async_downloads.ForEachInRange(download.address, size, [&](DAddr lo, DAddr hi, int) {
            for_each_current(lo, hi, [&](DAddr start, DAddr end) {
                std::memcpy(&guest[start / 4], &download.bytes[(start - download.address) / 4],
                            end - start);
            });
        });
        async_downloads.Subtract(download.address, size, [&](DAddr lo, DAddr hi) {
            for_each_current(lo, hi,
                             [&](DAddr start, DAddr end) { gpu_modified.Subtract(start, end - start); });
        });
    }
    bool Dirty(DAddr word) const {
        bool dirty = false;
        gpu_modified.ForEachInRange(word * 4, 4, [&](DAddr, DAddr) { dirty = true; });
        return dirty;
    }
};

// Frame N-1 culling result downloaded; frame N's culling rewrites the instance count before the
// pop lands. The macro read must either see the dirty mark (GPU path) or the new value.
void StaleDownloadAfterRewrite(bool supersede) {
    Cache cache;
    cache.supersede = supersede;
    cache.GpuWrite(9, 0);    // instance count culled to zero last frame
    cache.GpuWrite(8, 411);  // index count
    cache.Commit(8, 8);      // fence: download the record
    cache.GpuWrite(9, 5);    // this frame's culling: five visible instances
    cache.guest[9] = 77;     // sentinel: a stale pop would overwrite it with 0
    cache.Pop();
    const bool correct = cache.Dirty(9) || cache.guest[9] == 5;
    assert(correct == supersede);
    if (supersede) {
        assert(cache.Dirty(9) && cache.guest[9] == 77);
        assert(cache.guest[8] == 411 && !cache.Dirty(8)); // untouched neighbour still lands
    }
}

// A download with no later write still lands and clears its mark.
void PlainDownloadLands() {
    Cache cache;
    cache.GpuWrite(3, 7);
    cache.Commit(0, 16);
    cache.Pop();
    assert(cache.guest[3] == 7 && !cache.Dirty(3));
}

// Two downloads in flight over the same words, rewritten between them: only the newest data may
// reach guest memory, and the mark survives until the newest download pops.
void OverlappingDownloads() {
    Cache cache;
    cache.GpuWrite(4, 1);
    cache.Commit(4, 1);
    cache.GpuWrite(4, 2);
    cache.Commit(4, 1);
    cache.Pop();
    assert(cache.guest[4] != 1);
    assert(cache.Dirty(4));
    cache.Pop();
    assert(cache.guest[4] == 2 && !cache.Dirty(4));
}

// A rewrite after the newest download supersedes every in-flight one: the mark stays.
void RewriteAfterAllDownloads() {
    Cache cache;
    cache.GpuWrite(4, 1);
    cache.Commit(4, 1);
    cache.Commit(0, 8);
    cache.GpuWrite(4, 3);
    cache.Pop();
    cache.Pop();
    assert(cache.guest[4] != 1 && cache.Dirty(4));
    assert(!cache.Dirty(5));
}

int main() {
    StaleDownloadAfterRewrite(false);
    StaleDownloadAfterRewrite(true);
    PlainDownloadLands();
    OverlappingDownloads();
    RewriteAfterAllDownloads();
    std::cout << "async download supersede: 5 cases passed\n";
}
