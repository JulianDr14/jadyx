// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <span>
using u8 = std::uint8_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
using Result = int;
constexpr Result ResultUnknown = 9;
namespace FileSys {
constexpr Result ResultInvalidSize = 1;
constexpr Result ResultInvalidOffset = 2;
constexpr Result ResultOutOfRange = 3;
} // namespace FileSys
#define LOG_DEBUG(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define R_UNLESS(condition, result)                                                                \
    do {                                                                                           \
        if (!(condition))                                                                          \
            return result;                                                                         \
    } while (0)
#define R_THROW(result) return result
#define R_SUCCEED() return 0
constexpr int BufferAttr_HipcMapAlias = 4;
constexpr int BufferAttr_HipcMapTransferAllowsNonSecure = 64;
template <int Attr>
using OutBuffer = std::span<u8>;
struct Backend {
    std::size_t size = 64;
    std::size_t available = 64;
    int calls = 0;
    u64 last_offset = 0;
    std::size_t GetSize() const {
        return size;
    }
    std::size_t Read(u8* buffer, std::size_t length, std::size_t offset) {
        ++calls;
        last_offset = offset;
        const auto count = std::min(length, available);
        std::fill_n(buffer, count, 0x5a);
        return count;
    }
};
struct IStorage {
    Backend* backend{};
    Result Read(OutBuffer<68> out_bytes, s64 offset, s64 length);
};
// HANDLER
int main() {
    Backend backend;
    IStorage storage;
    storage.backend = &backend;
    std::array<u8, 10> bytes{};
    auto buffer = std::span{bytes}.subspan(1, 8);
    assert(storage.Read(buffer, 3, 8) == 0);
    assert(backend.calls == 1 && backend.last_offset == 3);
    assert(bytes.front() == 0 && bytes.back() == 0 && bytes[1] == 0x5a);

    backend.available = 0;
    assert(storage.Read(buffer, 0, 8) == ResultUnknown);
    backend.available = 7;
    assert(storage.Read(buffer, 0, 8) == ResultUnknown);
    const int calls = backend.calls;
    assert(storage.Read(buffer, 0, 9) == FileSys::ResultInvalidSize);
    assert(storage.Read(buffer, -1, 8) == FileSys::ResultInvalidOffset);
    assert(storage.Read(buffer, 0, -1) == FileSys::ResultInvalidSize);
    assert(storage.Read(buffer, 60, 8) == FileSys::ResultOutOfRange);
    assert(storage.Read(buffer, 65, 0) == FileSys::ResultOutOfRange);
    assert(storage.Read({}, 64, 0) == 0);
    assert(storage.Read(buffer, std::numeric_limits<s64>::max(), 8) == FileSys::ResultOutOfRange);
    assert(backend.calls == calls);

    // No narrowing at the 4 GiB boundary, and a buffer larger than length is allowed.
    backend.size = (u64{1} << 33);
    backend.available = 4;
    assert(storage.Read(buffer, (s64{1} << 32) + 3, 4) == 0);
    assert(backend.last_offset == (u64{1} << 32) + 3);
    assert(bytes.back() == 0);

    // A mathematically valid range crossing signed INT64_MAX must not overflow internally.
    backend.size = std::numeric_limits<std::size_t>::max();
    assert(storage.Read(buffer, std::numeric_limits<s64>::max(), 4) == 0);
    assert(backend.last_offset == static_cast<u64>(std::numeric_limits<s64>::max()));

    // Exhaustive bounded requests: invalid ranges must not call or write the backend.
    backend.size = 64;
    backend.available = 64;
    for (s64 offset = 0; offset <= 65; ++offset) {
        for (s64 length = 0; length <= 10; ++length) {
            bytes.fill(0);
            const auto before_calls = backend.calls;
            const auto result = storage.Read(buffer, offset, length);
            const bool valid_size = length <= 8;
            const bool valid_range = offset <= 64 && length <= 64 - offset;
            const Result expected = !valid_size    ? FileSys::ResultInvalidSize
                                    : !valid_range ? FileSys::ResultOutOfRange
                                                   : 0;
            assert(result == expected);
            assert(backend.calls == before_calls + (expected == 0 && length != 0 ? 1 : 0));
            assert(bytes.front() == 0 && bytes.back() == 0);
            if (expected != 0) {
                assert(
                    std::all_of(bytes.begin(), bytes.end(), [](u8 value) { return value == 0; }));
            }
        }
    }
}
