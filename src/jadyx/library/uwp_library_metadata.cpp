// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/uwp_library_metadata.h"
#include "jadyx/settings/nacp_language.h"
#include "jadyx/storage/storage_path.h"
#include "jadyx/storage/uwp_rom_storage.h"
#include "jadyx/content/content_service.h"
#include "jadyx/storage/uwp_file_manager.h"
#include <memory>
#include <winrt/base.h>
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "core/loader/nso.h"

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
}

std::uint64_t ContentStamp(const Content::Catalog& catalog, std::uint64_t program_id) {
    return program_id ? Content::ApplicationStamp(catalog, FileSys::GetBaseTitleID(program_id)) : 0;
}
} // namespace

void ReadLibraryMetadata(const std::filesystem::path& root, std::vector<LibraryEntry>& entries,
                         const LibraryCache& cache, MetadataOptions options, std::stop_token stop) {
    Content::Catalog catalog; Content::DefaultStore().Load(catalog);
    const auto language = static_cast<std::uint32_t>(options.language);
    // NACP::GetLanguageEntry reads the title in Eden's system language, so each title is read in
    // the language its game will run in (PickNacpLanguage, as the boot does). Only one metadata
    // worker runs at a time and the guest is not running, so setting it here races with nothing.
    Settings::values.language_index = options.language;
    // Core construction provides the loader's filesystem services. Initialize would reserve
    // guest DRAM and start timing; metadata does not need either.
    std::unique_ptr<Core::System> system;
    bool have_keys = false;
    for (auto& entry : entries) {
        if (stop.stop_requested()) break;
        const auto stamp = IsStoragePath(entry.launch_path) ? 0 : LibraryCache::StampFor(root / entry.relative_path);
        // A record stays valid until its own game's updates/DLC change, not anyone else's.
        if (auto record = cache.Load(entry.launch_path, stamp, language, options.icons);
            record && record->content == ContentStamp(catalog, record->program_id)) {
            Apply(entry, std::move(*record), options.icons);
            continue;
        }
        if (options.cache_only) continue;
        if (!system) {
            system = std::make_unique<Core::System>();
            system->SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
            system->SetFilesystem(MakeUwpFilesystem());
            have_keys = HasUsableHeaderKey();
            if (have_keys) system->GetFileSystemController().CreateFactories(*system->GetFilesystem());
        }
        // A clean install has no user keys yet. Keep placeholders until the file manager imports
        // them, rather than constructing NCA readers with a zero XTS key.
        if (!have_keys && entry.relative_path.extension() != L".nro") continue;
        // From here on a failure is final for this session: the background fill must move on.
        entry.title_loaded = true;
        try {
            const auto path = (root / entry.relative_path).u8string();
            const auto file = system->GetFilesystem()->OpenFile(entry.launch_path.empty() ?
                std::string{reinterpret_cast<const char*>(path.data()), path.size()} : entry.launch_path,
                FileSys::OpenMode::Read);
            auto loader = Loader::GetLoader(*system, file);
            if (!loader) continue;
            LibraryCacheRecord record;
            FileSys::NACP control;
            const bool have_control = loader->ReadControlData(control) == Loader::ResultStatus::Success;
            Settings::values.language_index =
                have_control ? PickNacpLanguage(options.language, control) : options.language;
            std::string title;
            if (loader->ReadTitle(title) == Loader::ResultStatus::Success && !title.empty())
                record.name = winrt::to_hstring(title).c_str();
            if (have_control) {
                record.developer = winrt::to_hstring(control.GetDeveloperName()).c_str();
                record.display_version = winrt::to_hstring(control.GetVersionString()).c_str();
            }
            if (loader->ReadIcon(record.icon) != Loader::ResultStatus::Success ||
                record.icon.size() > LibraryCache::MaxIcon)
                record.icon.clear();
            bool cacheable = true;
            if (loader->ReadProgramId(record.program_id) == Loader::ResultStatus::Success &&
                entry.relative_path.extension() != L".nro")
            {
                FileSys::ManualContentProvider selected;
                const auto resolved = Content::ResolveLaunchContent(*system->GetFilesystem(), entry.launch_path, selected, false, stop);
                // A refusal (a USB unplugged, a cancelled fill, requirements) can clear without the
                // game's content stamp changing, so it is shown but read again next time.
                cacheable = resolved.error == Content::Error::None;
                if (resolved.error == Content::Error::None) {
                    record.build_id = resolved.build_id;
                    if (resolved.application) record.display_version = winrt::to_hstring(resolved.display_version).c_str();
                } else record.display_version.clear();
            }
            else
                record.program_id = 0;
            record.content = ContentStamp(catalog, record.program_id);
            LOG_INFO(Frontend, "Library metadata: title '{}', icon {} bytes", title, record.icon.size());
            if (cacheable) cache.Store(entry.launch_path, stamp, language, record);
            Apply(entry, std::move(record), options.icons);
        } catch (const std::exception&) {
            // Missing keys/control data or an unreadable dump keeps filename
            // and placeholder; one entry must not take down the whole library.
        } catch (const winrt::hresult_error&) {}
    }
    Settings::values.language_index = options.language;
}
}
