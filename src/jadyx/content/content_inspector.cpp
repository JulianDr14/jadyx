// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/content/content_inspector.h"
#include <array>
#include <span>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <openssl/evp.h>
#include "common/logging.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/partition_filesystem.h"
#include "core/loader/loader.h"

namespace Jadyx::Content {
namespace {
using namespace FileSys;
/// Where the partition (PFS0 or HFS0) at `offset` says its last file ends, or nothing when
/// there is no partition header there.
std::optional<u64> PartitionEnd(const VirtualFile& file, u64 offset) {
    const auto header = file->ReadBytes(16, offset);
    if (header.size() != 16) return std::nullopt;
    const bool hfs = !std::memcmp(header.data(), "HFS0", 4);
    if (!hfs && std::memcmp(header.data(), "PFS0", 4)) return std::nullopt;
    u32 count{}, strtab{};
    std::memcpy(&count, header.data() + 4, 4);
    std::memcpy(&strtab, header.data() + 8, 4);
    if (count > 4096) return std::nullopt;
    const u64 entry_size = hfs ? 0x40 : 0x18;
    const auto entries = file->ReadBytes(count * entry_size, offset + 16);
    if (entries.size() != count * entry_size) return offset + 16 + count * entry_size;
    u64 data_end{};
    for (u32 i = 0; i < count; ++i) {
        u64 begin{}, size{};
        std::memcpy(&begin, entries.data() + i * entry_size, 8);
        std::memcpy(&size, entries.data() + i * entry_size + 8, 8);
        data_end = std::max(data_end, begin + size);
    }
    return offset + 16 + count * entry_size + strtab + data_end;
}

/// A package cut short: empty, shorter than its header, or shorter than the files its partition
/// table lists. Told apart from a damaged one so the message says to copy it again.
bool Truncated(const VirtualFile& file, const std::string& extension) {
    const u64 size = file->GetSize();
    if (size == 0) return true;
    if (extension == "nsp") {
        const auto end = PartitionEnd(file, 0);
        return size < 16 || (end && *end > size);
    }
    if (extension == "xci") {
        if (size < 0x200) return true;
        u64 hfs_offset{};
        if (file->ReadBytes(&hfs_offset, sizeof(hfs_offset), 0x130) != sizeof(hfs_offset)) return true;
        if (hfs_offset >= size) return true;
        // The secure partition is the root's last file, so the root's end is the image's end.
        const auto end = PartitionEnd(file, hfs_offset);
        return end && *end > size;
    }
    return extension == "nca" && size < 0xC00;
}

std::string Hex(std::span<const u8> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (auto b : bytes) { result += digits[b >> 4]; result += digits[b & 15]; }
    return result;
}
Error Hash(const VirtualFile& file, std::array<u8, 32>& digest, std::stop_token stop,
           const Progress& progress, u64& done, u64 total) {
    if (!file) return Error::Unavailable;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) return Error::Io;
    std::vector<u8> buffer(1024 * 1024);
    const auto size = file->GetSize();
    for (size_t offset = 0; offset < size;) {
        if (stop.stop_requested()) return Error::Cancelled;
        const auto bytes = std::min(buffer.size(), size - offset);
        if (file->Read(buffer.data(), bytes, offset) != bytes) return Error::Unavailable;
        if (EVP_DigestUpdate(ctx.get(), buffer.data(), bytes) != 1) return Error::Io;
        offset += bytes; done += bytes;
        if (progress) progress(done, total);
    }
    unsigned length{};
    return EVP_DigestFinal_ex(ctx.get(), digest.data(), &length) == 1 && length == digest.size() ?
        Error::None : Error::Io;
}
bool Gather(const VirtualDir& dir, std::vector<VirtualFile>& files, unsigned depth = 0) {
    if (!dir || depth > 3) return false;
    for (auto& file : dir->GetFiles()) {
        if (files.size() >= 4096) return false;
        files.push_back(std::move(file));
    }
    for (const auto& sub : dir->GetSubdirectories()) if (!Gather(sub, files, depth + 1)) return false;
    return true;
}
bool UsableStatus(Loader::ResultStatus status, bool update) {
    return status == Loader::ResultStatus::Success ||
        (update && status == Loader::ResultStatus::ErrorMissingBKTRBaseRomFS);
}
} // namespace

Error VerifyContent(InspectedContent& content, std::stop_token stop, Progress progress) {
    if (content.error != Error::None) return content.error;
    u64 done{}, total{};
    for (const auto& item : content.files) total += item.file->GetSize();
    for (const auto& item : content.files) {
        std::array<u8, 32> actual{};
        if (auto error = Hash(item.file, actual, stop, progress, done, total); error != Error::None)
            return content.error = error;
        if (actual != item.record.hash) return content.error = Error::HashMismatch;
    }
    content.revision.verified = true;
    return Error::None;
}

Inspection InspectPackage(FileSys::VfsFilesystem& vfs, const std::string& source,
                          bool verify, std::stop_token stop, Progress progress, std::string_view format) {
    using namespace FileSys;
    Inspection result;
    std::vector<VirtualFile> raw;
    if (vfs.GetEntryType(source) == VfsEntryType::Directory) {
        if (!Gather(vfs.OpenDirectory(source, OpenMode::Read), raw)) return {Error::Invalid, {}};
    } else {
        const auto file = vfs.OpenFile(source, OpenMode::Read);
        if (!file) return {Error::Unavailable, {}};
        auto extension = format.empty() ? file->GetExtension() : std::string{format};
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });
        if (Truncated(file, extension)) {
            LOG_WARNING(Frontend, "Content: {} is empty or cut short ({} bytes)", source, file->GetSize());
            return {Error::Truncated, {}};
        }
        if (extension == "nsp") {
            PartitionFilesystem nsp{file};
            if (nsp.GetStatus() != Loader::ResultStatus::Success) return {Error::Invalid, {}};
            raw = nsp.GetFiles();
        } else if (extension == "xci") {
            XCI xci{file};
            if (xci.GetStatus() != Loader::ResultStatus::Success || !xci.GetSecurePartitionNSP())
                return {Error::Invalid, {}};
            raw = xci.GetSecurePartitionNSP()->GetFiles();
        } else if (extension == "nca") raw.push_back(file);
        else return {Error::Unsupported, {}};
    }
    if (raw.size() > 4096) return {Error::Invalid, {}};
    std::map<std::string, VirtualFile> by_id;
    for (const auto& f : raw) {
        if (stop.stop_requested()) return {Error::Cancelled, {}};
        auto name = f->GetName();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        if (name.ends_with(".nca") && name.size() >= 36) {
            const auto id = name.substr(0, 32);
            if (!std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isxdigit(c); }) ||
                !by_id.emplace(id, f).second) return {Error::Invalid, {}};
        }
        if (f->GetExtension() == "tik") {
            if (f->GetSize() > 4096) return {Error::Invalid, {}};
            const auto ticket = Core::Crypto::Ticket::Read(f);
            if (ticket.IsValid()) Core::Crypto::KeyManager::Instance().AddTicket(ticket);
        }
    }
    bool missing_keys = false;
    for (const auto& [meta_id, file] : by_id) {
        if (stop.stop_requested()) return {Error::Cancelled, {}};
        NCA meta{file};
        if (meta.GetStatus() != Loader::ResultStatus::Success) { missing_keys = true; continue; }
        if (meta.GetType() != NCAContentType::Meta) continue;
        for (const auto& dir : meta.GetSubdirectories()) for (const auto& cnmt_file : dir->GetFiles()) {
            if (cnmt_file->GetExtension() != "cnmt") continue;
            auto cnmt = std::make_shared<CNMT>(cnmt_file);
            if (!cnmt->IsValid()) return {Error::Invalid, {}};
            if (cnmt->GetType() == TitleType::Application) continue;
            InspectedContent content;
            content.meta = file; content.cnmt = cnmt;
            auto& r = content.revision;
            r.source = source; r.title = cnmt->GetTitleID(); r.version = cnmt->GetTitleVersion();
            r.kind = cnmt->GetType() == TitleType::Update ? Kind::Update : Kind::Dlc;
            r.application = cnmt->GetApplicationId().value_or(0);
            r.required_application = cnmt->GetRequiredApplicationVersion();
            r.required_system = cnmt->GetRequiredSystemVersion();
            if ((cnmt->GetType() != TitleType::Update && cnmt->GetType() != TitleType::AOC) ||
                !r.application || cnmt->GetDataPatchId() != 0 || GetBaseTitleID(r.title) != r.application ||
                (r.kind == Kind::Update && r.title != GetUpdateTitleID(r.application)))
                content.error = Error::Unsupported;
            std::set<std::array<u8, 16>> seen;
            std::set<RightsId> rights;
            bool payload = false;
            r.bytes = file->GetSize();
            for (const auto& record : cnmt->GetContentRecords()) {
                if (record.type == ContentRecordType::DeltaFragment) continue;
                if (!seen.insert(record.nca_id).second) { content.error = Error::Invalid; break; }
                auto found = by_id.find(Hex(record.nca_id));
                if (found == by_id.end() || found->second->GetSize() != record.GetSize()) {
                    content.error = Error::Incomplete; break;
                }
                NCA nca{found->second};
                if (!UsableStatus(nca.GetStatus(), r.kind == Kind::Update)) {
                    content.error = Error::MissingKeys; break;
                }
                const auto program = nca.GetTitleId();
                if (GetBaseTitleID(program) != r.application || (r.kind == Kind::Dlc && program != r.title) ||
                    (GetCRTypeFromNCAType(nca.GetType()) != record.type && !(nca.GetType() == NCAContentType::Manual && record.type == ContentRecordType::LegalInformation))) {
                    content.error = Error::WrongGame; break;
                }
                payload |= record.type == (r.kind == Kind::Update ? ContentRecordType::Program : ContentRecordType::Data);
                rights.insert(nca.GetRightsId());
                content.files.push_back({record, found->second, program});
                r.bytes += record.GetSize();
                if (record.type == ContentRecordType::Control && nca.GetRomFS()) {
                    const auto control_dir = ExtractRomFS(nca.GetRomFS());
                    const auto control_file = control_dir ? control_dir->GetFile("control.nacp") : nullptr;
                    if (control_file && control_file->GetSize() >= sizeof(RawNACP)) {
                        NACP nacp{control_file};
                        r.name = nacp.GetApplicationName(); r.display_version = nacp.GetVersionString();
                    }
                }
            }
            if (!payload && content.error == Error::None) content.error = Error::Incomplete;
            for (const auto& f : raw) if (f->GetExtension() == "tik") {
                const auto ticket = Core::Crypto::Ticket::Read(f);
                if (ticket.IsValid() && rights.contains(ticket.GetData().rights_id)) {
                    content.tickets.push_back(f); r.bytes += f->GetSize();
                }
            }
            // Meta identity binds the payload hash list; this is integrity, not independent authenticity.
            std::array<u8, 32> digest{}; u64 done{};
            if (auto error = Hash(file, digest, stop, {}, done, file->GetSize()); error != Error::None)
                return {error, {}};
            std::array<u8, 44> identity{};
            std::copy(digest.begin(), digest.end(), identity.begin());
            for (size_t i=0; i<8; ++i) identity[32+i] = static_cast<u8>(r.title >> (8*i));
            for (size_t i=0; i<4; ++i) identity[40+i] = static_cast<u8>(r.version >> (8*i));
            unsigned length{};
            if (EVP_Digest(identity.data(), identity.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 || length != digest.size()) return {Error::Io, {}};
            r.id = Hex(digest);
            if (vfs.GetEntryType(source) == VfsEntryType::File)
                if (const auto f = vfs.OpenFile(source, OpenMode::Read)) r.source_size = f->GetSize();
            if (verify && content.error == Error::None) VerifyContent(content, stop, progress);
            result.contents.push_back(std::move(content));
        }
    }
    if (result.contents.empty()) result.error = missing_keys ? Error::MissingKeys : source.ends_with(".nca") ? Error::Incomplete : Error::Unsupported;
    return result;
}

void RegisterContent(FileSys::ManualContentProvider& provider, const InspectedContent& content) {
    using namespace FileSys;
    const auto& r = content.revision;
    const auto type = r.kind == Kind::Update ? TitleType::Update : TitleType::AOC;
    provider.AddEntryWithVersion(type, ContentRecordType::Meta, r.title, r.version, r.display_version, content.meta);
    for (const auto& file : content.files)
        provider.AddEntryWithVersion(type, file.record.type, r.kind == Kind::Update ? GetUpdateTitleID(file.program) : r.title, r.version, r.display_version, file.file);
}
} // namespace Jadyx::Content
