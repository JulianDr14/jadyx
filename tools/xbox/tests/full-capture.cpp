// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "video_core/diagnostics/full_capture.h"
#include "video_core/diagnostics/full_capture_archive.h"

using namespace VideoCommon::FullCapture;
void Check(bool pass) { if (!pass) throw std::runtime_error("full capture gate failed"); }
int main(int argc, char** argv) {
    Check(argc == 2);
    const std::filesystem::path root{argv[1]};
    Archive archive;
    const std::array<std::uint8_t, 8> padded{1, 2, 99, 98, 3, 4, 97, 96};
    const std::array<std::uint8_t, 4> packed{1, 2, 3, 4};
    const std::array<std::uint8_t, 4> changed{1, 2, 3, 5};
    Check(archive.Begin(root / "complete"));
    Check(!archive.Begin(root / "must_not_replace"));
    archive.Data("padded", "{}", padded, 2, 4, 2);
    archive.Data("packed", "{}", packed);
    archive.Data("changed", "{}", changed);
    std::vector<std::uint8_t> large(2 * 65536);
    for (std::size_t i = 0; i < large.size(); ++i) large[i] = static_cast<std::uint8_t>(i * 5 + i / 65536);
    archive.Data("large", "{}", large);
    archive.Data("large_repeat", "{}", large);
    large[65000] ^= 1;
    archive.Data("large_one_byte_change", "{}", large);
    archive.Event("escaped\"\n", "{\"valid\":true}");
    archive.End(true);
    Check(!archive.Begin(root / "complete"));
    Check(std::filesystem::file_size(root / "complete/blobs.bin") == 8 + 3 * 65536);
    Check(archive.Begin(root / "incomplete"));
    archive.Data("bad_pitch", "{}", padded, 3, 2, 2);
    archive.Data("bad_rows", "{}", padded, 2, 4, 3);
    archive.Data("overflow", "{}", padded, 4, 4, SIZE_MAX);
    archive.Omit("test", "{}");
    archive.End(false);
    Check(archive.Begin(root / "interrupted"));
    archive.Event("test", "{}");
    Check(!std::filesystem::exists(root / "interrupted/summary.json"));
    archive.End(false);

    Enable(false);
    Check(!Begin(root / "disabled"));
    Enable(true);
    Check(Begin(root / "concurrent"));
    std::array<std::thread, 4> threads;
    for (auto& thread : threads) thread = std::thread([&] {
        for (int i = 0; i < 100; ++i) Data("worker", "{}", packed);
    });
    for (auto& thread : threads) thread.join();
    End(true);
    Check(!Active());
    Check(std::filesystem::file_size(root / "concurrent/blobs.bin") == 4);
    std::cout << "PASS: full bytes, pitch, exact dedup, changed bytes, invalid ranges, "
                 "interruptions, no overwrite, opt-in and concurrent writer\n";
}
