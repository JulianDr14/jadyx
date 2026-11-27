// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace Kernel::Board::Nintendo::Nx {

/// The fixed secure region that applet-pool processes take their system resource from.
///
/// It follows the NX kernel (Mesosphere's AllocateSecureMemoryForApplet): one region reserved at
/// boot, lent whole to one applet at a time. A second applet, an oversized or unaligned request
/// fails like on hardware instead of reaching the generic pools.
class SecureAppletMemory {
public:
    enum class Status {
        Ok,
        InvalidSize, ///< Size is not a multiple of the page size.
        OutOfMemory, ///< Region missing, too small, or already lent out.
    };

    /// Sets the region. Zero size means the board has none, so every request fails.
    void Initialize(std::uint64_t address, std::size_t size, std::size_t page_size) {
        std::scoped_lock lock{mutex};
        base = address;
        capacity = size;
        page = page_size;
        in_use = false;
    }

    [[nodiscard]] Status Allocate(std::uint64_t* out, std::size_t size) {
        std::scoped_lock lock{mutex};
        if (page == 0 || size % page != 0) {
            return Status::InvalidSize;
        }
        if (capacity == 0 || size > capacity || in_use) {
            return Status::OutOfMemory;
        }
        in_use = true;
        *out = base;
        return Status::Ok;
    }

    /// Returns the region. False when the request does not match the lent one; the region then
    /// stays lent, because freeing on a bad request could hand it to two applets at once.
    [[nodiscard]] bool Free(std::uint64_t address, std::size_t size) {
        std::scoped_lock lock{mutex};
        if (!in_use || address != base || size > capacity || page == 0 || size % page != 0) {
            return false;
        }
        in_use = false;
        return true;
    }

    [[nodiscard]] bool InUse() const {
        std::scoped_lock lock{mutex};
        return in_use;
    }

private:
    mutable std::mutex mutex;
    std::uint64_t base = 0;
    std::size_t capacity = 0;
    std::size_t page = 0;
    bool in_use = false;
};

} // namespace Kernel::Board::Nintendo::Nx
