// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
constexpr u64 YUZU_PAGEBITS = 12;
constexpr u64 YUZU_PAGESIZE = 1 << YUZU_PAGEBITS;
namespace Common {
using ProcessAddress = u64;
using PhysicalAddress = u64;
enum class MemoryPermission { None, Read, Write, ReadWrite };
MemoryPermission& operator|=(MemoryPermission& value, MemoryPermission) {
    return value;
}
template <typename T>
struct SparseLargeVector {
    std::array<T, 8> items{};
    void CommitRegion(u64, u64) {}
    void ZeroRegion(u64 begin, u64 end);
    T& GetUnchecked(u64 index) {
        return items[index];
    }
    T& operator[](u64 index) {
        return items[index];
    }
    std::size_t size() const {
        return items.size();
    }
};
} // namespace Common
// PAGE_TABLE
namespace Common {
PageTable::PageTable() = default;
PageTable::~PageTable() noexcept = default;
template <typename T>
void SparseLargeVector<T>::ZeroRegion(u64 begin, u64 end) {
    for (; begin < end; ++begin) {
        if constexpr (std::is_same_v<T, PageTable::JitPageEntry>) {
            items[begin].Store(0);
        } else {
            items[begin].Store(false, PageType::Unmapped, 0, 0);
        }
    }
}
} // namespace Common
#define ASSERT(condition) assert(condition)
#define ASSERT_MSG(condition, ...) assert(condition)
#define UNREACHABLE() assert(false)
#define LOG_DEBUG(...) ((void)0)
u64 GetInteger(u64 address) {
    return address;
}
bool AddressSpaceContains(const Common::PageTable&, u64 address, u64 size) {
    return address < 8 * YUZU_PAGESIZE && size <= 8 * YUZU_PAGESIZE - address;
}
namespace Settings {
struct Reactive {
    bool GetValue() const {
        return false;
    }
};
struct Values {
    Reactive use_reactive_flushing;
} values;
} // namespace Settings
struct Host {
    std::promise<void> entered;
    std::shared_future<void> release;
    void Protect(u64, u64, Common::MemoryPermission) {
        entered.set_value();
        release.wait();
    }
};
struct System {
    System& DeviceMemory() {
        return *this;
    }
    template <typename T>
    T* GetPointer(u64 address) {
        return reinterpret_cast<T*>(address + 0x100000);
    }
};
struct Memory {
    Common::PageTable* current_page_table{};
    Host* host_buffer{};
    System system;
    std::atomic<u16> block_count{1};
    // WRITERS
};
void Check(const Common::PageTable& table) {
    for (u64 index = 1; index <= 4; ++index) {
        const auto& entry = table.entries.items[index];
        const auto expected = entry.Type() == Common::PageType::Memory ? entry.Pointer() : 0;
        assert(table.jit_entries.items[index].Load() == expected);
    }
}
int main() {
    // Pause inside host protection: another writer must not pass the same table's lock.
    for (const bool debug : {false, true}) {
        Common::PageTable table;
        Host host;
        Memory memory;
        memory.current_page_table = &table;
        memory.host_buffer = &host;
        memory.MapPages(table, 1, 4, 0x200000, Common::PageType::Memory);
        table.fastmem_arena = reinterpret_cast<u8*>(1);
        std::promise<void> release;
        host.release = release.get_future().share();
        auto first = std::async(std::launch::async, [&] {
            if (debug)
                memory.MarkRegionDebug(YUZU_PAGESIZE, 4 * YUZU_PAGESIZE, true);
            else
                memory.RasterizerMarkRegionCached(YUZU_PAGESIZE, 4 * YUZU_PAGESIZE, true);
        });
        host.entered.get_future().wait();
        std::promise<void> started;
        auto second = std::async(std::launch::async, [&] {
            started.set_value();
            memory.MapPages(table, 1, 4, 0, Common::PageType::Unmapped);
        });
        started.get_future().wait();
        const bool blocked =
            second.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
        release.set_value();
        first.get();
        second.get();
        assert(blocked && "map/unmap overlapped a debug/rasterizer writer");
        Check(table);
    }
    // Exercise real canonical entry atomics and all three production writers concurrently.
    Common::PageTable table;
    Memory memory;
    memory.current_page_table = &table;
    memory.MapPages(table, 1, 4, 0x200000, Common::PageType::Memory);
    for (unsigned round = 0; round < 100; ++round) {
        std::thread mapping([&] {
            for (unsigned i = 0; i < 100; ++i)
                memory.MapPages(table, 1, 4, 0x200000, Common::PageType::Memory);
        });
        std::thread marking([&] {
            for (unsigned i = 0; i < 100; ++i) {
                memory.MarkRegionDebug(YUZU_PAGESIZE, 4 * YUZU_PAGESIZE, true);
                memory.MarkRegionDebug(YUZU_PAGESIZE, 4 * YUZU_PAGESIZE, false);
                memory.RasterizerMarkRegionCached(YUZU_PAGESIZE, 4 * YUZU_PAGESIZE, true);
                memory.RasterizerMarkRegionCached(YUZU_PAGESIZE, 4 * YUZU_PAGESIZE, false);
            }
        });
        mapping.join();
        marking.join();
        Check(table);
    }
    memory.MapPages(table, 1, 4, 0, Common::PageType::Unmapped);
    Check(table);
}
