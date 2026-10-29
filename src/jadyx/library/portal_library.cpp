// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/portal_library.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>
#include <archive.h>
#include <archive_entry.h>
#include <openssl/rand.h>
#include <windows.h>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/uuid.h"
#include "core/file_sys/common_funcs.h"
#include "core/hle/service/acc/profile_manager.h"
#include "jadyx/content/content_actions.h"
#include "jadyx/content/content_service.h"
#include "jadyx/library/save_archive.h"
#include "jadyx/storage/storage_path.h"
#include "jadyx/storage/utf8.h"
#include <fmt/format.h>

namespace Jadyx {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t SaveArchiveLimit = 4ULL << 30;
constexpr std::uint64_t SaveExpandedLimit = 8ULL << 30;
constexpr std::uint64_t SaveCopyLimit = 8ULL << 30;
constexpr std::uint64_t SaveReserve = 64ULL << 20;
constexpr std::uint64_t SaveSpaceCheckInterval = 16ULL << 20;

struct SavePathLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        const auto result = CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                                 right.data(), static_cast<int>(right.size()), TRUE);
        return result == CSTR_LESS_THAN || (result == 0 && left < right);
    }
};

struct SaveImportError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void CheckSaveSpace(const fs::path& path) {
    std::error_code ec;
    const auto space = fs::space(path, ec);
    if (ec || space.available < SaveReserve + SaveSpaceCheckInterval)
        throw SaveImportError{"space"};
}

void AccountSaveWrite(const fs::path& stage, std::uint64_t bytes, std::uint64_t& since_check) {
    since_check += bytes;
    if (since_check < SaveSpaceCheckInterval) return;
    CheckSaveSpace(stage);
    since_check = 0;
}

struct SaveZipSource {
    std::ifstream file;
    std::array<char, 65536> buffer{};

    static la_ssize_t Read(archive*, void* data, const void** output) {
        auto& source = *static_cast<SaveZipSource*>(data);
        *output = source.buffer.data();
        source.file.read(source.buffer.data(), source.buffer.size());
        return source.file.bad() ? -1 : static_cast<la_ssize_t>(source.file.gcount());
    }

    static la_int64_t Seek(archive*, void* data, la_int64_t offset, int whence) {
        auto& source = *static_cast<SaveZipSource*>(data);
        source.file.clear();
        source.file.seekg(offset, whence == SEEK_SET ? std::ios::beg :
                                  whence == SEEK_CUR ? std::ios::cur : std::ios::end);
        return source.file ? static_cast<la_int64_t>(source.file.tellg()) : ARCHIVE_FATAL;
    }
};

void CheckSaveStop(std::stop_token stop) {
    if (stop.stop_requested()) throw SaveImportError{"interrupted"};
}

DWORD InspectSaveDirectory(const fs::path& root, const fs::path& relative) {
    auto attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return INVALID_FILE_ATTRIBUTES;
        throw SaveImportError{"save_path"};
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        throw SaveImportError{"save_path"};
    auto current = root;
    for (const auto& component : relative) {
        current /= component;
        attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                return INVALID_FILE_ATTRIBUTES;
            throw SaveImportError{"save_path"};
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            throw SaveImportError{"save_path"};
    }
    return attributes;
}

std::string SafeSavePath(std::string path) {
    auto safe = NormalizeSaveArchivePath(std::move(path));
    if (!safe) throw SaveImportError{"archive_unsafe"};
    return std::move(*safe);
}

void CopySaveTree(const fs::path& source, const fs::path& destination, std::uint64_t& copied,
                  std::uint64_t& since_space_check, std::stop_token stop) {
    std::vector<char> buffer(1U << 20);
    for (fs::recursive_directory_iterator it{source}, end; it != end; ++it) {
        CheckSaveStop(stop);
        const auto attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw SaveImportError{"save_path"};
        const auto relative = it->path().lexically_relative(source);
        const auto target = destination / relative;
        if (it->is_directory()) {
            fs::create_directories(target);
            continue;
        }
        if (!it->is_regular_file()) throw SaveImportError{"save_path"};
        const auto size = it->file_size();
        if (size > SaveCopyLimit - copied) throw SaveImportError{"save_limit"};
        copied += size;
        std::ifstream input{it->path(), std::ios::binary};
        std::ofstream output{target, std::ios::binary | std::ios::trunc};
        if (!input || !output) throw SaveImportError{"disk"};
        while (input) {
            CheckSaveStop(stop);
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) {
                output.write(buffer.data(), count);
                if (!output) throw SaveImportError{"disk"};
                AccountSaveWrite(destination, static_cast<std::uint64_t>(count), since_space_check);
            }
        }
        if (input.bad()) throw SaveImportError{"disk"};
        output.close();
        if (!output) throw SaveImportError{"disk"};
    }
}

void ExtractSaveZip(const fs::path& zip, const fs::path& stage, std::uint64_t& extracted,
                    unsigned& file_count,
                    std::uint64_t& since_space_check, std::stop_token stop) {
    if (fs::file_size(zip) > SaveArchiveLimit) throw SaveImportError{"save_limit"};
    auto* reader = archive_read_new();
    if (!reader) throw SaveImportError{"archive"};
    const auto free_reader = [&] { archive_read_free(reader); };
    archive_read_support_filter_none(reader);
    archive_read_support_format_zip_seekable(reader);
    SaveZipSource source{std::ifstream{zip, std::ios::binary}, {}};
    if (!source.file) {
        free_reader();
        throw SaveImportError{"disk"};
    }
    archive_read_set_callback_data(reader, &source);
    archive_read_set_read_callback(reader, SaveZipSource::Read);
    archive_read_set_seek_callback(reader, SaveZipSource::Seek);
    if (archive_read_open1(reader) != ARCHIVE_OK) {
        free_reader();
        throw SaveImportError{"archive"};
    }

    try {
        std::set<std::wstring, SavePathLess> paths;
        std::vector<char> buffer(1U << 20);
        unsigned entries{};
        archive_entry* entry{};
        for (;;) {
            CheckSaveStop(stop);
            const auto status = archive_read_next_header(reader, &entry);
            if (status == ARCHIVE_EOF) break;
            if (status != ARCHIVE_OK ||
                (archive_format(reader) & ARCHIVE_FORMAT_BASE_MASK) != ARCHIVE_FORMAT_ZIP)
                throw SaveImportError{"archive"};
            if (++entries > 8192 || archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
                archive_entry_is_encrypted(entry) != 0)
                throw SaveImportError{"archive_unsafe"};
            const auto* encoded = archive_entry_pathname_utf8(entry);
            if (!encoded) throw SaveImportError{"archive_unsafe"};
            auto path = std::string{encoded};
            const bool directory = archive_entry_filetype(entry) == AE_IFDIR;
            if (directory && !path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
            path = SafeSavePath(std::move(path));
            auto canonical = fs::u8path(path).wstring();
            if (!paths.insert(std::move(canonical)).second) throw SaveImportError{"archive_unsafe"};
            const auto target = stage / fs::u8path(path);
            if (directory) {
                fs::create_directories(target);
                continue;
            }
            if (archive_entry_filetype(entry) != AE_IFREG) throw SaveImportError{"archive_unsafe"};
            const auto declared = archive_entry_size(entry);
            if (declared < 0 || static_cast<std::uint64_t>(declared) > SaveExpandedLimit - extracted)
                throw SaveImportError{"save_limit"};
            ++file_count;
            fs::create_directories(target.parent_path());
            std::ofstream output{target, std::ios::binary | std::ios::trunc};
            if (!output) throw SaveImportError{"disk"};
            std::uint64_t written{};
            for (;;) {
                CheckSaveStop(stop);
                const auto count = archive_read_data(reader, buffer.data(), buffer.size());
                if (count < 0) throw SaveImportError{"archive"};
                if (count == 0) break;
                const auto bytes = static_cast<std::uint64_t>(count);
                if (bytes > static_cast<std::uint64_t>(declared) - written ||
                    bytes > SaveExpandedLimit - extracted)
                    throw SaveImportError{"save_limit"};
                output.write(buffer.data(), static_cast<std::streamsize>(count));
                if (!output) throw SaveImportError{"disk"};
                AccountSaveWrite(stage, bytes, since_space_check);
                written += bytes;
                extracted += bytes;
            }
            if (written != static_cast<std::uint64_t>(declared)) throw SaveImportError{"archive"};
            output.close();
            if (!output) throw SaveImportError{"disk"};
        }
        if (paths.empty() || !file_count || archive_read_has_encrypted_entries(reader) > 0)
            throw SaveImportError{"archive"};
    } catch (...) {
        free_reader();
        throw;
    }
    free_reader();
}

fs::path GameSavePath(std::uint64_t application) {
    Service::Account::ProfileManager profiles;
    const auto index = static_cast<std::size_t>((std::max)(0, Settings::values.current_user.GetValue()));
    const auto user = profiles.GetUser(index);
    if (!user || user->IsInvalid()) throw SaveImportError{"profile"};

    const auto save_root = Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir);
    const auto future = save_root / "user/save/account" / fs::u8path(user->RawString()) /
                        fmt::format("{:016X}", application & ~0xFFULL) / "0";
    std::error_code ec;
    if (fs::is_directory(future, ec) && !ec) return future;
    const auto uid = user->AsU128();
    return save_root / "user/save/0000000000000000" /
           fmt::format("{:016X}{:016X}", uid[1], uid[0]) / fmt::format("{:016X}", application);
}

std::string ImportSaveArchive(const fs::path& zip, std::uint64_t application, std::stop_token stop) {
    fs::path target, stage, backup;
    bool target_moved{};
    const auto rollback = [&] {
        std::error_code cleanup;
        if (!stage.empty()) {
            fs::remove_all(stage, cleanup);
            if (cleanup) {
                LOG_WARNING(Frontend, "Save import staging cleanup failed: {}", cleanup.message());
                return false;
            }
        }
        if (target_moved && !backup.empty()) {
            const bool target_exists = fs::exists(target, cleanup);
            if (cleanup) return false;
            if (!target_exists) {
                fs::rename(backup, target, cleanup);
                if (cleanup) {
                    LOG_ERROR(Frontend, "Save import rollback failed: {}", cleanup.message());
                    return false;
                }
            }
        }
        return true;
    };
    try {
        CheckSaveStop(stop);
        target = GameSavePath(application);
        const auto root = Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir).lexically_normal();
        const auto relative = target.lexically_normal().lexically_relative(root);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            throw SaveImportError{"save_path"};
        const auto target_attributes = InspectSaveDirectory(root, relative);

        const auto zip_size = fs::file_size(zip);
        std::uint64_t old_bytes{};
        const bool target_exists = target_attributes != INVALID_FILE_ATTRIBUTES;
        if (target_exists) {
            for (fs::recursive_directory_iterator it{target}, end; it != end; ++it) {
                CheckSaveStop(stop);
                const auto attributes = GetFileAttributesW(it->path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    throw SaveImportError{"save_path"};
                if (it->is_regular_file()) {
                    const auto size = it->file_size();
                    if (size > SaveCopyLimit - old_bytes) throw SaveImportError{"save_limit"};
                    old_bytes += size;
                } else if (!it->is_directory()) {
                    throw SaveImportError{"save_path"};
                }
            }
        }
        fs::create_directories(target.parent_path());
        std::error_code ec;
        const auto space = fs::space(target.parent_path(), ec);
        if (ec || old_bytes > SaveCopyLimit || space.available < old_bytes + zip_size + SaveReserve)
            throw SaveImportError{"space"};

        std::array<unsigned char, 16> random{};
        if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
            throw SaveImportError{"random"};
        constexpr char hex[] = "0123456789abcdef";
        std::string suffix;
        for (const auto byte : random) { suffix += hex[byte >> 4]; suffix += hex[byte & 15]; }
        stage = target.parent_path() / fs::u8path(".jadyx-save-import-" + suffix);
        backup = target.parent_path() / fs::u8path(".jadyx-save-backup-" + suffix);
        if (!fs::create_directory(stage, ec) || ec) throw SaveImportError{"disk"};

        std::uint64_t copied{}, since_space_check{};
        if (target_exists) CopySaveTree(target, stage, copied, since_space_check, stop);
        std::uint64_t extracted{};
        unsigned files{};
        ExtractSaveZip(zip, stage, extracted, files, since_space_check, stop);
        if (!files) throw SaveImportError{"archive"};
        CheckSaveStop(stop);

        if (target_exists) {
            fs::rename(target, backup);
            target_moved = true;
        }
        try {
            fs::rename(stage, target);
        } catch (...) {
            fs::rename(backup, target);
            target_moved = false;
            throw;
        }
        std::error_code cleanup;
        fs::remove_all(backup, cleanup);
        if (cleanup) LOG_WARNING(Frontend, "Save import completed but backup cleanup failed: {}", cleanup.message());
        return {};
    } catch (const SaveImportError& error) {
        return rollback() ? error.what() : "disk";
    } catch (const std::exception& error) {
        LOG_ERROR(Frontend, "Save import failed: {}", error.what());
        rollback();
        return "disk";
    } catch (...) {
        LOG_ERROR(Frontend, "Save import failed with an unknown error");
        rollback();
        return "disk";
    }
}
} // namespace

std::uint64_t ContentTagApplication(const std::string& tag) {
    const auto colon = tag.find(':');
    const auto kind = tag.substr(0, colon);
    std::uint64_t application{};
    if (colon == std::string::npos || tag.size() - colon - 1 != 16 ||
        (kind != "update" && kind != "dlc" && kind != "save"))
        return 0;
    const auto parsed = std::from_chars(tag.data() + colon + 1, tag.data() + tag.size(), application, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != tag.data() + tag.size()) return 0;
    return application;
}

PortalLibrarySource::PortalLibrarySource(std::filesystem::path root_, std::filesystem::path cache_directory)
    : root{std::move(root_)}, cache{std::move(cache_directory)} {}

void PortalLibrarySource::Publish(const std::vector<LibraryEntry>& entries, std::uint32_t language_) {
    std::vector<Game> previous;
    {
        std::lock_guard lock{mutex};
        previous = games;
    }
    std::vector<Game> next;
    const auto listed = [&](std::uint64_t application) {
        return std::any_of(next.begin(), next.end(), [&](const auto& g) { return g.info.program_id == application; });
    };
    for (const auto& entry : entries) {
        if (entry.launch_path.empty()) continue;
        if (!entry.title_loaded) {
            // A rescan reads metadata again; keep showing what was known of a dump still present
            // rather than emptying the page until the fill reaches it.
            const auto known = std::find_if(previous.begin(), previous.end(),
                [&](const auto& g) { return g.launch_path == entry.launch_path; });
            if (known != previous.end() && !listed(known->info.program_id)) next.push_back(*known);
            continue;
        }
        if (!entry.program_id) continue;
        const auto application = FileSys::GetBaseTitleID(entry.program_id);
        // One card per game: a second dump of the same title adds nothing to manage.
        if (listed(application)) continue;
        auto files = FindGameFiles(entries, application);
        const bool removable = !files.external;
        next.push_back({.info = {.program_id = application, .name = Utf16ToUtf8(entry.name),
                                 .developer = Utf16ToUtf8(entry.developer),
                                 .version = Utf16ToUtf8(entry.display_version), .removable = removable},
                        .launch_path = entry.launch_path, .relative_path = entry.relative_path,
                        .files = std::move(files)});
    }
    std::lock_guard lock{mutex};
    if (next == games && language_ == language) return;
    games = std::move(next);
    language = language_;
    ++revision;
}

PortalLibrary PortalLibrarySource::View() {
    return {
        .games = [this] {
            std::lock_guard lock{mutex};
            std::vector<PortalGame> list;
            list.reserve(games.size());
            for (const auto& game : games) list.push_back(game.info);
            return std::pair{revision, std::move(list)};
        },
        .cover = [this](std::uint64_t application) { return Cover(application); },
        .contents = [](std::uint64_t application) {
            Content::Catalog catalog;
            if (Content::DefaultStore().Load(catalog) != Content::Error::None) throw std::runtime_error{"catalog"};
            std::vector<PortalContent> list;
            for (const auto& r : catalog.revisions) {
                if (r.application != application) continue;
                list.push_back({.id = r.id, .kind = r.kind == Content::Kind::Update ? "update" : "dlc",
                                .name = r.name, .version = r.display_version, .title = r.title,
                                .bytes = r.bytes, .enabled = r.enabled, .internal = r.internal});
            }
            return list;
        },
        .progress = [this] { return std::pair{installed.load() >> 20, install_total.load() >> 20}; },
        .save_export = [this](std::uint64_t application, const fs::path& output, std::stop_token stop,
                              const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
            return ExportSave(application, output, stop, progress);
        },
    };
}

std::optional<PortalLibrarySource::Game> PortalLibrarySource::Find(std::uint64_t application) const {
    std::lock_guard lock{mutex};
    const auto found = std::find_if(games.begin(), games.end(),
        [&](const auto& g) { return g.info.program_id == application; });
    if (found == games.end()) return std::nullopt;
    return *found;
}

std::string PortalLibrarySource::Install(const std::filesystem::path& package, const std::string& tag,
                                         std::stop_token stop, const Content::Progress& progress) {
    // The portal validated the tag when the upload was created; parse it again, never trust it.
    const auto application = ContentTagApplication(tag);
    if (!application) return "file";
    const auto game = Find(application);
    if (!game) return "game";
    if (tag.starts_with("save:")) return ImportSaveArchive(package, application, stop);
    return Content::ImportUploadedContent(package, tag.starts_with("update:") ? Content::Kind::Update : Content::Kind::Dlc,
        application, game->launch_path, stop, [&](std::uint64_t done, std::uint64_t total) {
            Progress(done, total);
            if (progress) progress(done, total);
        });
}

std::string PortalLibrarySource::Remove(const PortalRemoval& removal) {
    const auto game = Find(removal.application);
    if (!game) return "game";
    return removal.content.empty() ? RemoveGame(root, game->files) :
                                     Content::RemoveContent(removal.application, removal.content);
}

std::string PortalLibrarySource::ExportSave(
    std::uint64_t application, const fs::path& output, std::stop_token stop,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    try {
        if (!Find(application)) return "game";
        CheckSaveStop(stop);
        const auto target = GameSavePath(application);
        const auto save_root = Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir).lexically_normal();
        const auto relative = target.lexically_normal().lexically_relative(save_root);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            throw SaveImportError{"save_path"};
        if (InspectSaveDirectory(save_root, relative) == INVALID_FILE_ATTRIBUTES)
            throw SaveImportError{"no_save"};
        return ExportSaveArchive(target, output, stop, progress);
    } catch (const SaveImportError& error) {
        return error.what();
    } catch (const std::filesystem::filesystem_error&) {
        return "disk";
    } catch (const std::exception&) {
        return "disk";
    }
}

std::optional<std::string> PortalLibrarySource::Cover(std::uint64_t application) {
    const auto found = Find(application);
    if (!found) return std::nullopt;
    const auto& game = *found;
    std::uint32_t game_language{};
    {
        std::lock_guard lock{mutex};
        game_language = language;
    }
    // The cover belongs to the dump: an update/DLC of the game does not invalidate it.
    const auto stamp = IsStoragePath(game.launch_path) ? 0 : LibraryCache::StampFor(root / game.relative_path);
    auto record = cache.Load(game.launch_path, stamp, game_language, true);
    if (!record || record->icon.empty()) return std::nullopt;
    return std::string{record->icon.begin(), record->icon.end()};
}

} // namespace Jadyx
