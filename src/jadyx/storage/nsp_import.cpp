// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/nsp_import.h"
#include "jadyx/storage/friendly_setup.h"
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include "common/logging.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/partition_filesystem.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/fssystem/fssystem_crypto_configuration.h"
#include "core/file_sys/fssystem/fssystem_compression_configuration.h"
#include "core/file_sys/fssystem/fssystem_nca_file_system_driver.h"
#include "core/loader/loader.h"

namespace Jadyx {
std::string ValidateUploadedNsp(const std::filesystem::path& path, std::stop_token stop) {
    using namespace FileSys;
    RealVfsFilesystem vfs;
    const auto encoded=path.u8string();
    const auto file=vfs.OpenFile(std::string{encoded.begin(), encoded.end()}, OpenMode::Read);
    if (!file) return "disk";
    // Reject HFS0 too: it is supported by the shared reader but is not a base NSP.
    const auto header=file->ReadBytes(16);
    if (header.size()!=16 || !std::equal(header.begin(),header.begin()+4,std::array<u8,4>{'P','F','S','0'}.begin())) return "nsp";
    const auto count=u32{header[4]} | (u32{header[5]} << 8) | (u32{header[6]} << 16) | (u32{header[7]} << 24);
    if (!count || count>4096) return "nsp"; // Bound shared-reader allocations before construction.
    PartitionFilesystem partition{file};
    if (partition.GetStatus() != Loader::ResultStatus::Success) return "nsp";
    const auto files=partition.GetFiles();
    if (files.empty() || files.size() > 4096) return "nsp";
    struct Entry { VirtualFile file; u64 title; NCAContentType type; };
    std::map<std::string, Entry> entries;
    std::set<std::string> metadata_xml;
    for (const auto& item : files) {
        if (stop.stop_requested()) return "interrupted";
        const auto name=item->GetName();
        if (name.size() == 41 && name.ends_with(".cnmt.xml")) {
            const auto id = name.substr(0, 32);
            if (id.find_first_not_of("0123456789abcdef") != std::string::npos ||
                item->GetSize() > 4 * 1024 * 1024 || !metadata_xml.insert(id).second) return "nsp";
            continue; // Optional exporter description; the encrypted CNMT remains authoritative.
        }
        if (name.ends_with(".tik") || name.ends_with(".cert")) {
            if (item->GetSize() > 16384) return "nsp";
            continue; // Loading the installed NSP handles its tickets; never mutate KeyManager here.
        }
        if (!name.ends_with(".nca") || (name.size() != 36 && !name.ends_with(".cnmt.nca")) || name.size() > 41)
            return "nsp";
        const auto id=name.substr(0,32);
        if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos) return "nsp";
        NcaReader reader;
        if (R_FAILED(reader.Initialize(item, GetCryptoConfiguration(), GetNcaCompressionConfiguration()))) return "keys";
        if (reader.GetContentSize() != item->GetSize()) {
            LOG_WARNING(Frontend, "Network import: NCA length mismatch, stored={} header={}",
                        item->GetSize(), reader.GetContentSize());
            return "incomplete";
        }
        const auto type=static_cast<NCAContentType>(reader.GetContentType());
        if (static_cast<unsigned>(type) > static_cast<unsigned>(NCAContentType::PublicData)) return "nsp";
        if (!entries.emplace(id, Entry{item, reader.GetProgramId(), type}).second) return "nsp";
    }
    for (const auto& id : metadata_xml) {
        const auto entry = entries.find(id);
        if (entry == entries.end() || entry->second.type != NCAContentType::Meta) return "nsp";
    }
    std::set<std::string> referenced;
    bool application=false, program=false;
    for (const auto& [id, entry] : entries) {
        if (entry.type != NCAContentType::Meta) continue;
        if (stop.stop_requested()) return "interrupted";
        NCA meta{entry.file};
        if (meta.GetStatus() != Loader::ResultStatus::Success) return "keys";
        referenced.insert(id);
        bool found=false;
        for (const auto& directory : meta.GetSubdirectories()) for (const auto& item : directory->GetFiles()) {
            if (item->GetExtension() != "cnmt") continue;
            if (item->GetSize() > 4 * 1024 * 1024) return "nsp";
            CNMT cnmt{item};
            if (!cnmt.IsValid()) return "nsp";
            if (cnmt.GetContentRecords().empty() || cnmt.GetContentRecords().size()>entries.size()) return "nsp";
            const auto route = GeneralUploadRoute(static_cast<unsigned>(cnmt.GetType()), cnmt.GetTitleID());
            if (route == UploadRoute::Update) return "update_route";
            if (route == UploadRoute::Dlc) return "dlc_route";
            if (route != UploadRoute::Base) return "base";
            if (cnmt.GetTitleID() != entry.title) return "nsp";
            found=application=true;
            bool app_program=false;
            std::set<std::string> records;
            for (const auto& record : cnmt.GetContentRecords()) {
                if (stop.stop_requested()) return "interrupted";
                std::string record_id;
                constexpr char hex[]="0123456789abcdef";
                for (const auto byte : record.nca_id) { record_id+=hex[byte >> 4]; record_id+=hex[byte & 15]; }
                if (!records.insert(record_id).second) return "nsp";
                const auto found_entry=entries.find(record_id);
                if (found_entry == entries.end() || found_entry->second.file->GetSize() != record.GetSize()) return "incomplete";
                const auto& content=found_entry->second;
                if (GetBaseTitleID(content.title) != cnmt.GetTitleID() ||
                    (GetCRTypeFromNCAType(content.type) != record.type &&
                     !(content.type == NCAContentType::Manual && record.type == ContentRecordType::LegalInformation))) return "base";
                referenced.insert(record_id);
                app_program |= record.type == ContentRecordType::Program;
            }
            if (!app_program) return "incomplete";
            program |= app_program;
        }
        if (!found) return "nsp";
    }
    // Unreferenced NCAs can hide update/DLC content in a mixed NSP. Reject rather than drop it.
    if (!application || !program || referenced.size() != entries.size()) return "base";
    return {};
}
} // namespace Jadyx
