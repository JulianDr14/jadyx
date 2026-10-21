// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/content/content_actions.h"
#include <algorithm>
#include <array>
#include <cstring>
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "jadyx/storage/uwp_rom_storage.h"

namespace Jadyx::Content {
namespace {
bool Managed(const Catalog& catalog, std::uint64_t application) {
    return std::find(catalog.managed_applications.begin(), catalog.managed_applications.end(), application) !=
           catalog.managed_applications.end();
}

std::string Word(Error error) {
    switch (error) {
    case Error::None: return {};
    case Error::Cancelled: return "interrupted";
    case Error::MissingKeys: return "keys";
    case Error::Incomplete: return "incomplete";
    case Error::HashMismatch: return "hash";
    case Error::WrongGame: return "wrong_game";
    case Error::Conflict: return "conflict";
    case Error::Space: return "space";
    case Error::Io: case Error::Registry: return "disk";
    case Error::Requirements: return "requirements";
    case Error::Unavailable: case Error::Unsupported: return "unsupported";
    default: return "invalid";
    }
}

/// Format from the package header, never from the browser's file name.
std::string Format(const FileSys::VirtualFile& file) {
    std::array<std::uint8_t, 4> magic{};
    if (file->ReadBytes(magic.data(), magic.size(), 0) == magic.size() && !std::memcmp(magic.data(), "PFS0", 4))
        return "nsp";
    if (file->ReadBytes(magic.data(), magic.size(), 0x100) == magic.size() && !std::memcmp(magic.data(), "HEAD", 4))
        return "xci";
    return {};
}
} // namespace

Error AdoptEmbeddedContent(ContentStore& store, Catalog& catalog, FileSys::VfsFilesystem& vfs,
                           std::uint64_t application, const std::string& base_path,
                           std::uint32_t base_version, std::stop_token stop, Progress progress) {
    if (!application || Managed(catalog, application)) return Error::None;
    // Discover embedded content once. Persist it too, so individual toggles have exact semantics.
    if (!base_path.empty()) {
        auto embedded = InspectPackage(vfs, base_path, false, stop);
        for (auto& item : embedded.contents) {
            if (stop.stop_requested()) return Error::Cancelled;
            if (item.error != Error::None || PlanImport(catalog.revisions, item.revision) != ImportAction::Add) continue;
            if (const auto error = ExecuteImport(store, catalog, vfs, item, false, true, stop, progress, base_version);
                error != Error::None) return error;
        }
    }
    if (stop.stop_requested()) return Error::Cancelled;
    catalog.managed_applications.push_back(application);
    return store.Save(catalog);
}

std::string ImportUploadedContent(const std::filesystem::path& package, Kind kind, std::uint64_t application,
                                  const std::string& base_path, std::stop_token stop, Progress progress) {
    application = FileSys::GetBaseTitleID(application);
    auto store = DefaultStore();
    const auto vfs = MakeUwpFilesystem();
    Catalog catalog;
    if (const auto error = store.Load(catalog); error != Error::None) return Word(error);
    store.Cleanup(catalog);
    const auto base_version = base_path.empty() ? 0 : PackageBaseVersion(*vfs, base_path, application);
    if (const auto error = AdoptEmbeddedContent(store, catalog, *vfs, application, base_path, base_version, stop);
        error != Error::None) return Word(error);

    const auto encoded = package.u8string();
    const std::string path{encoded.begin(), encoded.end()};
    std::string format;
    {
        FileSys::RealVfsFilesystem disk;
        const auto file = disk.OpenFile(path, FileSys::OpenMode::Read);
        if (!file) return "disk";
        format = Format(file);
    }
    if (format.empty()) return "nsp";
    auto inspected = InspectPackage(*vfs, path, false, stop, {}, format);
    if (inspected.error != Error::None) return Word(inspected.error);
    bool found = false;
    for (const auto& content : inspected.contents) {
        if (content.revision.kind != kind) continue;
        // Reject the whole package before installing any part of it.
        if (content.revision.application != application) return "wrong_game";
        found = true;
    }
    if (!found) return "kind";
    for (auto& content : inspected.contents) {
        if (stop.stop_requested()) return "interrupted";
        if (content.revision.kind != kind) continue;
        // Same choices as the content manager: internal copy, base present, never a downgrade.
        if (const auto error = ExecuteImport(store, catalog, *vfs, content, true, true, stop, progress, base_version);
            error != Error::None) return Word(error);
    }
    return {};
}

std::string RemoveContent(std::uint64_t application, const std::string& id) {
    const auto store = DefaultStore();
    Catalog catalog;
    if (const auto error = store.Load(catalog); error != Error::None) return Word(error);
    // The id alone is not enough: the browser may only delete what it listed for this game.
    if (const auto error = store.Remove(catalog, [&](const Revision& r) { return r.id == id && r.application == application; });
        error != Error::None) return error == Error::Invalid ? "content" : Word(error);
    return {};
}

std::string RemoveApplicationContent(std::uint64_t application) {
    const auto store = DefaultStore();
    Catalog catalog;
    if (const auto error = store.Load(catalog); error != Error::None) return Word(error);
    std::erase(catalog.managed_applications, application);
    // A reinstalled game discovers its embedded content again.
    auto error = store.Remove(catalog, [&](const Revision& r) { return r.application == application; });
    if (error == Error::Invalid) error = store.Save(catalog); // Nothing registered; still forget it.
    return Word(error);
}
} // namespace Jadyx::Content
