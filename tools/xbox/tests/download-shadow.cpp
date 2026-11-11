// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Download merge gate. Runs random uploads, CPU and GPU writes, recorded and landing downloads and
// buffer deletions through DownloadShadow and through a plain model that copies the host contents
// per download. Guest memory and the words kept for the CPU must match after every step.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

#include "video_core/buffer_cache/download_shadow.h"

namespace {

constexpr u64 PAGE = 4096;
constexpr u64 PAGES = 48;
constexpr u64 MEMORY = PAGES * PAGE;

[[noreturn]] void Fail(const char* what, int seed, int step) {
    std::printf("FAIL %s, seed %d, step %d\n", what, seed, step);
    std::exit(1);
}

struct Memory {
    std::vector<u8> bytes = std::vector<u8>(MEMORY);

    template <class T>
    T* GetPointer(u64 address) {
        return reinterpret_cast<T*>(bytes.data() + address);
    }
    void ReadBlockUnsafe(u64 address, void* out, u64 size) {
        std::memcpy(out, bytes.data() + address, size);
    }
    void WriteBlockUnsafe(u64 address, const void* in, u64 size) {
        std::memcpy(bytes.data() + address, in, size);
    }
};

/// A snapshot of the shadow per download, updated by landings: what DownloadShadow must behave as.
class PlainModel {
public:
    struct Copy {
        u64 address;
        std::vector<u8> baseline; // Empty: lands whole.
    };

    bool Snapshot(u64 address, u64 size, Memory& memory, std::vector<u8>& out) {
        out.resize(size);
        for (u64 done = 0; done < size;) {
            const u64 current = address + done;
            const u64 offset = current % PAGE;
            const u64 count = (std::min)(PAGE - offset, size - done);
            auto& page = pages[current / PAGE];
            if (page.empty()) {
                page.resize(PAGE);
                memory.ReadBlockUnsafe(current - offset, page.data(), PAGE);
            }
            std::memcpy(out.data() + done, page.data() + offset, count);
            done += count;
        }
        return true;
    }
    void Store(u64 address, const u8* data, u64 size) {
        for (u64 done = 0; done < size;) {
            const u64 current = address + done;
            const u64 offset = current % PAGE;
            const u64 count = (std::min)(PAGE - offset, size - done);
            if (const auto it = pages.find(current / PAGE); it != pages.end()) {
                std::memcpy(it->second.data() + offset, data + done, count);
            }
            done += count;
        }
    }
    void Erase(u64 address, u64 size) {
        for (u64 page = address / PAGE; page < (address + size + PAGE - 1) / PAGE; ++page) {
            pages.erase(page);
        }
    }

    std::unordered_map<u64, std::vector<u8>> pages;
};

struct Download {
    u64 address;
    std::vector<u8> incoming;
    std::vector<u8> baseline;
    std::vector<std::pair<u64, u64>> parts; // Sub-ranges still to land.
};

struct Group {
    u64 record;
    std::vector<Download> copies;
};

void UpdatePlainBaselines(std::deque<Group>& groups, u64 address, const u8* data, u64 size) {
    for (Group& group : groups) {
        for (Download& copy : group.copies) {
            const u64 lower = (std::max)(address, copy.address);
            const u64 upper = (std::min)(address + size, copy.address + copy.incoming.size());
            if (lower < upper) {
                std::memcpy(copy.baseline.data() + (lower - copy.address), data + (lower - address),
                            upper - lower);
            }
        }
    }
}

void PlainLand(PlainModel& plain, Memory& memory, std::deque<Group>& groups, u64 start, u64 size,
               const u8* incoming, const u8* baseline, std::set<u64>& kept) {
    for (u64 offset = 0; offset < size; offset += 4) {
        u32 from_gpu;
        u32 before;
        u32 guest;
        std::memcpy(&from_gpu, incoming + offset, 4);
        std::memcpy(&before, baseline + offset, 4);
        std::memcpy(&guest, memory.bytes.data() + start + offset, 4);
        if (from_gpu != before) {
            std::memcpy(memory.bytes.data() + start + offset, &from_gpu, 4);
            plain.Store(start + offset, incoming + offset, 4);
            UpdatePlainBaselines(groups, start + offset, incoming + offset, 4);
        } else if (from_gpu != guest) {
            kept.insert(start + offset);
        }
    }
}

void RunSeed(int seed, int steps) {
    std::mt19937 random(static_cast<u32>(seed));
    const auto below = [&](u64 limit) { return std::uniform_int_distribution<u64>(0, limit - 1)(random); };
    // Few distinct values, so writes often repeat what a word already holds.
    const auto value = [&]() { return static_cast<u32>(below(4)); };

    Memory new_guest;
    Memory plain_guest;
    std::vector<u8> host(MEMORY);
    VideoCommon::DownloadShadow shadow;
    PlainModel plain;
    std::deque<Group> plain_groups;

    const auto random_range = [&](u64& address, u64& size) {
        const u64 kind = below(10);
        const u64 words = kind < 5 ? 1 + below(64) : kind < 9 ? 1 + below(1024) : 1 + below(3072);
        address = below(MEMORY / 4 - words) * 4;
        size = words * 4;
    };

    // Both models must see the same uploads; a kept word goes up like any other CPU write.
    const auto upload = [&](u64 address, u64 size) {
        std::memcpy(host.data() + address, new_guest.bytes.data() + address, size);
        shadow.HostWritten(address, new_guest.bytes.data() + address, size);
        plain.Store(address, plain_guest.bytes.data() + address, size);
    };

    const auto check = [&](int step) {
        if (new_guest.bytes != plain_guest.bytes) {
            for (u64 at = 0; at < MEMORY; ++at) {
                if (new_guest.bytes[at] != plain_guest.bytes[at]) {
                    std::printf("first difference at %llx\n", static_cast<unsigned long long>(at));
                    break;
                }
            }
            Fail("guest memory differs", seed, step);
        }
    };

    const auto land = [&](u64 address, u64 size, const u8* incoming, u64 record,
                          const u8* baseline, int step) {
        std::set<u64> new_kept;
        std::set<u64> plain_kept;
        shadow.Merge(
            address, size, incoming, record, new_guest, [](u64, u64) {},
            [&](u64 at, u64 length) {
                for (u64 word = 0; word < length; word += 4) {
                    new_kept.insert(at + word);
                }
            });
        PlainLand(plain, plain_guest, plain_groups, address, size, incoming, baseline, plain_kept);
        if (new_kept != plain_kept) {
            Fail("kept words differ", seed, step);
        }
        check(step);
        for (const u64 word : new_kept) {
            upload(word, 4);
        }
    };

    for (int step = 0; step < steps; ++step) {
        u64 address;
        u64 size;
        const u64 op = below(100);
        if (op < 22) {
            // CPU write, not uploaded yet.
            random_range(address, size);
            size = (std::min<u64>)(size, 64);
            for (u64 at = 0; at < size; at += 4) {
                const u32 word = value();
                std::memcpy(new_guest.bytes.data() + address + at, &word, 4);
                std::memcpy(plain_guest.bytes.data() + address + at, &word, 4);
            }
        } else if (op < 38) {
            random_range(address, size);
            upload(address, size);
        } else if (op < 58) {
            // The GPU writes some words of a range.
            random_range(address, size);
            for (u64 at = 0; at < size; at += 4) {
                if (below(3) == 0) {
                    const u32 word = value();
                    std::memcpy(host.data() + address + at, &word, 4);
                }
            }
        } else if (op < 72) {
            Group group;
            group.record = shadow.BeginRecord();
            const u64 count = 1 + below(3);
            for (u64 index = 0; index < count; ++index) {
                random_range(address, size);
                Download copy;
                copy.address = address;
                copy.incoming.assign(host.begin() + static_cast<std::ptrdiff_t>(address),
                                     host.begin() + static_cast<std::ptrdiff_t>(address + size));
                if (!shadow.Track(address, size, group.record, new_guest)) {
                    Fail("shadow full", seed, step);
                }
                plain.Snapshot(address, size, plain_guest, copy.baseline);
                if (below(3) == 0 && size >= 16) {
                    // Lands in two parts, as when a range is superseded in the middle.
                    const u64 middle = (1 + below(size / 4 - 1)) * 4;
                    copy.parts = {{0, middle}, {middle, size - middle}};
                } else {
                    copy.parts = {{0, size}};
                }
                group.copies.push_back(std::move(copy));
            }
            plain_groups.push_back(std::move(group));
        } else if (op < 90) {
            if (plain_groups.empty()) {
                continue;
            }
            Group& group = plain_groups.front();
            for (Download& copy : group.copies) {
                for (const auto& [offset, length] : copy.parts) {
                    land(copy.address + offset, length, copy.incoming.data() + offset, group.record,
                         copy.baseline.data() + offset, step);
                }
            }
            const u64 record = group.record;
            plain_groups.pop_front();
            shadow.Retire(record);
        } else if (op < 96) {
            // A download that lands right after it was read.
            random_range(address, size);
            std::vector<u8> incoming(host.begin() + static_cast<std::ptrdiff_t>(address),
                                     host.begin() + static_cast<std::ptrdiff_t>(address + size));
            if (below(4) == 0) {
                // Lands whole: merging off, or a range that is not word aligned.
                new_guest.WriteBlockUnsafe(address, incoming.data(), size);
                plain_guest.WriteBlockUnsafe(address, incoming.data(), size);
                shadow.Landed(address, incoming.data(), size);
                plain.Store(address, incoming.data(), size);
                UpdatePlainBaselines(plain_groups, address, incoming.data(), size);
                check(step);
                continue;
            }
            std::vector<u8> baseline;
            if (!shadow.Track(address, size, 0, new_guest)) {
                Fail("shadow full", seed, step);
            }
            plain.Snapshot(address, size, plain_guest, baseline);
            land(address, size, incoming.data(), VideoCommon::DownloadShadow::LATEST,
                 baseline.data(), step);
        } else {
            random_range(address, size);
            shadow.Erase(address, size);
            plain.Erase(address, size);
        }
        check(step);
    }
    while (!plain_groups.empty()) {
        Group& group = plain_groups.front();
        for (Download& copy : group.copies) {
            land(copy.address, copy.incoming.size(), copy.incoming.data(), group.record,
                 copy.baseline.data(), steps);
        }
        const u64 record = group.record;
        plain_groups.pop_front();
        shadow.Retire(record);
    }
    if (shadow.VersionPages() != 0) {
        Fail("versions left after every download landed", seed, steps);
    }
}

/// Unmapped guest pages land through WriteBlockUnsafe and do not crash.
struct HalfMappedMemory : Memory {
    template <class T>
    T* GetPointer(u64 address) {
        return address < PAGE ? nullptr : Memory::GetPointer<T>(address);
    }
};

void UnmappedPage() {
    HalfMappedMemory memory;
    VideoCommon::DownloadShadow shadow;
    const u64 record = shadow.BeginRecord();
    if (!shadow.Track(0, PAGE * 2, record, memory)) {
        Fail("shadow full", 0, 0);
    }
    std::vector<u8> incoming(PAGE * 2, 7);
    u64 landed = 0;
    shadow.Merge(
        0, PAGE * 2, incoming.data(), record, memory, [&](u64, u64 size) { landed += size; },
        [](u64, u64) { Fail("kept on unmapped page", 0, 0); });
    if (landed != PAGE * 2) {
        Fail("unmapped page did not land", 0, 0);
    }
}

} // namespace

int main() {
    UnmappedPage();
    for (int seed = 1; seed <= 300; ++seed) {
        RunSeed(seed, 1500);
    }
    std::puts("download shadow: guest memory and kept words match over 300 seeds");
    return 0;
}
