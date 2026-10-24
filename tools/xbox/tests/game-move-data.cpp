// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include "jadyx/storage/game_move_catalog.h"
#include "jadyx/storage/move_digest.h"
#include "jadyx/storage/verified_move.h"

int main() {
    using namespace Jadyx;
    std::vector<unsigned char> source(2 * 1024 * 1024 + 19, 42);
    auto reader = [&](std::span<unsigned char> out, std::uint64_t offset) {
        const auto n =
            static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), source.size() - offset));
        std::copy_n(source.data() + offset, n, out.data());
        return n;
    };
    auto digest = VerifiedMove::Hash(reader, source.size());
    assert(digest);
    source[1024 * 1024] ^= 1;
    assert(digest != VerifiedMove::Hash(reader, source.size())); // Same size corruption.
    assert(!VerifiedMove::Hash(reader, source.size() - 1));      // Trailing byte.
    auto truncated = [&](std::span<unsigned char> out, std::uint64_t offset) {
        if (offset >= 1024 * 1024)
            return std::size_t{};
        return reader(out, offset);
    };
    assert(!VerifiedMove::Hash(truncated, source.size()));
    std::stop_source cancelled;
    cancelled.request_stop();
    assert(!VerifiedMove::Hash(reader, source.size(), cancelled.get_token()));
    const auto empty = VerifiedMove::Hash([](auto, auto) { return std::size_t{}; }, 0);
    assert(empty && (*empty)[0] == 0xe3 && (*empty)[31] == 0x55); // SHA-256 empty vector.
    Content::Catalog catalog;
    Content::Revision update;
    update.id = "update";
    update.application = 0x1000;
    update.internal = true;
    update.enabled = update.verified = true;
    update.version = 7;
    update.required_system = 9;
    Content::Revision dlc = update;
    dlc.id = "disabled";
    dlc.kind = Content::Kind::Dlc;
    dlc.enabled = false;
    Content::Revision embedded = dlc;
    embedded.id = "embedded";
    embedded.internal = false;
    embedded.source = "old-base";
    Content::Revision other = update;
    other.application = 0x2000;
    Content::Revision external = dlc;
    external.id = "external";
    external.internal = false;
    external.source = "other-usb";
    catalog.revisions = {update, dlc, embedded, other, external};
    catalog.managed_applications = {0x1000, 0x2000};
    const auto next = RelocateGameCatalog(catalog, 0x1000, "old-base", "new-base", 100,
                                          {{"update", "usb-update"}, {"disabled", "usb-dlc"}});
    assert(next.managed_applications == catalog.managed_applications);
    assert(!next.revisions[0].internal && next.revisions[0].enabled && next.revisions[0].verified);
    assert(next.revisions[0].version == 7 && next.revisions[0].required_system == 9);
    assert(next.revisions[0].source == "usb-update" && next.revisions[0].source_size == 0);
    assert(!next.revisions[1].enabled && next.revisions[1].source == "usb-dlc");
    assert(next.revisions[2].source == "new-base" && next.revisions[2].source_size == 100);
    assert(next.revisions[3].internal && next.revisions[4].source == "other-usb");
    assert(catalog.revisions[0].internal && catalog.revisions[2].source == "old-base");
    // Real disposable files exercise the shared transaction and SHA-256 together.
    // Equal-size corruption of the last destination must retain ALL originals.
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
                      ("jadyx-move-host-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(fs::create_directory(root));
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            fs::remove_all(path);
        }
    } cleanup{.path = root};
    for (bool corrupt : {true, false}) {
        const auto trial = root / (corrupt ? "corrupt" : "valid");
        const auto originals = trial / "originals";
        auto target = trial / "staging";
        fs::create_directories(originals);
        fs::create_directory(target);
        const std::vector<VerifiedMove::File> files{{.relative = "base.bin", .bytes = 100},
                                                    {.relative = "update.bin", .bytes = 23},
                                                    {.relative = "disabled-dlc.bin", .bytes = 7}};
        for (const auto& file : files) {
            std::ofstream out{originals / file.relative, std::ios::binary};
            out << std::string(static_cast<std::size_t>(file.bytes), 'a');
        }
        const auto hash = [&](const fs::path& path, std::uint64_t bytes) {
            std::ifstream in{path, std::ios::binary};
            return VerifiedMove::Hash(
                [&](std::span<unsigned char> buffer, std::uint64_t offset) {
                    in.clear();
                    in.seekg(static_cast<std::streamoff>(offset));
                    in.read(reinterpret_cast<char*>(buffer.data()),
                            static_cast<std::streamsize>(buffer.size()));
                    return static_cast<std::size_t>(in.gcount());
                },
                bytes);
        };
        bool registered = false;
        VerifiedMove::IO io;
        io.cancelled = [] { return false; };
        io.copy = [&](const auto& file) {
            if (!fs::copy_file(originals / file.relative, target / file.relative))
                return false;
            if (corrupt && file.relative == "disabled-dlc.bin") {
                std::fstream out{target / file.relative,
                                 std::ios::binary | std::ios::in | std::ios::out};
                out.put('x');
            }
            return true;
        };
        io.verify = [&](const auto& file) {
            const auto expected = hash(originals / file.relative, file.bytes);
            return expected && fs::file_size(target / file.relative) == file.bytes &&
                   hash(target / file.relative, file.bytes) == expected;
        };
        io.publish = [&] {
            fs::rename(target, trial / "published");
            target = trial / "published";
            return true;
        };
        io.commit = [&] {
            registered = true;
            return true;
        };
        io.remove = [&](const auto& file) {
            assert(registered);
            return fs::remove(originals / file.relative);
        };
        const auto result = VerifiedMove::Run(files, VerifiedMove::Reserve + 130, io);
        assert(result ==
               (corrupt ? VerifiedMove::Result::Verification : VerifiedMove::Result::Success));
        for (const auto& file : files)
            assert(fs::exists(originals / file.relative) == corrupt);
    }
    std::cout << "move data: SHA-256 corruption/EOF/truncation/cancellation/empty catalog "
                 "preservation and real-file transactions PASS\n";
}
