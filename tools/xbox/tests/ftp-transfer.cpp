// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdio>
#include "jadyx/storage/ftp/ftp_paths.h"
#include "jadyx/storage/ftp/ftp_transfer.h"

int main() {
    using namespace Jadyx::Ftp;
    Roots roots;
    roots.games = "/fixture/LocalState/games";
    roots.keys = "/fixture/LocalState/jadyx/keys";
    roots.firmware = "/fixture/LocalState/jadyx/nand/system/Contents/registered";
    roots.saves = "/fixture/LocalState/jadyx/nand/user/save";
    for (const auto alias : RootNames) {
        const auto path = MapPath(roots, std::string(alias) + "/nested/file with spaces.bin");
        assert(path && path->relative == "nested/file with spaces.bin");
        assert(path->root.is_absolute());
    }
    assert(MapPath(roots, "keys/prod.keys")->root == roots.keys);
    assert(MapPath(roots, "firmware/a.nca")->root == roots.firmware);
    assert(MapPath(roots, "saves/account/user/title/file")->root == roots.saves);
    for (const auto path : {"interno/jadyx/keys/prod.keys", "games-old/a", "jadyx/log/a",
                            "games/../keys/a", "games/a.part", "keys/../../a", "/games/a"})
        assert(!MapPath(roots, path));

    // Simulate 12 GiB + 37 bytes without writing a giant fixture or using a network socket.
    // Real copy loop, real 1 MiB spans, 64-bit totals and a short final block.
    constexpr std::uint64_t large = 12ULL * 1024 * 1024 * 1024 + 37;
    auto run = [&](bool known) {
        std::uint64_t source_left = large, written{};
        std::size_t calls{};
        const auto copied = CopyStream(
            [&](std::span<std::uint8_t> buffer) {
                assert(buffer.size() <= BufferSize);
                const auto count =
                    static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), source_left));
                if (count) {
                    buffer.front() = 0x42;
                    buffer[count - 1] = 0x42;
                }
                source_left -= count;
                ++calls;
                return count;
            },
            [&](std::span<const std::uint8_t> buffer) {
                assert(buffer.front() == 0x42 && buffer.back() == 0x42);
                written += buffer.size();
                return buffer.size();
            },
            [] { return true; }, known ? large : std::numeric_limits<std::uint64_t>::max());
        assert(copied == large && written == large && source_left == 0);
        assert(calls == (known ? 12289 : 12290));
    };
    run(true);
    run(false);
    const auto expect_failure = [](auto action) {
        bool failed{};
        try {
            action();
        } catch (const std::runtime_error&) {
            failed = true;
        }
        assert(failed);
    };
    const auto full_read = [](auto buffer) { return buffer.size(); };
    const auto full_write = [](auto buffer) { return buffer.size(); };
    expect_failure([&] { CopyStream(full_read, full_write, [] { return false; }, 1); });
    expect_failure([&] {
        CopyStream([](auto) { return std::size_t{0}; }, full_write, [] { return true; }, large);
    });
    expect_failure([&] {
        CopyStream(full_read, [](auto) { return std::size_t{0}; }, [] { return true; }, 1);
    });
    expect_failure([&] {
        CopyStream([](auto buffer) { return buffer.size() + 1; }, full_write, [] { return true; },
                   1);
    });
    bool active = true;
    expect_failure([&] {
        CopyStream(
            [&](auto buffer) {
                active = false;
                return buffer.size();
            },
            full_write, [&] { return active; }, 1);
    });
    assert(CopyStream(full_read, full_write, [] { return true; }, 0) == 0);
    std::puts(
        "FTP paths and streaming: 12 GiB+37 in both directions, cancellation and I/O errors PASS");
}
