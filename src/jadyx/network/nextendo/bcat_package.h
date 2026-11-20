// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pure rules of Nextendo's BCAT packages (https://wiki.nextendo.network/bcat): before a game
// boots, https://nextendo.network/api/bcat/<base title id> answers 200 with a ZIP that is the
// title's whole delivery cache, or 204 when nothing is published. The ZIP holds exactly
// <directory>/<file> entries whose names fit the 32-byte DirectoryName/FileName; it replaces the
// title's folder (NAND system/save/bcat/<TID>), which the emulated bcat services already serve.
// The SHA-256 of the last installed ZIP is kept next to the files, so an unchanged package is not
// written again. No platform calls, so the desktop gate covers it.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Jadyx::Network::Nextendo {

inline constexpr std::size_t kBcatMaxEntries = 512;
inline constexpr std::uint64_t kBcatMaxBytes = 32ull << 20; // unpacked, and the ZIP itself
/// Next to the cache directories; the services list directories only, so games never see it.
inline constexpr std::string_view kBcatHashFile = ".nextendo_bcat_hash";

/// https://nextendo.network/api/bcat/<16 lower-case hex digits>.
std::string BcatUrl(std::uint64_t title_id);

/// Titles whose package is published under another title's ID (one event shared by two
/// editions). The package still installs into the asking title's own folder.
std::optional<std::uint64_t> BcatSiblingTitle(std::uint64_t title_id);

/// What an answer from the BCAT route means.
enum class BcatAnswer : std::uint8_t {
    Package,     ///< 200: a package to check and maybe install.
    TrySibling,  ///< 204 or 404 for a title with a sibling: ask again with the sibling's ID.
    KeepLocal,   ///< 204, a network error, a timeout or any other status: keep what is there.
};
BcatAnswer ClassifyBcatAnswer(unsigned status, bool has_sibling);

/// 1 to 31 characters from A-Z a-z 0-9 _ - . (and not "." or "..").
bool ValidBcatName(std::string_view name);

struct BcatEntry {
    std::string path; ///< As the archive lists it ('/' separated).
    std::uint64_t size{};
};

struct BcatFile {
    std::string path; ///< The archive path, to extract it.
    std::string directory;
    std::string name;
    std::uint64_t size{};
};

enum class BcatPackageError : std::uint8_t {
    None,
    Empty,          ///< No file at all.
    BadPath,        ///< Not exactly <directory>/<file> (root files, deeper paths, absolute, "..").
    BadName,        ///< A name outside the allowed characters or longer than 31.
    Duplicate,      ///< The same path twice (compared without case, as the disk does).
    TooManyEntries, ///< More than kBcatMaxEntries.
    TooLarge,       ///< More than kBcatMaxBytes unpacked.
};

struct BcatPlan {
    BcatPackageError error = BcatPackageError::None;
    std::vector<BcatFile> files;
    std::vector<std::string> directories; ///< Each once, in first-seen order.
    std::uint64_t total_bytes{};
};

/// Checks a listed package and says what to write. Any error refuses the whole package.
BcatPlan PlanBcatPackage(const std::vector<BcatEntry>& entries);

const char* BcatPackageErrorName(BcatPackageError error);

/// The package must be installed: no hash stored, another hash, or the cache is gone.
bool BcatNeedsInstall(std::string_view stored_hash, std::string_view package_hash,
                      bool cache_present);

} // namespace Jadyx::Network::Nextendo
