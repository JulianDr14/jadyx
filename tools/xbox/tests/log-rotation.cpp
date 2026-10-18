// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Logs appended across launches stay bounded: past the cap the log becomes its .old copy and a new
// file starts; below it nothing moves.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include "common/log_rotation.h"

namespace fs = std::filesystem;

namespace {
void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

void Append(const fs::path& path, std::size_t bytes, char fill) {
    std::ofstream file{path, std::ios::binary | std::ios::app};
    file << std::string(bytes, fill);
}

std::string Read(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, {}};
}
} // namespace

int main(int, char** argv) {
    const fs::path dir = fs::path{argv[0]}.parent_path() / "log-rotation-work";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path log = dir / "jadyx_diag.txt";
    const fs::path old_log = dir / "jadyx_diag.old.txt";
    Check(Common::RotatedLogPath(log) == old_log, "old name");

    Check(!Common::RotateLogIfLarger(log, 100), "missing log is left alone");
    Append(log, 100, 'a');
    Check(!Common::RotateLogIfLarger(log, 100), "at the cap stays");
    Check(fs::file_size(log) == 100 && !fs::exists(old_log), "nothing moved");

    // Launches keep appending; each rotation keeps exactly the previous history.
    Append(log, 1, 'b');
    Check(Common::RotateLogIfLarger(log, 100), "past the cap rotates");
    Check(!fs::exists(log) && Read(old_log) == std::string(100, 'a') + "b", "history kept");
    Append(log, 150, 'c');
    Check(Common::RotateLogIfLarger(log, 100), "second rotation");
    Check(Read(old_log) == std::string(150, 'c'), "older history replaced");
    for (int launch = 0; launch < 50; ++launch) {
        Common::RotateLogIfLarger(log, 100);
        Append(log, 60, 'd');
        Check(fs::file_size(log) <= 160 && fs::file_size(old_log) <= 160, "disk use bounded");
    }
    fs::remove_all(dir);
    std::printf("PASS log-rotation\n");
    return 0;
}
