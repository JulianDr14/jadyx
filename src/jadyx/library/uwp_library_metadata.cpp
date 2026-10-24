// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cwctype>
#include <map>
#include <memory>
#include <winrt/base.h>
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "core/loader/nso.h"
#include "jadyx/content/content_service.h"
#include "jadyx/library/library_metadata_policy.h"
#include "jadyx/library/uwp_library_metadata.h"
#include "jadyx/settings/nacp_language.h"
#include "jadyx/storage/storage_path.h"
#include "jadyx/storage/uwp_file_manager.h"
#include "jadyx/storage/uwp_rom_storage.h"

namespace Jadyx {
namespace {
void Apply(LibraryEntry& entry, LibraryCacheRecord&& record, bool icons) {
    if (!record.name.empty()) entry.name = std::move(record.name);
    entry.developer = std::move(record.developer);
    entry.display_version = std::move(record.display_version);
    if (icons) entry.icon = std::move(record.icon);
    entry.program_id = record.program_id;
    entry.build_id = record.build_id;
    entry.title_loaded = true;
    entry.metadata_retry = false;
}

std::uint64_t ContentStamp(const Content::Catalog& catalog, std::uint64_t program_id) {
    return program_id ? Content::ApplicationStamp(catalog, FileSys::GetBaseTitleID(program_id)) : 0;
}

// The NSP app loader expects an Application/Control pair. Update-only and DLC-only
// containers need CNMT identities, and may have an update Control without a base Program.
void ReadPackagePresentation(Core::System& system, const FileSys::VirtualFile& file,
                             LibraryCacheRecord& record, Settings::Language language) {
    FileSys::ManualContentProvider package;
    if (!package.AddEntriesFromContainer(file))
        return;
    const auto contents = package.ListEntriesFilter({}, {}, {});
    if (!record.program_id && !contents.empty()) {
        const auto application = FileSys::GetBaseTitleID(contents.front().title_id);
        // Never borrow an arbitrary game's cover from a multi-application container.
        if (!application || std::any_of(contents.begin(), contents.end(), [&](const auto& item) {
                return FileSys::GetBaseTitleID(item.title_id) != application;
            }))
            return;
        record.program_id = contents.front().title_id;
    }
    if (!record.program_id)
        return;
    const auto application = FileSys::GetBaseTitleID(record.program_id);
    const FileSys::PatchManager patches{application, system.GetFileSystemController(), package};
    for (const auto& item : contents) {
        if (item.type != FileSys::ContentRecordType::Control ||
            FileSys::GetBaseTitleID(item.title_id) != application)
            continue;
        const auto nca = package.GetEntry(item.title_id, item.type);
        if (!nca || nca->GetStatus() != Loader::ResultStatus::Success)
            continue;
        auto [control, icon] = patches.ParseControlNCA(*nca);
        if (control) {
            Settings::values.language_index = PickNacpLanguage(language, *control);
            record.name = winrt::to_hstring(control->GetApplicationName()).c_str();
            record.developer = winrt::to_hstring(control->GetDeveloperName()).c_str();
            record.display_version = winrt::to_hstring(control->GetVersionString()).c_str();
        }
        if (icon && icon->GetSize() <= LibraryCache::MaxIcon) {
            record.icon = icon->ReadAllBytes();
            if (record.icon.size() != icon->GetSize())
                record.icon.clear();
        }
        if (!record.name.empty() && !record.icon.empty())
            break;
    }
}

} // namespace

void ReadLibraryMetadata(const std::filesystem::path& root, std::vector<LibraryEntry>& entries,
                         const LibraryCache& cache, MetadataOptions options, std::stop_token stop,
                         const std::vector<MetadataSource>& sources) {
    Content::Catalog catalog; Content::DefaultStore().Load(catalog);
    const auto language = static_cast<std::uint32_t>(options.language);
    Settings::values.language_index = options.language;
    struct RestoreLanguage {
        Settings::Language language;
        ~RestoreLanguage() {
            Settings::values.language_index = language;
        }
    } restore_language{.language = options.language};
    // No guest Initialize/Load/Run: only the filesystem services, on the existing worker.
    std::unique_ptr<Core::System> system;
    bool have_keys = false;
    struct CachedBase {
        const MetadataSource* source{};
        std::uint64_t stamp{};
    };
    std::map<std::uint64_t, CachedBase> bases;
    bool indexed_bases = false;
    const auto source_stamp = [&](const MetadataSource& source) {
        return IsStoragePath(source.launch_path)
                   ? 0
                   : LibraryCache::StampFor(root / source.relative_path);
    };
    const auto borrow_base = [&](LibraryCacheRecord& record) {
        if (!record.program_id || FileSys::GetBaseTitleID(record.program_id) == record.program_id)
            return;
        if (!indexed_bases) {
            indexed_bases = true;
            for (const auto& source : sources) {
                if (stop.stop_requested())
                    return;
                const auto stamp = source_stamp(source);
                const auto donor = cache.Load(source.launch_path, stamp, language, false);
                if (donor && donor->program_id &&
                    donor->program_id == FileSys::GetBaseTitleID(donor->program_id) &&
                    donor->content == ContentStamp(catalog, donor->program_id)) {
                    CachedBase candidate;
                    candidate.source = &source;
                    candidate.stamp = stamp;
                    bases.try_emplace(donor->program_id, candidate);
                }
            }
        }
        const auto found = bases.find(FileSys::GetBaseTitleID(record.program_id));
        if (found == bases.end())
            return;
        const auto donor =
            cache.Load(found->second.source->launch_path, found->second.stamp, language, true);
        if (donor)
            BorrowBaseMetadata(record, *donor);
    };
    for (auto& entry : entries) {
        if (stop.stop_requested()) break;
        const auto stamp = IsStoragePath(entry.launch_path) ? 0 : LibraryCache::StampFor(root / entry.relative_path);
        // Names-only cache probes stay cheap; actual fill results carry icons to visible tiles.
        const bool keep_icon = options.icons || !options.cache_only;
        if (auto record = cache.Load(entry.launch_path, stamp, language, keep_icon);
            record && record->content == ContentStamp(catalog, record->program_id)) {
            Apply(entry, std::move(*record), keep_icon);
            continue;
        }
        if (options.cache_only) continue;
        LibraryCacheRecord record;
        bool read_failed = false;
        // No guest or storage manager runs alongside this worker. Start a per-entry I/O probe.
        ConsumeStorageReadFailure();
        std::string reason = "control/icon unavailable (keys, missing base or unreadable content)";
        try {
            Settings::values.language_index = options.language;
            if (!system) {
                system = std::make_unique<Core::System>();
                system->SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
                system->SetFilesystem(MakeUwpFilesystem());
                have_keys = HasUsableHeaderKey();
                if (have_keys)
                    system->GetFileSystemController().CreateFactories(*system->GetFilesystem());
            }
            auto extension = entry.relative_path.extension().wstring();
            std::transform(
                extension.begin(), extension.end(), extension.begin(),
                [](wchar_t letter) { return static_cast<wchar_t>(std::towlower(letter)); });
            if (!have_keys && extension != L".nro") {
                reason = "missing usable header key";
            } else {
                const auto path = (root / entry.relative_path).u8string();
                const auto file = system->GetFilesystem()->OpenFile(
                    entry.launch_path.empty()
                        ? std::string{reinterpret_cast<const char*>(path.data()), path.size()}
                        : entry.launch_path,
                    FileSys::OpenMode::Read);
                auto loader = file ? Loader::GetLoader(*system, file) : nullptr;
                if (!loader)
                    reason = "file unavailable or loader not recognized";
                else {
                    FileSys::NACP control;
                    const bool have_control =
                        loader->ReadControlData(control) == Loader::ResultStatus::Success;
                    Settings::values.language_index =
                        have_control ? PickNacpLanguage(options.language, control)
                                     : options.language;
                    std::string title;
                    const auto title_status = loader->ReadTitle(title);
                    if (title_status == Loader::ResultStatus::Success && !title.empty())
                        record.name = winrt::to_hstring(title).c_str();
                    if (have_control) {
                        record.developer = winrt::to_hstring(control.GetDeveloperName()).c_str();
                        record.display_version =
                            winrt::to_hstring(control.GetVersionString()).c_str();
                    }
                    const auto icon_status = loader->ReadIcon(record.icon);
                    if (icon_status != Loader::ResultStatus::Success ||
                        record.icon.size() > LibraryCache::MaxIcon)
                        record.icon.clear();
                    reason = "title_status=" + std::to_string(static_cast<unsigned>(title_status)) +
                             " icon_status=" + std::to_string(static_cast<unsigned>(icon_status)) +
                             " (control unavailable, missing keys/base or empty/oversized icon)";
                    if (loader->ReadProgramId(record.program_id) != Loader::ResultStatus::Success)
                        record.program_id = 0;
                    if (extension != L".nro") {
                        if (record.name.empty() || record.icon.empty()) {
                            ReadPackagePresentation(*system, file, record, options.language);
                            borrow_base(record);
                        }
                        if (record.program_id &&
                            record.program_id == FileSys::GetBaseTitleID(record.program_id)) {
                            FileSys::ManualContentProvider selected;
                            const auto resolved = Content::ResolveLaunchContent(
                                *system->GetFilesystem(), entry.launch_path, selected, false, stop,
                                Content::ReportedSystemVersion(*system));
                            if (resolved.error == Content::Error::None) {
                                record.build_id = resolved.build_id;
                                if (resolved.application)
                                    record.display_version =
                                        winrt::to_hstring(resolved.display_version).c_str();
                            } else
                                record.display_version.clear();
                        }
                    }
                }
            }
        } catch (const std::exception& error) {
            read_failed = true;
            reason = error.what();
        } catch (const winrt::hresult_error& error) {
            read_failed = true;
            reason = "storage HRESULT=" + std::to_string(error.code().value);
        }
        if (ConsumeStorageReadFailure()) {
            read_failed = true;
            reason = "storage read failed (short read, USB removed or permission lost)";
        }
        if (read_failed)
            record.icon.clear(); // Never persist even a nonempty partial JPEG.
        if (stop.stop_requested())
            break; // Interrupted work is resumed, never cached.
        record.content = ContentStamp(catalog, record.program_id);
        const bool incomplete = record.name.empty() || record.icon.empty();
        if (incomplete) {
            LOG_WARNING(Frontend,
                        "Library metadata: no complete cover/title path={} program={:016X} "
                        "reason={}; retry next scan",
                        entry.launch_path, record.program_id, reason);
        } else {
            cache.Store(entry.launch_path, stamp, language, record);
        }
        Apply(entry, std::move(record), keep_icon);
        entry.metadata_retry = incomplete;
    }
}
} // namespace Jadyx
