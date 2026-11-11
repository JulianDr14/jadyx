// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include "common/common_types.h"

namespace VideoCommon {

/// What the host buffers hold where downloads happen, as far as uploads and landed downloads tell.
/// A download compared with this shows which words the GPU changed. A shader binding is marked
/// as written whole, but on the console the GPU only stores the words the shader writes: the rest
/// of the guest memory keeps whatever the CPU put there meanwhile.
///
/// A download compares against the contents when it was recorded. Those only differ from the
/// current page if an upload touched it since, so nothing is copied at record time: an upload over
/// a page with a download in flight first saves the page as a version for that download.
/// Callers hold the buffer cache mutex.
class DownloadShadow {
public:
    /// Compares against the current contents: for a download landing right after it was read.
    static constexpr u64 LATEST = ~u64{0};

    /// Numbers a group of downloads recorded together. Groups land in this order.
    [[nodiscard]] u64 BeginRecord() {
        return ++newest_record;
    }

    /// Prepares the range for a download of that group (0: one that lands at once). Pages seen for
    /// the first time start from guest memory. False once the shadow is full: pages without an
    /// entry land whole.
    template <class DeviceMemory>
    [[nodiscard]] bool Track(u64 address, u64 size, u64 record, DeviceMemory& device_memory) {
        const u64 end = (address + size + PAGE_MASK) >> PAGE_BITS;
        for (u64 key = address >> PAGE_BITS; key < end; ++key) {
            Entry* entry = Find(key);
            if (!entry) {
                if (entries.size() > MAX_PAGES) {
                    return false;
                }
                auto created = std::make_unique<Entry>();
                device_memory.ReadBlockUnsafe(key << PAGE_BITS, created->data.data(), PAGE_SIZE);
                entry = created.get();
                entries.emplace(key, std::move(created));
                last_key = key;
                last_entry = entry;
            } else if (entry->host_gone) {
                Preserve(*entry, key);
                device_memory.ReadBlockUnsafe(key << PAGE_BITS, entry->data.data(), PAGE_SIZE);
                entry->host_gone = false;
            }
            entry->last_record = (std::max)(entry->last_record, record);
        }
        return true;
    }

    /// An upload put these bytes on the host. Downloads in flight did not see them.
    void HostWritten(u64 address, const u8* data, u64 size) {
        if (entries.empty()) {
            return;
        }
        ForEachPage(address, size, [&](u64 key, u64 offset, u64 done, u64 count) {
            if (Entry* const entry = Find(key)) {
                Preserve(*entry, key);
                if (!entry->host_gone) {
                    std::memcpy(entry->data.data() + offset, data + done, count);
                }
            }
        });
    }

    /// A download put these bytes on the host. Downloads still in flight saw them too: comparing
    /// them with older contents would read it as a CPU write when the GPU changes it back.
    void Landed(u64 address, const u8* data, u64 size) {
        if (entries.empty()) {
            return;
        }
        ForEachPage(address, size, [&](u64 key, u64 offset, u64 done, u64 count) {
            if (Entry* const entry = Find(key)) {
                StoreLanded(*entry, offset, data + done, count);
            }
        });
    }

    /// The host buffer over these pages is gone. Downloads in flight keep what they compare with.
    void Erase(u64 address, u64 size) {
        if (entries.empty()) {
            return;
        }
        const u64 end = (address + size + PAGE_MASK) >> PAGE_BITS;
        for (u64 key = address >> PAGE_BITS; key < end; ++key) {
            Entry* const entry = Find(key);
            if (!entry) {
                continue;
            }
            Preserve(*entry, key);
            if (entry->versions.empty()) {
                Remove(key);
            } else {
                entry->host_gone = true;
            }
        }
    }

    /// Every download of the group and older ones landed.
    void Retire(u64 record) {
        retired_through = (std::max)(retired_through, record);
        for (size_t index = 0; index < versioned.size();) {
            const u64 key = versioned[index];
            Entry& entry = *Find(key);
            auto& versions = entry.versions;
            size_t expired = 0;
            while (expired < versions.size() && versions[expired].through <= retired_through) {
                Free(std::move(versions[expired].page));
                ++expired;
            }
            versions.erase(versions.begin(), versions.begin() + static_cast<std::ptrdiff_t>(expired));
            if (!versions.empty()) {
                ++index;
                continue;
            }
            versioned[index] = versioned.back();
            versioned.pop_back();
            entry.listed = false;
            if (entry.host_gone) {
                Remove(key);
            }
        }
    }

    /// Three values per word: the host when the download was recorded, the download, and guest
    /// memory now. What differs from the first was changed by the GPU and lands in guest memory.
    /// Anything else in guest memory that differs from the download was written by the CPU and has
    /// not reached the host. on_land and on_keep take (address, size) of each run of words.
    /// start and size are word aligned.
    template <class DeviceMemory, class OnLand, class OnKeep>
    void Merge(u64 start, u64 size, const u8* incoming, u64 record, DeviceMemory& device_memory,
               OnLand&& on_land, OnKeep&& on_keep) {
        ForEachPage(start, size, [&](u64 key, u64 offset, u64 done, u64 count) {
            const u64 page_address = key << PAGE_BITS;
            Entry* const entry = Find(key);
            u8* const guest = device_memory.template GetPointer<u8>(page_address);
            const u8* const before = entry ? entry->BaselineFor(record) : nullptr;
            if (!guest || !before) {
                device_memory.WriteBlockUnsafe(page_address + offset, incoming + done, count);
                if (entry) {
                    StoreLanded(*entry, offset, incoming + done, count);
                }
                on_land(page_address + offset, count);
                return;
            }
            MergePage(*entry, page_address, offset, count, incoming + done, before, guest, on_land,
                      on_keep);
        });
    }

    [[nodiscard]] u64 Bytes() const {
        return (entries.size() + version_pages) * PAGE_SIZE;
    }

    [[nodiscard]] u64 VersionPages() const {
        return version_pages;
    }

private:
    static constexpr u64 PAGE_BITS = 12;
    static constexpr u64 PAGE_SIZE = 1ULL << PAGE_BITS;
    static constexpr u64 PAGE_MASK = PAGE_SIZE - 1;
    static constexpr u64 MAX_PAGES = (32ULL * 1024 * 1024) >> PAGE_BITS;
    static constexpr size_t MAX_FREE_PAGES = 64;
    using Page = std::array<u8, PAGE_SIZE>;

    struct Version {
        u64 through; ///< Downloads recorded up to this group, and after the previous version's.
        std::unique_ptr<Page> page;
    };

    struct Entry {
        Page data;
        std::vector<Version> versions; ///< Oldest first; almost always empty.
        u64 last_record{};             ///< Newest group with a download over this page.
        bool host_gone{};              ///< Kept only for the versions: data is out of date.
        bool listed{};                 ///< In versioned.

        [[nodiscard]] const u8* BaselineFor(u64 record) const {
            for (const Version& version : versions) {
                if (version.through >= record) {
                    return version.page->data();
                }
            }
            return host_gone ? nullptr : data.data();
        }
    };

    enum class Run : u8 { None, Land, Keep };

    template <class Func>
    static void ForEachPage(u64 address, u64 size, Func&& func) {
        for (u64 done = 0; done < size;) {
            const u64 current = address + done;
            const u64 offset = current & PAGE_MASK;
            const u64 count = (std::min)(PAGE_SIZE - offset, size - done);
            func(current >> PAGE_BITS, offset, done, count);
            done += count;
        }
    }

    [[nodiscard]] Entry* Find(u64 key) {
        // Most downloads are tiny and land next to the previous one.
        if (last_entry && last_key == key) {
            return last_entry;
        }
        const auto it = entries.find(key);
        if (it == entries.end()) {
            return nullptr;
        }
        last_key = key;
        last_entry = it->second.get();
        return last_entry;
    }

    void Remove(u64 key) {
        if (last_key == key) {
            last_entry = nullptr;
        }
        entries.erase(key);
    }

    /// Saves the page for the downloads in flight, unless a version already holds it for them.
    void Preserve(Entry& entry, u64 key) {
        if (entry.last_record <= retired_through) {
            return;
        }
        if (!entry.versions.empty() && entry.versions.back().through >= entry.last_record) {
            return;
        }
        std::unique_ptr<Page> page;
        if (!free_pages.empty()) {
            page = std::move(free_pages.back());
            free_pages.pop_back();
        } else {
            page = std::make_unique<Page>();
        }
        *page = entry.data;
        entry.versions.push_back({entry.last_record, std::move(page)});
        ++version_pages;
        if (!entry.listed) {
            entry.listed = true;
            versioned.push_back(key);
        }
    }

    void Free(std::unique_ptr<Page> page) {
        --version_pages;
        if (free_pages.size() < MAX_FREE_PAGES) {
            free_pages.push_back(std::move(page));
        }
    }

    static void StoreLanded(Entry& entry, u64 offset, const u8* data, u64 size) {
        if (!entry.host_gone) {
            std::memcpy(entry.data.data() + offset, data, size);
        }
        for (Version& version : entry.versions) {
            std::memcpy(version.page->data() + offset, data, size);
        }
    }

    template <class OnLand, class OnKeep>
    static void MergePage(Entry& entry, u64 page_address, u64 offset, u64 count, const u8* incoming,
                          const u8* before_page, u8* guest_page, OnLand&& on_land,
                          OnKeep&& on_keep) {
        const u8* const before = before_page + offset;
        u8* const guest = guest_page + offset;
        const u64 words = count / sizeof(u32);
        Run run = Run::None;
        u64 run_start = 0;
        // Runs only close behind the cursor, so the shadow they update (often the page before
        // points to) is never read again in this merge.
        const auto close_run = [&](u64 word) {
            const u64 run_offset = run_start * sizeof(u32);
            const u64 length = (word - run_start) * sizeof(u32);
            if (run == Run::Land) {
                std::memcpy(guest + run_offset, incoming + run_offset, length);
                StoreLanded(entry, offset + run_offset, incoming + run_offset, length);
                on_land(page_address + offset + run_offset, length);
            } else if (run == Run::Keep) {
                on_keep(page_address + offset + run_offset, length);
            }
        };
        const auto step = [&](u64 word, Run kind) {
            if (kind != run) {
                close_run(word);
                run = kind;
                run_start = word;
            }
        };
        u64 word = 0;
#if defined(__AVX2__)
        constexpr u64 LANES = sizeof(__m256i) / sizeof(u32);
        for (; word + LANES <= words; word += LANES) {
            const u64 at = word * sizeof(u32);
            const __m256i from_gpu =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(incoming + at));
            const __m256i host = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(before + at));
            const __m256i now = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(guest + at));
            const u32 land = ~static_cast<u32>(_mm256_movemask_ps(
                                 _mm256_castsi256_ps(_mm256_cmpeq_epi32(from_gpu, host)))) &
                             0xff;
            const u32 differs = ~static_cast<u32>(_mm256_movemask_ps(
                                    _mm256_castsi256_ps(_mm256_cmpeq_epi32(from_gpu, now)))) &
                                0xff;
            if ((land | differs) == 0) {
                step(word, Run::None);
                continue;
            }
            if (land == 0xff) {
                step(word, Run::Land);
                continue;
            }
            for (u32 lane = 0; lane < LANES; ++lane) {
                const u32 bit = 1U << lane;
                step(word + lane, (land & bit) != 0      ? Run::Land
                                  : (differs & bit) != 0 ? Run::Keep
                                                         : Run::None);
            }
        }
#endif
        for (; word < words; ++word) {
            const u64 at = word * sizeof(u32);
            u32 from_gpu;
            u32 host;
            u32 now;
            std::memcpy(&from_gpu, incoming + at, sizeof(u32));
            std::memcpy(&host, before + at, sizeof(u32));
            std::memcpy(&now, guest + at, sizeof(u32));
            step(word, from_gpu != host ? Run::Land : (from_gpu != now ? Run::Keep : Run::None));
        }
        close_run(words);
    }

    std::unordered_map<u64, std::unique_ptr<Entry>> entries;
    std::vector<u64> versioned; ///< Keys of the entries with versions.
    std::vector<std::unique_ptr<Page>> free_pages;
    u64 version_pages{};
    u64 newest_record{};
    u64 retired_through{};
    u64 last_key{};
    Entry* last_entry{};
};

} // namespace VideoCommon
