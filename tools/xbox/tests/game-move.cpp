// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <iostream>
#include <limits>
#include "jadyx/storage/verified_move.h"

int main() {
    using namespace Jadyx::VerifiedMove;
    const std::vector<File> files{{.relative = "base", .bytes = 16},
                                  {.relative = "update", .bytes = 8},
                                  {.relative = "dlc-disabled", .bytes = 4}};
    for (int fault = 0; fault != 10; ++fault) {
        unsigned copied{}, verified{}, removed{};
        bool published{}, committed{};
        IO io;
        io.copy = [&](const File&) { return ++copied != (fault == 1 ? 2U : 99U); };
        io.verify = [&](const File&) {
            ++verified;
            return verified != (fault == 2 ? 2U : fault == 3 ? 5U : 99U);
        };
        io.publish = [&] {
            published = fault != 4;
            return published;
        };
        io.commit = [&] {
            assert(published && verified == 6);
            committed = fault != 5;
            return committed;
        };
        io.remove = [&](const File&) {
            assert(committed);
            ++removed;
            return fault != 6;
        };
        io.cancelled = [&] { return fault == 7 || (fault == 8 && copied == 3); };
        const auto result =
            Run(files, fault == 9 ? std::optional<std::uint64_t>{} : Reserve + 28, io);
        if (fault == 0)
            assert(result == Result::Success && removed == 3);
        else if (fault == 6)
            assert(result == Result::CleanupIncomplete && removed == 1);
        else
            assert(result != Result::Success && removed == 0);
    }
    IO never;
    assert(Run(files, Reserve + 27, never) == Result::Space);
    assert(Run({}, Reserve, never) == Result::Invalid);
    assert(Run({{.relative = "x", .bytes = std::numeric_limits<std::uint64_t>::max()}}, Reserve,
               never) == Result::Invalid);
    assert(Run({{.relative = "same", .bytes = 1}, {.relative = "same", .bytes = 2}}, Reserve + 3,
               never) == Result::Invalid);
    assert(Run({{.relative = "Base.nsp", .bytes = 1}, {.relative = "base.nsp", .bytes = 2}},
               Reserve + 3, never) == Result::Invalid);
    assert(Run({{.relative = "../escape", .bytes = 1}}, Reserve + 1, never) == Result::Invalid);
    assert(Run({{.relative = "base:ads", .bytes = 1}}, Reserve + 1, never) == Result::Invalid);
    assert(Run({{.relative = "base//x", .bytes = 1}}, Reserve + 1, never) == Result::Invalid);
    std::cout << "verified move: 18 transaction cases PASS\n";
}
