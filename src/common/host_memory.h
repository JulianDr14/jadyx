// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/guest_zero_census.h"

namespace Common {

struct HostMemoryFastmemRegion {
    size_t offset{};
    size_t size{};
    bool force_full{};
};

/// Configured by the UWP frontend before Core::System creates DeviceMemory.
void ConfigureHostMemoryFastmem(size_t hot_mib, bool force_full);
[[nodiscard]] size_t HostMemoryFastmemHotSize();
[[nodiscard]] bool HostMemoryFastmemForceFull();

/// Xbox private DRAM backing: cold guest memory written to a swap file (Permafrost, common/permafrost.h).
/// Configured by the UWP frontend before Core::System creates DeviceMemory; off unless enabled.
struct GuestSwapSettings {
    bool enabled{};
    std::wstring path;
    u64 reserve_bytes{};
    u32 probation_ms{1500};
    u32 min_idle_ms{8000};
    u64 batch_bytes{64ULL << 20};
    u32 fill_pages{4};
    bool prefetch{true};
    bool pause_fill{true};
};
void ConfigureGuestSwap(const GuestSwapSettings& settings);
/// Whether the backing in use writes cold memory out.
[[nodiscard]] bool GuestSwapActive();
/// How short of memory the swapper's caller is.
enum class GuestSwapPressure : u8 {
    Watch,     // close: the access sampling runs, nothing goes out
    Low,       // short: idle granules go out
    Urgent,    // far short
    Emergency, // commits about to fail
};
/// One pass of the swapper: tries to give back `want` bytes, written out by its own threads.
/// Returns the bytes those threads gave back since the last pass.
u64 StepGuestSwap(u64 want, GuestSwapPressure pressure);
/// Commits again the reserve given up to a failed demand commit. True when it is held.
bool RefillGuestSwapReserve();
/// Whether a demand commit failed and took the reserve since it was last refilled.
[[nodiscard]] bool GuestSwapReserveReleased();
[[nodiscard]] std::string DescribeGuestSwap();

enum class MemoryPermission : u32 {
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
    Execute = 1 << 2,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

/**
 * A low level linear memory buffer, which supports multiple mappings
 * Its purpose is to rebuild a given sparse memory layout, including mirrors.
 */
class HostMemory {
public:
    explicit HostMemory(size_t backing_size_, size_t virtual_size_,
                        HostMemoryFastmemRegion fastmem_region_ = {});
    ~HostMemory();

    /**
     * Copy constructors. They shall return a copy of the buffer without the mappings.
     * TODO: Implement them with COW if needed.
     */
    HostMemory(const HostMemory& other) = delete;
    HostMemory& operator=(const HostMemory& other) = delete;

    /**
     * Move constructors. They will move the buffer and the mappings to the new object.
     */
    HostMemory(HostMemory&& other) noexcept;
    HostMemory& operator=(HostMemory&& other) noexcept;

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms,
             bool separate_heap);

    void Unmap(size_t virtual_offset, size_t length, bool separate_heap);

    void Protect(size_t virtual_offset, size_t length, MemoryPermission perms);

    void EnableDirectMappedAddress();

    void ClearBackingRegion(size_t physical_offset, size_t length, u32 fill_value);

    size_t ReleaseBackingRegion(size_t physical_offset, size_t length);

    struct TrimResult {
        size_t visited{};
        size_t released{};
    };
    TrimResult Trim(size_t& cursor, size_t budget);

    [[nodiscard]] u8* BackingBasePointer() noexcept {
        return backing_base;
    }
    [[nodiscard]] const u8* BackingBasePointer() const noexcept {
        return backing_base;
    }

    [[nodiscard]] size_t BackingSize() const noexcept {
        return backing_size;
    }

    [[nodiscard]] u8* VirtualBasePointer() noexcept {
        return virtual_base;
    }
    [[nodiscard]] const u8* VirtualBasePointer() const noexcept {
        return virtual_base;
    }

    bool IsInVirtualRange(void* address) const noexcept {
        return address >= virtual_base && address < virtual_base + virtual_size;
    }

private:
    size_t backing_size{};
    size_t virtual_size{};

#if !(defined(__OPENORBIS__) || defined(__managarm__))
    // Low level handler for the platform dependent memory routines
    class Impl;
    std::unique_ptr<Impl> impl;
#endif
    u8* backing_base{};
    u8* virtual_base{};
    size_t virtual_base_offset{};
    // Windows requires it for kernels whom lack proper support for some functions!
    bool fallback_buffer{false};
};

/// Xbox fastmem: MiB committed in the DRAM section and the demand-commit failures so far, for the
/// diag. Empty elsewhere.
std::string HostMemoryCommitStats();

/// Xbox: where `address` falls against the DRAM backing, the process commit and the last
/// demand-commit failure, as one line in `out`. Allocates nothing, for the crash handler.
void FormatBackingFault(char* out, size_t out_size, uintptr_t address);

/// Bytes of the emulated DRAM committed in the app (Xbox host memory). Empty where it cannot be
/// told apart, such as a file-backed section.
std::optional<u64> EmulatedDramCommittedBytes();

/// Xbox private DRAM backing: the run of host pages around `host` that share its commit state,
/// clamped to the backing. `untouched` pages are reserved but never committed, and the guest reads
/// them as zero. Empty outside such a backing (fastmem section, file, desktop).
struct DramRegion {
    const u8* begin{};
    const u8* end{};
    bool untouched{};
};
std::optional<DramRegion> QueryDramRegion(const void* host);

/// Held around every decommit of the DRAM backing and every move of the guest swap that needs
/// one, so what is committed stays committed while a reader holds it (Permafrost::InspectCommitted).
std::mutex& DramDecommitMutex();

/// Diagnostic, Xbox private DRAM backing: the committed bytes that hold only zeros, by guest swap
/// granule. Reads every committed page, so it costs a pass over the DRAM. Empty elsewhere.
std::optional<GuestZeroCensus> CensusGuestZeroMemory();

/// Diagnostic: the commit state of the DRAM page at `host`, and the last kernel clear that
/// decommitted it.
std::string DescribeDramPage(const void* host);

/// Diagnostic: queue a DRAM page whose first write after ArmDramWriteWatch is logged.
void WatchDramPage(const void* host);

/// Diagnostic: forget the queued pages.
void ResetDramWatch();

/// Diagnostic: each queued page as its nonzero word count, or 'r' when never committed.
std::string DescribeWatchedPages();

/// Diagnostic: write-protect the queued pages; each one logs its first writer and unprotects.
void ArmDramWriteWatch();

/// Diagnostic: names the kernel path whose DRAM clears follow, on this thread.
class ScopedClearTag {
public:
    explicit ScopedClearTag(const char* tag);
    ~ScopedClearTag();

private:
    const char* previous;
};

} // namespace Common
