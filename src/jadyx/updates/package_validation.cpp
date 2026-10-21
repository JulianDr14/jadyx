// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <fstream>
#include <memory>
#include <string_view>
#include <archive.h>
#include <archive_entry.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include "jadyx/updates/package_validation.h"
namespace Jadyx::Updates {
bool ValidatePackage(const std::filesystem::path& path, const Release& release,
                     std::stop_token stop) {
    struct Input {
        std::ifstream file;
        std::array<char, 65536> buffer{};
        std::stop_token stop;
    } input{std::ifstream{path, std::ios::binary}, {}, stop};
    if (!input.file)
        return false;
    std::unique_ptr<archive, decltype(&archive_read_free)> zip{archive_read_new(),
                                                               archive_read_free};
    if (!zip || archive_read_support_format_zip(zip.get()) != ARCHIVE_OK)
        return false;
    const auto read = [](archive*, void* opaque, const void** bytes) -> la_ssize_t {
        auto& stream = *static_cast<Input*>(opaque);
        if (stream.stop.stop_requested())
            return -1;
        stream.file.read(stream.buffer.data(), stream.buffer.size());
        *bytes = stream.buffer.data();
        return stream.file.bad() ? -1 : stream.file.gcount();
    };
    // APPX uses ZIP64/data descriptors. Let libarchive consult the central directory
    // rather than guessing entry boundaries in its streaming ZIP reader.
    const auto seek = [](archive*, void* opaque, la_int64_t offset, int origin) -> la_int64_t {
        auto& stream = *static_cast<Input*>(opaque);
        if (stream.stop.stop_requested())
            return -1;
        stream.file.clear();
        const auto direction = origin == SEEK_SET ? std::ios::beg
                             : origin == SEEK_CUR ? std::ios::cur : std::ios::end;
        stream.file.seekg(offset, direction);
        return stream.file ? static_cast<la_int64_t>(stream.file.tellg()) : -1;
    };
    if (archive_read_set_seek_callback(zip.get(), seek) != ARCHIVE_OK ||
        archive_read_open(zip.get(), &input, nullptr, read, nullptr) != ARCHIVE_OK)
        return false;
    archive_entry* entry{};
    bool identity = false, signature = false;
    int status{};
    while ((status = archive_read_next_header(zip.get(), &entry)) == ARCHIVE_OK) {
        const auto raw_name = archive_entry_pathname(entry);
        if (!raw_name)
            return false;
        const std::string_view name{raw_name};
        if (name == "AppxSignature.p7x") {
            if (signature || archive_entry_size(entry) <= 4 ||
                archive_entry_size(entry) > (1 << 20))
                return false;
            signature = true;
        }
        if (name != "AppxManifest.xml") {
            if (archive_read_data_skip(zip.get()) != ARCHIVE_OK)
                return false;
            continue;
        }
        const auto size = archive_entry_size(entry);
        if (size <= 0 || size > 65536 || identity)
            return false;
        std::string xml(static_cast<std::size_t>(size), '\0');
        if (archive_read_data(zip.get(), xml.data(), xml.size()) != size)
            return false;
        try {
            using namespace winrt::Windows::Data::Xml::Dom;
            XmlLoadSettings settings;
            settings.ProhibitDtd(true);
            settings.ResolveExternals(false);
            settings.MaxElementDepth(32);
            XmlDocument document;
            // PowerShell's XmlDocument.Save writes a UTF-8 BOM, which LoadXml rejects as text.
            std::string_view text{xml};
            if (text.starts_with("\xEF\xBB\xBF"))
                text.remove_prefix(3);
            document.LoadXml(winrt::to_hstring(text), settings);
            // Parse the actual document: a comment containing a plausible Identity is not one.
            const auto nodes =
                document.SelectNodes(L"/*[local-name()='Package' and "
                                     L"namespace-uri()='http://schemas.microsoft.com/appx/manifest/"
                                     L"foundation/windows10']/*[local-name()='Identity' and "
                                     L"namespace-uri()='http://schemas.microsoft.com/appx/manifest/"
                                     L"foundation/windows10']");
            if (nodes.Size() != 1 ||
                document.SelectNodes(L"//*[local-name()='Identity']").Size() != 1)
                return false;
            const auto id = nodes.GetAt(0).as<XmlElement>();
            identity =
                id.GetAttribute(L"Name") == L"Jadyx" &&
                id.GetAttribute(L"Publisher") == L"CN=JadyxDev" &&
                id.GetAttribute(L"ProcessorArchitecture") == L"x64" &&
                id.GetAttribute(L"Version") == winrt::to_hstring(FormatVersion(release.version));
            if (!identity)
                return false;
        } catch (...) {
            return false;
        }
    }
    return status == ARCHIVE_EOF && identity && signature;
}

} // namespace Jadyx::Updates
