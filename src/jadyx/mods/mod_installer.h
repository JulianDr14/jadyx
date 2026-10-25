// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One game's installed mods on disk. Mods live where the loader reads them, load/<title id>/<mod>;
// the manager's own files (the manifests, the staging and trash folders) live beside them under
// mods/, on the same volume, so every step that the loader can see is a rename:
//  - an install extracts into mods/staging/ and renames the finished folder into place, so a game
//    never boots with half a mod, and a crash leaves only staging behind, cleared on the next open;
//  - an uninstall renames the folder into mods/trash/ before deleting it, so it is gone for the
//    loader at once even if deleting thousands of files is cut short.
// A save from an archive goes over the game's save instead, after the save it replaces is copied
// to mods/save-backups/.

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

#include "jadyx/mods/mod_archive.h"
#include "jadyx/mods/mod_layout.h"
#include "jadyx/mods/mod_model.h"

namespace Jadyx::Mods {

enum class ModError : std::uint8_t {
    None,
    NotAMod,    ///< The archive has nothing the loader reads.
    Encrypted,  ///< The archive asks for a password.
    Unreadable, ///< Damaged, not an archive, or a volume is missing.
    TooLarge,
    Space,      ///< Not enough free space for it.
    Write,      ///< The files could not be written or moved.
    Cancelled,
    Unavailable, ///< The source (a USB drive, a file) went away.
    Network,    ///< A download failed: no connection, or the site did not answer.
    Checksum,   ///< A download is not the file the site describes.
    NeedsSkyline,    ///< Only Skyline plugins, which the emulator does not load.
    NeedsArcropolis, ///< Made for ARCropolis, which needs Skyline.
    NoSave,     ///< A save for a game never played: there is no save of it to put it in.
};

ModError FromArchiveError(ArchiveError error);

/// Where mods and the manager's files are (both under the emulator's folder in LocalState).
struct ModPaths {
    std::filesystem::path load;  ///< load/: the loader's mod root.
    std::filesystem::path data;  ///< mods/: manifests, staging, trash.
    std::filesystem::path saves; ///< nand/: the emulated saves.
};

/// The game's title id as the loader names its folder: 16 upper-case hex digits.
std::string TitleFolder(std::uint64_t title_id);

class ModLibrary {
public:
    /// `reserved` names folders under the game's load folder that other pages own (the online
    /// cheats'): they are never listed as mods, and never taken by one.
    ModLibrary(ModPaths paths, std::uint64_t title_id, std::vector<std::string> reserved = {});

    /// The game's mods in priority order, after matching the manifest to the folders on disk
    /// (and saving it when that changed). Also clears what a cut install or uninstall left.
    std::vector<ModRecord> Load(std::int64_t now);
    bool Save(const std::vector<ModRecord>& mods) const;

    std::filesystem::path FolderPath(const std::string& folder) const;

    /// Extracts each candidate of the archive into its own new mod and adds it to `mods`, at the
    /// top: the newest install wins its conflicts until the player says otherwise. `done` counts
    /// the candidates installed before an error.
    ModError Install(std::vector<std::unique_ptr<ArchiveSource>>& volumes,
                     const std::vector<ModCandidate>& chosen, const ModSource& source,
                     std::vector<ModRecord>& mods, std::int64_t now, std::stop_token stop,
                     const ArchiveProgress& progress, std::size_t& done, std::string& detail);

    /// Deletes the mod's folder and its record.
    ModError Remove(std::vector<ModRecord>& mods, const std::string& folder);

    /// An update: puts mod `fresh` where `old` is, in the order and on or off like it, then
    /// uninstalls `old`. What the player chose for a mod survives its new versions.
    ModError Replace(std::vector<ModRecord>& mods, const std::string& old, const std::string& fresh);

    /// Writes a save candidate's files over the game's save (the profile's that was played
    /// last), keeping its other files. The save as it was is first copied to mods/save-backups/.
    ModError InstallSave(std::vector<std::unique_ptr<ArchiveSource>>& volumes, const ModCandidate& save,
                         std::stop_token stop, const ArchiveProgress& progress, std::string& detail);

    /// The game's save of the profile that played it last; empty when it has none.
    std::filesystem::path SaveFolder() const;

    std::uint64_t FreeSpace() const;

private:
    std::filesystem::path TitleRoot() const;
    std::filesystem::path ManifestPath() const;
    std::filesystem::path NewScratch(const char* kind) const;
    void ClearScratch() const;
    std::vector<std::string> ModFolders() const;

    ModPaths paths;
    std::uint64_t title_id;
    std::vector<std::string> reserved;
};

} // namespace Jadyx::Mods
