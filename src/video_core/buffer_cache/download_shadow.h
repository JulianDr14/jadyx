// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon {

/// What the host buffers hold where downloads happen, as far as uploads and landed downloads tell.
/// A download compared with this shows which words the GPU changed. A shader binding is marked
/// as written whole, but on the console the GPU only stores the words the shader writes: the rest
/// of the guest memory keeps whatever the CPU put there meanwhile.
/// Callers hold the buffer cache mutex.
class DownloadShadow {
public:
    /// Copy of the range into out. Pages seen for the first time start from guest memory. False
    /// once the shadow is full: those downloads land whole.
    template <class DeviceMemory>
    [[nodiscard]] bool Snapshot(u64 address, u64 size, DeviceMemory& device_memory,
                                std::vector<u8>& out) {
        out.resize(size);
        for (u64 done = 0; done < size;) {
            const u64 current = address + done;
            const u64 offset = current & PAGE_MASK;
            const u64 count = (std::min)(PAGE_SIZE - offset, size - done);
            auto& page = pages[current >> PAGE_BITS];
            if (!page) {
                if (pages.size() > MAX_PAGES) {
                    pages.erase(current >> PAGE_BITS);
                    if (!warned_full) {
                        warned_full = true;
                        LOG_WARNING(HW_GPU, "Download shadow full: some downloads land whole");
                    }
                    return false;
                }
                page = std::make_unique<Page>();
                device_memory.ReadBlockUnsafe(current - offset, page->data(), PAGE_SIZE);
            }
            std::memcpy(out.data() + done, page->data() + offset, count);
            done += count;
        }
        return true;
    }

    /// The host now holds these bytes. Ranges without a page have no download to compare.
    void Store(u64 address, const u8* data, u64 size) {
        if (pages.empty()) {
            return;
        }
        for (u64 done = 0; done < size;) {
            const u64 current = address + done;
            const u64 offset = current & PAGE_MASK;
            const u64 count = (std::min)(PAGE_SIZE - offset, size - done);
            if (const auto it = pages.find(current >> PAGE_BITS); it != pages.end()) {
                std::memcpy(it->second->data() + offset, data + done, count);
            }
            done += count;
        }
    }

    /// The host buffer over these pages is gone.
    void Erase(u64 address, u64 size) {
        if (pages.empty()) {
            return;
        }
        const u64 end = (address + size + PAGE_MASK) >> PAGE_BITS;
        for (u64 page = address >> PAGE_BITS; page < end; ++page) {
            pages.erase(page);
        }
    }

    [[nodiscard]] u64 Bytes() const {
        return pages.size() * PAGE_SIZE;
    }

private:
    static constexpr u64 PAGE_BITS = 12;
    static constexpr u64 PAGE_SIZE = 1ULL << PAGE_BITS;
    static constexpr u64 PAGE_MASK = PAGE_SIZE - 1;
    static constexpr u64 MAX_PAGES = (32ULL * 1024 * 1024) >> PAGE_BITS;
    using Page = std::array<u8, PAGE_SIZE>;

    std::unordered_map<u64, std::unique_ptr<Page>> pages;
    bool warned_full{};
};

} // namespace VideoCommon
