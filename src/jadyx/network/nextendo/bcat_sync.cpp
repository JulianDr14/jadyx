// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/network/nextendo/bcat_sync.h"

#include <condition_variable>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <system_error>
#include <thread>
#include <vector>

#include "core/online/nextendo/encoding.h"
#include "jadyx/mods/mod_archive.h"
#include "jadyx/network/http.h"
#include "jadyx/network/nextendo/bcat_package.h"
#include "jadyx/network/nextendo/pkce.h"

namespace Jadyx::Network::Nextendo {
namespace {

namespace fs = std::filesystem;
namespace Nx = Core::Online::Nextendo;

Http::ApiResponse Ask(std::uint64_t title_id, std::stop_token stop) {
    Http::ApiRequest request;
    request.url = BcatUrl(title_id);
    request.headers.emplace_back("X-Nextendo-Client-Id", std::string(kClientId));
    request.max_bytes = static_cast<std::size_t>(kBcatMaxBytes);
    return Http::Send(request, stop);
}

std::string ReadHash(const fs::path& cache) {
    std::ifstream file(cache / kBcatHashFile, std::ios::binary);
    std::string text;
    std::getline(file, text);
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text.size() == 64 ? text : std::string{};
}

/// The cache holds at least one directory: the files are there.
bool CachePresent(const fs::path& cache) {
    std::error_code error;
    for (fs::directory_iterator it{cache, error}, end; !error && it != end; it.increment(error)) {
        if (it->is_directory(error)) {
            return true;
        }
    }
    return false;
}

bool WriteAll(const fs::path& path, std::string_view bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

/// Removes a folder left by an earlier attempt; false when it is still there.
bool Clear(const fs::path& path) {
    std::error_code error;
    fs::remove_all(path, error);
    return !fs::exists(path, error);
}

BcatSyncResult Result(BcatSync outcome, std::string detail) {
    return {outcome, std::move(detail)};
}

BcatSyncResult Install(const std::string& zip_bytes, const std::string& hash, const fs::path& cache) {
    std::error_code error;
    const auto parent = cache.parent_path();
    const auto name = cache.filename().native();
    const auto zip = parent / (name + L".nextendo.zip");
    const auto staging = parent / (name + L".staging");
    const auto old = parent / (name + L".old");
    fs::create_directories(parent, error);
    struct Cleanup {
        fs::path zip, staging, old;
        ~Cleanup() {
            std::error_code ignored;
            fs::remove(zip, ignored);
            fs::remove_all(staging, ignored);
            fs::remove_all(old, ignored);
        }
    } cleanup{zip, staging, old};
    if (!Clear(staging) || !Clear(old) || !WriteAll(zip, zip_bytes)) {
        return Result(BcatSync::Kept, "cannot write next to the cache");
    }

    std::vector<std::unique_ptr<Mods::ArchiveSource>> volumes;
    volumes.push_back(Mods::OpenFileSource(zip));
    if (!volumes.back()) {
        return Result(BcatSync::Kept, "cannot reopen the package");
    }
    const auto listing = Mods::ListArchive(volumes);
    if (listing.error != Mods::ArchiveError::None || !listing.format.starts_with("ZIP")) {
        return Result(BcatSync::Refused, "not a readable ZIP (" + listing.format + ")");
    }
    std::vector<BcatEntry> entries;
    entries.reserve(listing.items.size());
    for (const auto& item : listing.items) {
        entries.push_back({item.path, item.size});
    }
    const auto plan = PlanBcatPackage(entries);
    if (plan.error != BcatPackageError::None) {
        return Result(BcatSync::Refused, BcatPackageErrorName(plan.error));
    }
    std::vector<Mods::LayoutFile> files;
    files.reserve(plan.files.size());
    for (const auto& file : plan.files) {
        files.push_back({file.path, file.directory + "/" + file.name, file.size});
    }
    // A fresh source: the listing read the first one to its end.
    volumes.clear();
    volumes.push_back(Mods::OpenFileSource(zip));
    if (!volumes.back()) {
        return Result(BcatSync::Kept, "cannot reopen the package");
    }
    std::string detail;
    const auto extracted = Mods::ExtractArchive(volumes, files, staging, kBcatMaxBytes, {}, {}, &detail);
    if (extracted != Mods::ArchiveError::None) {
        return Result(extracted == Mods::ArchiveError::Write ? BcatSync::Kept : BcatSync::Refused,
                      "unpack failed: " + detail);
    }
    if (!WriteAll(staging / kBcatHashFile, hash + "\n")) {
        return Result(BcatSync::Kept, "cannot write the package hash");
    }

    // Swap: the old cache steps aside, the new one takes its name, the old one goes.
    const bool had_cache = fs::exists(cache, error);
    if (had_cache) {
        fs::rename(cache, old, error);
        if (error) {
            return Result(BcatSync::Kept, "the cache is in use");
        }
    }
    fs::rename(staging, cache, error);
    if (error) {
        std::error_code ignored;
        if (had_cache) {
            fs::rename(old, cache, ignored);
        }
        return Result(BcatSync::Kept, "cannot move the new cache in place");
    }
    return Result(BcatSync::Installed, std::to_string(plan.directories.size()) + " directories, " +
                                           std::to_string(plan.files.size()) + " files, " +
                                           std::to_string(plan.total_bytes) + " bytes");
}

} // namespace

BcatSyncResult SyncBcat(std::uint64_t title_id, const fs::path& cache, std::chrono::seconds budget) {
    // The whole exchange ends when the budget does: the boot never waits longer.
    std::stop_source deadline;
    std::jthread timer([&deadline, budget](std::stop_token done) {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock{mutex};
        if (!wake.wait_for(lock, done, budget, [] { return false; }) && !done.stop_requested()) {
            deadline.request_stop();
        }
    });

    const auto sibling = BcatSiblingTitle(title_id);
    auto response = Ask(title_id, deadline.get_token());
    auto answer = ClassifyBcatAnswer(response.status, sibling.has_value());
    if (answer == BcatAnswer::TrySibling) {
        response = Ask(*sibling, deadline.get_token());
        answer = ClassifyBcatAnswer(response.status, false);
    }
    timer.request_stop();
    if (answer != BcatAnswer::Package) {
        if (response.status == 204 || response.status == 404) {
            return Result(BcatSync::NothingPublished, "status " + std::to_string(response.status));
        }
        return Result(BcatSync::Kept, response.status == 0
                                          ? std::string("no answer")
                                          : "status " + std::to_string(response.status));
    }
    const auto& body = response.body;
    const auto digest = Nx::Sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    const auto hash = Nx::HexEncode(digest);
    if (!BcatNeedsInstall(ReadHash(cache), hash, CachePresent(cache))) {
        return Result(BcatSync::UpToDate, hash.substr(0, 12));
    }
    return Install(body, hash, cache);
}

const char* BcatSyncName(BcatSync outcome) {
    switch (outcome) {
    case BcatSync::NothingPublished:
        return "nothing published";
    case BcatSync::UpToDate:
        return "up to date";
    case BcatSync::Installed:
        return "installed";
    case BcatSync::Kept:
        return "kept the local cache";
    case BcatSync::Refused:
        return "refused the package";
    }
    return "?";
}

} // namespace Jadyx::Network::Nextendo
