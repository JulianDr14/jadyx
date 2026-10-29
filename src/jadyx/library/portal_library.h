// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What Jadyx Portal shows of the library: the scanned games, their covers from the library
// cache and the updates/DLC registered for each, plus the storage jobs the browser asks for
// (installing an update/DLC, deleting content or a game). The coordinator publishes the scan
// from the UI thread and runs installs, removals and save exports on its storage worker; portal
// workers only read copies, so no browser request touches the library state.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>
#include "jadyx/content/content_inspector.h"
#include "jadyx/library/game_library.h"
#include "jadyx/library/game_removal.h"
#include "jadyx/library/library_cache.h"
#include "jadyx/storage/network_portal.h"

namespace Jadyx {

/// The application of a content upload's "<kind>:<application hex>" tag; 0 when malformed.
std::uint64_t ContentTagApplication(const std::string& tag);
class PortalLibrarySource {
public:
    PortalLibrarySource(std::filesystem::path root, std::filesystem::path cache_directory);
    PortalLibrarySource(const PortalLibrarySource&) = delete;
    PortalLibrarySource& operator=(const PortalLibrarySource&) = delete;

    /// Games with a known title and program id, one per application; a dump whose metadata is being
    /// read again keeps its previous card. Cheap when nothing changed.
    void Publish(const std::vector<LibraryEntry>& entries, std::uint32_t language);
    /// Callbacks for NetworkPortal::SetLibrary; this object must outlive the portal.
    PortalLibrary View();
    /// Bytes installed by the running portal import; reported to the browser in MiB.
    void Progress(std::uint64_t done, std::uint64_t total) { installed = done; install_total = total; }
    /// Installs a portal update/DLC upload; `tag` is its "<kind>:<application hex>".
    /// `progress` also hears the bytes copied. Returns a portal error word.
    std::string Install(const std::filesystem::path& package, const std::string& tag, std::stop_token stop,
                        const Content::Progress& progress = {});
    /// Carries out a deletion asked from the portal. Returns a portal error word.
    std::string Remove(const PortalRemoval& removal);
    /// Writes a bounded ZIP of the current user's account save for a listed game.
    std::string ExportSave(std::uint64_t application, const std::filesystem::path& output,
                           std::stop_token stop,
                           const std::function<void(std::uint64_t, std::uint64_t)>& progress);

private:
    struct Game {
        PortalGame info;
        std::string launch_path; ///< The first dump: the base for its updates/DLC and its cover.
        std::filesystem::path relative_path;
        GameFiles files;
        bool operator==(const Game&) const = default;
    };
    std::optional<Game> Find(std::uint64_t application) const;
    std::optional<std::string> Cover(std::uint64_t application);

    std::filesystem::path root;
    const LibraryCache cache;
    std::atomic<std::uint64_t> installed{}, install_total{};
    mutable std::mutex mutex;
    std::vector<Game> games;
    std::uint32_t language{};
    std::uint64_t revision{};
};

} // namespace Jadyx
