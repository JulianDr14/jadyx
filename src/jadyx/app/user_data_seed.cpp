// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <string>
#include <system_error>

#include <windows.h>

#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "jadyx/app/boot_diag.h"
#include "jadyx/app/user_data_seed.h"

namespace Jadyx {

namespace {

// ---- User data seeding ------------------------------------------------------------------------
// Data for a local test reaches the console inside a local-only package, never the repo, under
// InstalledLocation\userdata (package-appx.ps1 -Game, or what a local -StageScript adds). Eden
// reads keys and firmware from its writable user dir, so they are copied into LocalState once;
// LocalState survives package updates, so later packages can leave them out.
//   userdata\keys\*      -> LocalState\jadyx\keys
//   userdata\firmware\*  -> LocalState\jadyx\nand\system\Contents\registered
//   userdata\games\*     -> LocalState\games
// A file is copied when it is missing or its size differs.
void SeedDirectory(const std::filesystem::path& from, const std::filesystem::path& to,
                   const char* what) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(from, ec)) {
        return;
    }
    fs::create_directories(to, ec);
    u32 copied = 0;
    u32 kept = 0;
    u64 bytes = 0;
    const ULONGLONG start = GetTickCount64();
    for (const fs::directory_entry& entry : fs::directory_iterator(from, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const fs::path target = to / entry.path().filename();
        const u64 size = entry.file_size(ec);
        std::error_code size_ec;
        if (fs::exists(target, size_ec) && fs::file_size(target, size_ec) == size && !size_ec) {
            ++kept;
            continue;
        }
        std::error_code copy_ec;
        fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            WriteDiag(std::string("seed ") + what + ": FAILED copying " +
                      entry.path().filename().string() + ": " + copy_ec.message());
            continue;
        }
        ++copied;
        bytes += size;
    }
    WriteDiag(std::string("seed ") + what + ": " + std::to_string(copied) + " copied (" +
              std::to_string(bytes >> 20) + " MiB in " + std::to_string(GetTickCount64() - start) +
              " ms), " + std::to_string(kept) + " already there");
}

} // namespace

void SeedUserData(const std::filesystem::path& install, const std::filesystem::path& local) {
    const std::filesystem::path userdata = install / "userdata";
    // The data folders the core writes into without creating them (play_time, ...), as the
    // desktop frontends do at startup.
    Common::FS::CreateEdenPaths();
    // Through the path manager, which knows the data folder.
    const auto data = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir);
    SeedDirectory(userdata / "keys", data / "keys", "keys");
    SeedDirectory(userdata / "firmware", data / "nand" / "system" / "Contents" / "registered",
                  "firmware");
    SeedDirectory(userdata / "games", local / "games", "games");
}

} // namespace Jadyx
