// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include "jadyx/storage/ftp/ftp_safety.h"

int main() {
    using namespace Jadyx::Ftp;
    using namespace std::chrono_literals;
    assert(ParseAllocation("0") == 0);
    assert(ParseAllocation("12884901925") == 12884901925ULL);
    assert(ParseAllocation("18446744073709551615") == std::numeric_limits<std::uint64_t>::max());
    for (const auto* invalid : {"", "-1", "+1", "1 x", "1 ", "18446744073709551616"})
        assert(!ParseAllocation(invalid));

    // The production publication gate must never call the replacement on
    // uncertain completion.
    for (const auto expected : {std::optional<std::uint64_t>{}, {10}}) {
        bool published = false;
        try {
            CompleteUpload(expected, 4, [] { return true; }, [&] { published = true; }, [] {});
        } catch (const std::runtime_error&) {
        }
        assert(!published);
    }
    for (const auto bytes : {9ULL, 11ULL}) {
        bool published = false;
        try {
            CompleteUpload(10, bytes, [] { return true; }, [&] { published = true; }, [] {});
        } catch (const std::runtime_error&) {
        }
        assert(!published);
    }
    bool published = false;
    try {
        CompleteUpload(10, 10, [] { return false; }, [&] { published = true; }, [] {});
    } catch (const std::runtime_error&) {
    }
    assert(!published);
    CompleteUpload(10, 10, [] { return true; }, [&] { published = true; }, [] {});
    assert(published);
    CompleteUpload(0, 0, [] { return true; }, [] {}, [] {});

    const auto directory =
        std::filesystem::temp_directory_path() /
        ("jadyx-ftp-safety-" +
         std::to_string(SessionDeadline::Clock::now().time_since_epoch().count()));
    assert(std::filesystem::create_directory(directory));
    const auto target = directory / "existing.bin";
    const auto partial = directory / "existing.bin.part";
    const auto contents = [](const auto& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    // Clean data EOF, missing declaration, control loss/ABOR and commit failure.
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        std::ofstream(target, std::ios::binary) << "good-original";
        std::ofstream(partial, std::ios::binary) << "new-data";
        bool rejected = false;
        try {
            CompleteUpload(
                scenario == 1 ? std::nullopt : std::optional<std::uint64_t>{8},
                scenario == 0 ? 4 : 8, [&] { return scenario != 2; },
                [&] {
                    if (scenario == 3)
                        throw std::runtime_error("commit failed");
                    if (scenario == 4)
                        throw std::runtime_error("replace failed");
                    std::filesystem::rename(partial, target);
                },
                [&] { std::filesystem::remove(partial); });
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected == (scenario != 5));
        assert(contents(target) == (scenario == 5 ? "new-data" : "good-original"));
        assert(!std::filesystem::exists(partial));
    }
    std::filesystem::remove_all(directory);

    const SessionDeadline::Time start{};
    SessionDeadline login(start);
    assert(!login.Expired(start + 29s));
    login.Activity(start + 29s); // Even a partial/unauthenticated command cannot extend login.
    assert(login.Expired(start + 30s));
    SessionDeadline idle(start);
    idle.Login(true, start + 10s);
    assert(!idle.Expired(start + 309s));
    assert(idle.Expired(start + 310s));
    idle.Activity(start + 309s);
    assert(!idle.Expired(start + 608s));
    assert(idle.Expired(start + 609s));
    idle.Login(false, start + 610s);
    assert(idle.Expired(start + 640s));

    LoginBackoff peers;
    assert(peers.Failure("192.168.1.2") == 1s);
    // The same shared limiter is used by subsequent sessions, independent of
    // credentials.
    auto& reconnect = peers;
    assert(reconnect.Failure("192.168.1.2") == 2s);
    assert(peers.Failure("192.168.1.3") == 1s);
    for (const auto seconds : {4, 8, 16, 30, 30})
        assert(reconnect.Failure("192.168.1.2") == std::chrono::seconds(seconds));
    for (unsigned i = 0; i < 2048; ++i)
        (void)peers.Failure(std::to_string(i));
    assert(peers.Failure("new-peer-after-capacity") == 30s);
    assert(peers.Failure("192.168.1.2") == 30s);

    assert(CanRenameTo("games/existing.bin", true));
    assert(CanRenameTo("GAMES/existing.bin", true));
    for (const auto* rename_target :
         {"keys/existing.bin", "saves/a/existing.bin", "firmware/a.bin"}) {
        assert(!CanRenameTo(rename_target, true));
        assert(CanRenameTo(rename_target, false));
    }
}
