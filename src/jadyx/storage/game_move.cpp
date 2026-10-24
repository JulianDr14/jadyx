// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/game_move.h"

#include <map>
#include <memory>
#include <stdexcept>
#include <fmt/format.h>
#include <windows.h>

#include <fileapifromapp.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Storage.Search.h>
#include <winrt/Windows.Storage.Streams.h>
#include "common/logging.h"
#include "core/file_sys/vfs/vfs.h"
#include "jadyx/content/content_service.h"
#include "jadyx/storage/game_move_catalog.h"
#include "jadyx/storage/move_digest.h"
#include "jadyx/storage/move_recovery.h"
#include "jadyx/storage/storage_path.h"
#include "jadyx/storage/uwp_rom_storage.h"
#include "jadyx/storage/verified_move.h"

namespace Jadyx {
namespace {
namespace fs = std::filesystem;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;

std::string Utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
bool Inside(const fs::path& root, const fs::path& path) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && relative.is_relative() && *relative.begin() != ".." &&
           relative != ".";
}
// Refuse links/junctions at EVERY component, not just at the final file.
bool SafeFile(const fs::path& root, const fs::path& path) {
    if (!Inside(root, path))
        return false;
    auto current = root;
    for (const auto& part : path.lexically_relative(root)) {
        current /= part;
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
    }
    return fs::is_regular_file(path);
}
struct Source {
    fs::path path;
    HANDLE handle{INVALID_HANDLE_VALUE};
    VerifiedMove::Digest digest{};
    ~Source() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
    std::size_t Read(std::span<unsigned char> buffer, std::uint64_t offset) const {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        DWORD count{};
        if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) ||
            !ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
            throw std::runtime_error("Game move source read failed");
        return count;
    }
};
StorageFolder Parent(StorageFolder folder, std::string_view relative, bool create) {
    for (auto slash = relative.find('/'); slash != std::string_view::npos;
         slash = relative.find('/')) {
        const auto name = winrt::to_hstring(relative.substr(0, slash));
        folder = create
                     ? folder.CreateFolderAsync(name, CreationCollisionOption::OpenIfExists).get()
                     : folder.GetFolderAsync(name).get();
        relative.remove_prefix(slash + 1);
    }
    return folder;
}
std::string Leaf(std::string_view path) {
    return std::string{path.substr(path.rfind('/') + 1)};
}
bool VerifyOutput(const StorageFolder& folder, const std::string& token,
                  const VerifiedMove::File& file, const VerifiedMove::Digest& expected,
                  std::stop_token stop) {
    const auto parent = Parent(folder, file.relative, false);
    const auto output = parent.GetFileAsync(winrt::to_hstring(Leaf(file.relative))).get();
    const auto path = MakeStoragePath(token, winrt::to_string(folder.Name()) + "/" + file.relative);
    auto vfs = MakeUwpFilesystem();
    auto reopened = vfs->OpenFile(path, FileSys::OpenMode::Read);
    if (!reopened || reopened->GetSize() != file.bytes ||
        output.GetBasicPropertiesAsync().get().Size() != file.bytes)
        return false;
    const auto digest = VerifiedMove::Hash(
        [&](auto buffer, auto offset) {
            return reopened->Read(buffer.data(), buffer.size(), static_cast<std::size_t>(offset));
        },
        file.bytes, stop);
    return digest && *digest == expected;
}
} // namespace

void CleanupGameMoveStaging(const StorageFolder& destination, std::stop_token stop) {
    if (!MoveToUsbEnabled())
        return;
    try {
        // Snapshot names before deleting: removal must not shift the broker's page offsets.
        std::vector<std::string> names;
        constexpr unsigned page_size = 64;
        constexpr unsigned max_folders = 10000;
        for (unsigned start = 0; start < max_folders && !stop.stop_requested();
             start += page_size) {
            const auto folders =
                destination
                    .GetFoldersAsync(
                        winrt::Windows::Storage::Search::CommonFolderQuery::DefaultQuery, start,
                        std::min(page_size, max_folders - start))
                    .get();
            for (const auto& folder : folders)
                names.push_back(winrt::to_string(folder.Name()));
            if (folders.Size() < page_size)
                break;
        }
        VerifiedMove::CleanupStaging(
            names,
            [&](const std::string& name) {
                try {
                    const auto folder = destination.GetFolderAsync(winrt::to_hstring(name)).get();
                    const auto attributes = GetFileAttributesW(folder.Path().c_str());
                    if (attributes == INVALID_FILE_ATTRIBUTES ||
                        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                        return;
                    folder.DeleteAsync(StorageDeleteOption::PermanentDelete).get();
                    LOG_INFO(Frontend, "Game move: removed interrupted staging={}", name);
                } catch (const winrt::hresult_error& error) {
                    LOG_WARNING(Frontend, "Game move: retained staging={} reason={}", name,
                                winrt::to_string(error.message()));
                }
            },
            [&] { return stop.stop_requested(); });
    } catch (const winrt::hresult_error& error) {
        LOG_WARNING(Frontend, "Game move: staging recovery unavailable reason={}",
                    winrt::to_string(error.message()));
    }
}

std::string MoveInternalGame(const fs::path& games, const LibraryEntry& game,
                             const StorageFolder& destination, std::stop_token stop,
                             const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    using namespace VerifiedMove;
    StorageFolder staging{nullptr};
    bool published = false;
    bool committed = false;
    try {
        if (!MoveToUsbEnabled() || IsStoragePath(game.launch_path) || !game.program_id ||
            !game.pulse_saved.empty())
            return "invalid";
        const auto source_path = fs::u8path(game.launch_path).lexically_normal();
        const auto root = fs::weakly_canonical(games);
        if (!SafeFile(root, source_path))
            return "invalid";
        const fs::path target_path{destination.Path().c_str()};
        // Never authorize/copy back into the application's own data tree.
        const auto normalized_target = target_path.lexically_normal();
        if (target_path.empty() || normalized_target == root.parent_path() ||
            Inside(root.parent_path(), normalized_target))
            return "invalid";
        const auto application = game.program_id & ~std::uint64_t{0x1fff};
        auto store = Content::DefaultStore();
        Content::Catalog catalog;
        if (store.Load(catalog) != Content::Error::None)
            return "registry";
        const auto bundle = fmt::format("Jadyx-{:016X}", application);
        const auto existing = destination.TryGetItemAsync(winrt::to_hstring(bundle)).get();
        if (existing && !existing.IsOfType(StorageItemTypes::Folder))
            return "conflict";
        std::vector<File> files;
        std::map<std::string, std::shared_ptr<Source>> sources;
        std::map<std::string, std::string> objects;
        std::uint64_t total{};
        const auto add = [&](const fs::path& source, std::string relative,
                             const fs::path& allowed) {
            if (!ValidStorageRelative(relative) || !SafeFile(allowed, source) ||
                files.size() >= 4096)
                throw std::runtime_error("Unsafe or excessive game move manifest");
            auto item = std::make_shared<Source>();
            item->path = source;
            // Deny writes, rename and deletion throughout copy, verification and publication.
            item->handle = CreateFile2FromAppW(source.c_str(), GENERIC_READ | DELETE,
                                               FILE_SHARE_READ, OPEN_EXISTING, nullptr);
            if (item->handle == INVALID_HANDLE_VALUE)
                throw std::runtime_error("Game move source locked");
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(item->handle, &size) || size.QuadPart < 0)
                throw std::runtime_error("Game move source size unavailable");
            const auto bytes = static_cast<std::uint64_t>(size.QuadPart);
            if (bytes > (std::numeric_limits<std::uint64_t>::max)() - total - Reserve)
                throw std::runtime_error("Game move size overflow");
            total += bytes;
            const auto digest = Hash(
                [&](auto buffer, auto offset) { return item->Read(buffer, offset); }, bytes, stop);
            if (!digest)
                throw std::runtime_error("Game move source hashing failed or cancelled");
            item->digest = *digest;
            if (!sources.emplace(relative, item).second)
                throw std::runtime_error("Duplicate game move target");
            files.push_back(File{.relative = std::move(relative), .bytes = bytes});
        };
        const auto base_relative = "base/" + Utf8(source_path.filename());
        add(source_path, base_relative, root);
        if (files.front().bytes == 0)
            return "invalid";
        for (const auto& revision : catalog.revisions) {
            if (revision.application != application || !revision.internal)
                continue;
            if (!Content::ValidId(revision.id))
                return "invalid";
            const auto object = store.Object(revision.id);
            // Refuse a missing/empty object instead of silently dropping an update or DLC.
            const auto before = files.size();
            for (const auto& entry : fs::recursive_directory_iterator{object}) {
                if (entry.is_symlink() ||
                    (GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))
                    return "invalid";
                if (entry.is_directory())
                    continue;
                add(entry.path(),
                    "content/" + revision.id + "/" + Utf8(entry.path().lexically_relative(object)),
                    store.Root());
            }
            if (before == files.size())
                return "invalid";
            objects.emplace(revision.id, "content/" + revision.id);
        }
        if (stop.stop_requested())
            return "cancelled";
        if (existing) {
            const auto token = RememberGameFolder(destination);
            ExistingCopy recovery = ExistingCopy::Conflict;
            try {
                recovery = CheckPublished(
                    files,
                    [&](const File& file) {
                        return VerifyOutput(existing.as<StorageFolder>(), token, file,
                                            sources.at(file.relative)->digest, stop);
                    },
                    [&] { return stop.stop_requested(); });
            } catch (const winrt::hresult_error&) {
                return stop.stop_requested() ? "cancelled" : "conflict";
            }
            if (recovery == ExistingCopy::Cancelled)
                return "cancelled";
            return recovery == ExistingCopy::Verified ? "verify" : "conflict";
        }
        // Broker metadata, never a guessed drive letter. Unknown capacity fails closed.
        std::optional<std::uint64_t> available;
        const auto properties =
            destination.Properties().RetrievePropertiesAsync({L"System.FreeSpace"}).get();
        if (properties.HasKey(L"System.FreeSpace"))
            available = winrt::unbox_value<std::uint64_t>(properties.Lookup(L"System.FreeSpace"));
        if (!available || *available < total + Reserve)
            return "space";
        const auto token = RememberGameFolder(destination);
        staging =
            destination
                .CreateFolderAsync(L".jadyx-move", CreationCollisionOption::GenerateUniqueName)
                .get();
        std::uint64_t done{};
        if (progress)
            progress(done, total);
        IO io;
        io.cancelled = [&] { return stop.stop_requested(); };
        io.copy = [&](const File& file) {
            const auto parent = Parent(staging, file.relative, true);
            const auto output_file = parent
                                         .CreateFileAsync(winrt::to_hstring(Leaf(file.relative)),
                                                          CreationCollisionOption::FailIfExists)
                                         .get();
            auto output = output_file.OpenAsync(FileAccessMode::ReadWrite).get();
            DataWriter writer{output.GetOutputStreamAt(0)};
            std::vector<unsigned char> buffer(1024 * 1024);
            for (std::uint64_t offset = 0; offset < file.bytes;) {
                if (stop.stop_requested())
                    return false;
                const auto count = static_cast<std::size_t>(
                    std::min<std::uint64_t>(buffer.size(), file.bytes - offset));
                if (sources.at(file.relative)->Read(std::span{buffer}.first(count), offset) !=
                    count)
                    return false;
                writer.WriteBytes(
                    winrt::array_view<const std::uint8_t>{buffer.data(), buffer.data() + count});
                if (writer.StoreAsync().get() != count)
                    return false;
                offset += count;
                done += count;
                if (progress)
                    progress(done, total);
            }
            if (!writer.FlushAsync().get() || !output.FlushAsync().get() ||
                output.Size() != file.bytes)
                return false;
            writer.DetachStream();
            output.Close();
            return true;
        };
        io.verify = [&](const File& file) {
            return VerifyOutput(staging, token, file, sources.at(file.relative)->digest, stop);
        };
        io.publish = [&] {
            staging.RenameAsync(winrt::to_hstring(bundle), NameCollisionOption::FailIfExists).get();
            published = true;
            return true;
        };
        io.commit = [&] {
            std::map<std::string, std::string> relocated;
            for (const auto& [id, relative] : objects)
                relocated.emplace(id, MakeStoragePath(token, bundle + "/" + relative));
            const auto next_base = MakeStoragePath(token, bundle + "/" + base_relative);
            auto next = RelocateGameCatalog(catalog, application, game.launch_path, next_base,
                                            files.front().bytes, relocated);
            // Both current and fallback catalogs must reference USB before deleting any object.
            if (store.Save(next) != Content::Error::None ||
                store.Save(next) != Content::Error::None)
                return false;
            committed = true;
            LOG_INFO(Frontend,
                     "Game move: verified and registered application={:016X} files={} bytes={} "
                     "target={}",
                     application, files.size(), total, next_base);
            return true;
        };
        io.remove = [&](const File& file) {
            auto& source = *sources.at(file.relative);
            FILE_DISPOSITION_INFO disposition{};
            disposition.DeleteFile = TRUE;
            if (!SetFileInformationByHandle(source.handle, FileDispositionInfo, &disposition,
                                            sizeof(disposition)))
                return false;
            CloseHandle(source.handle);
            source.handle = INVALID_HANDLE_VALUE;
            LOG_INFO(Frontend, "Game move: removed verified source={}", Utf8(source.path));
            return true;
        };
        const auto result = Run(files, available, io);
        if (!published) {
            try {
                staging.DeleteAsync(StorageDeleteOption::PermanentDelete).get();
            } catch (...) {
            }
        }
        switch (result) {
        case Result::Success:
            return {};
        case Result::Cancelled:
            return "cancelled";
        case Result::Space:
            return "space";
        case Result::CleanupIncomplete:
            return "cleanup";
        default:
            return "failed";
        }
    } catch (const winrt::hresult_error& error) {
        LOG_WARNING(Frontend, "Game move: storage failed, published={} reason={}", published,
                    winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        LOG_WARNING(Frontend, "Game move: failed, published={} reason={}", published, error.what());
    } catch (...) {
        LOG_WARNING(Frontend, "Game move: storage unavailable, published={}", published);
    }
    if (staging && !published) {
        try {
            staging.DeleteAsync(StorageDeleteOption::PermanentDelete).get();
        } catch (...) {
        }
    }
    if (committed)
        return "cleanup";
    return stop.stop_requested() ? "cancelled" : "failed";
}
} // namespace Jadyx
