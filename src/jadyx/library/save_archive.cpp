// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/save_archive.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>
#include <windows.h>
#include <fileapifromapp.h>

namespace Jadyx {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t ArchiveLimit = 4ULL << 30;
constexpr std::uint64_t Reserve = 64ULL << 20;
constexpr unsigned EntryLimit = 8192;

struct SaveArchiveError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct SavePathLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        const auto result = CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                                 right.data(), static_cast<int>(right.size()), TRUE);
        return result == CSTR_LESS_THAN || (result == 0 && left < right);
    }
};

bool IsReservedName(std::string_view name) {
    const auto dot = name.find('.');
    auto stem = std::string{name.substr(0, dot)};
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' ')) stem.pop_back();
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return true;
    return stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
           stem[3] >= '1' && stem[3] <= '9';
}

void CheckStop(std::stop_token stop) {
    if (stop.stop_requested()) throw SaveArchiveError{"interrupted"};
}

constexpr auto MakeCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        auto value = static_cast<std::uint32_t>(i);
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xEDB88320U : 0U);
        table[i] = value;
    }
    return table;
}
constexpr auto CrcTable = MakeCrcTable();

struct Entry {
    fs::path path;
    std::string name;
    std::uint32_t size{}, crc{}, offset{};
    bool directory{};
};

struct FileHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

void WriteBytes(std::ofstream& output, const void* bytes, std::size_t count, std::uint64_t& written) {
    if (count > ArchiveLimit - written) throw SaveArchiveError{"save_limit"};
    output.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(count));
    if (!output) throw SaveArchiveError{"disk"};
    written += count;
}

void Write16(std::ofstream& output, std::uint16_t value, std::uint64_t& written) {
    const std::array<char, 2> bytes{static_cast<char>(value), static_cast<char>(value >> 8)};
    WriteBytes(output, bytes.data(), bytes.size(), written);
}

void Write32(std::ofstream& output, std::uint32_t value, std::uint64_t& written) {
    const std::array<char, 4> bytes{static_cast<char>(value), static_cast<char>(value >> 8),
                                    static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
    WriteBytes(output, bytes.data(), bytes.size(), written);
}

std::string ZipPath(const fs::path& path) {
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::vector<Entry> Scan(const fs::path& source, std::uint64_t& total, std::stop_token stop) {
    std::vector<Entry> entries;
    std::set<std::wstring, SavePathLess> paths;
    std::uint64_t files{};
    for (fs::recursive_directory_iterator it{source}, end; it != end; ++it) {
        CheckStop(stop);
        const auto attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw SaveArchiveError{"save_path"};
        const bool directory = it->is_directory();
        if (!directory && !it->is_regular_file()) throw SaveArchiveError{"save_path"};
        if (entries.size() >= EntryLimit) throw SaveArchiveError{"save_limit"};
        auto name = ZipPath(it->path().lexically_relative(source));
        if (name.ends_with('/')) name.pop_back();
        auto safe = NormalizeSaveArchivePath(std::move(name));
        if (!safe) throw SaveArchiveError{"save_path"};
        const auto canonical = fs::u8path(*safe).wstring();
        if (!paths.insert(canonical).second) throw SaveArchiveError{"save_path"};
        if (directory) safe->push_back('/');
        std::uint32_t size{};
        if (!directory) {
            const auto length = it->file_size();
            if (length > std::numeric_limits<std::uint32_t>::max() || length > ArchiveLimit - total)
                throw SaveArchiveError{"save_limit"};
            size = static_cast<std::uint32_t>(length);
            total += length;
            ++files;
        }
        entries.push_back({it->path(), std::move(*safe), size, 0, 0, directory});
    }
    if (!files) throw SaveArchiveError{"empty"};
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    return entries;
}

void WriteZip(const fs::path& source, const fs::path& output_path, std::stop_token stop,
              const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    std::uint64_t total{};
    auto entries = Scan(source, total, stop);
    if (progress) progress(0, total);
    std::error_code ec;
    const auto available = fs::space(output_path.parent_path(), ec);
    if (ec || available.available < total + Reserve) throw SaveArchiveError{"space"};
    auto partial = output_path;
    partial += L".partial";
    fs::remove(partial, ec);
    if (ec) throw SaveArchiveError{"busy"};
    fs::remove(output_path, ec);
    if (ec) throw SaveArchiveError{"busy"};

    try {
        std::ofstream output{partial, std::ios::binary | std::ios::trunc};
        if (!output) throw SaveArchiveError{"disk"};
        std::uint64_t archive_size{}, processed{};
        std::vector<char> buffer(1U << 20);
        for (auto& entry : entries) {
            CheckStop(stop);
            if (archive_size > std::numeric_limits<std::uint32_t>::max() ||
                entry.name.size() > std::numeric_limits<std::uint16_t>::max())
                throw SaveArchiveError{"save_limit"};
            entry.offset = static_cast<std::uint32_t>(archive_size);
            const auto flags = static_cast<std::uint16_t>(entry.directory ? 0x0800 : 0x0808);
            Write32(output, 0x04034B50, archive_size);
            Write16(output, 20, archive_size);
            Write16(output, flags, archive_size);
            Write16(output, 0, archive_size);
            Write16(output, 0, archive_size);
            Write16(output, 0x0021, archive_size);
            Write32(output, 0, archive_size);
            Write32(output, 0, archive_size);
            Write32(output, 0, archive_size);
            Write16(output, static_cast<std::uint16_t>(entry.name.size()), archive_size);
            Write16(output, 0, archive_size);
            WriteBytes(output, entry.name.data(), entry.name.size(), archive_size);
            if (entry.directory) continue;

            FileHandle input;
            CREATEFILE2_EXTENDED_PARAMETERS options{sizeof(options)};
            options.dwFileFlags = FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT;
            input.value = CreateFile2FromAppW(entry.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                               OPEN_EXISTING, &options);
            if (input.value == INVALID_HANDLE_VALUE) throw SaveArchiveError{"disk"};
            BY_HANDLE_FILE_INFORMATION info{};
            LARGE_INTEGER length{};
            if (!GetFileInformationByHandle(input.value, &info) ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
                !GetFileSizeEx(input.value, &length) || length.QuadPart != entry.size)
                throw SaveArchiveError{"save_path"};

            std::uint32_t crc = 0xFFFFFFFFU;
            std::uint64_t remaining = entry.size;
            while (remaining) {
                CheckStop(stop);
                const auto request = static_cast<DWORD>(std::min<std::uint64_t>(remaining, buffer.size()));
                DWORD received{};
                if (!ReadFile(input.value, buffer.data(), request, &received, nullptr) || received != request)
                    throw SaveArchiveError{"disk"};
                for (DWORD i = 0; i < received; ++i)
                    crc = CrcTable[(crc ^ static_cast<unsigned char>(buffer[i])) & 0xFF] ^ (crc >> 8);
                WriteBytes(output, buffer.data(), received, archive_size);
                remaining -= received;
                processed += received;
                if (progress) progress(processed, total);
            }
            entry.crc = crc ^ 0xFFFFFFFFU;
            Write32(output, 0x08074B50, archive_size);
            Write32(output, entry.crc, archive_size);
            Write32(output, entry.size, archive_size);
            Write32(output, entry.size, archive_size);
        }

        const auto central_offset = archive_size;
        for (const auto& entry : entries) {
            const auto flags = static_cast<std::uint16_t>(entry.directory ? 0x0800 : 0x0808);
            Write32(output, 0x02014B50, archive_size);
            Write16(output, 20, archive_size);
            Write16(output, 20, archive_size);
            Write16(output, flags, archive_size);
            Write16(output, 0, archive_size);
            Write16(output, 0, archive_size);
            Write16(output, 0x0021, archive_size);
            Write32(output, entry.crc, archive_size);
            Write32(output, entry.size, archive_size);
            Write32(output, entry.size, archive_size);
            Write16(output, static_cast<std::uint16_t>(entry.name.size()), archive_size);
            Write16(output, 0, archive_size);
            Write16(output, 0, archive_size);
            Write16(output, 0, archive_size);
            Write16(output, 0, archive_size);
            Write32(output, entry.directory ? 0x10 : 0, archive_size);
            Write32(output, entry.offset, archive_size);
            WriteBytes(output, entry.name.data(), entry.name.size(), archive_size);
        }
        const auto central_size = archive_size - central_offset;
        if (entries.size() > std::numeric_limits<std::uint16_t>::max() ||
            central_offset > std::numeric_limits<std::uint32_t>::max() ||
            central_size > std::numeric_limits<std::uint32_t>::max())
            throw SaveArchiveError{"save_limit"};
        Write32(output, 0x06054B50, archive_size);
        Write16(output, 0, archive_size);
        Write16(output, 0, archive_size);
        Write16(output, static_cast<std::uint16_t>(entries.size()), archive_size);
        Write16(output, static_cast<std::uint16_t>(entries.size()), archive_size);
        Write32(output, static_cast<std::uint32_t>(central_size), archive_size);
        Write32(output, static_cast<std::uint32_t>(central_offset), archive_size);
        Write16(output, 0, archive_size);
        output.flush();
        if (!output) throw SaveArchiveError{"disk"};
        output.close();
        if (!output) throw SaveArchiveError{"disk"};
        CheckStop(stop);
        fs::rename(partial, output_path, ec);
        if (ec) throw SaveArchiveError{"disk"};
        if (progress) progress(total, total);
    } catch (...) {
        std::error_code ignored;
        fs::remove(partial, ignored);
        throw;
    }
}
} // namespace

std::optional<std::string> NormalizeSaveArchivePath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty() || path.front() == '/' || path.size() > 240 ||
        path.find(':') != std::string::npos || path.find('\0') != std::string::npos)
        return std::nullopt;
    std::size_t start{};
    for (;;) {
        const auto end = path.find('/', start);
        const auto part = std::string_view{path}.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ' ||
            IsReservedName(part) || part.find_first_of("<>\"|?*") != std::string_view::npos ||
            std::any_of(part.begin(), part.end(), [](unsigned char c) { return c < 32; }))
            return std::nullopt;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return path;
}

std::string ExportSaveArchive(
    const std::filesystem::path& source, const std::filesystem::path& output, std::stop_token stop,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    try {
        fs::create_directories(output.parent_path());
        WriteZip(source, output, stop, progress);
        return {};
    } catch (const SaveArchiveError& error) {
        return error.what();
    } catch (const std::filesystem::filesystem_error&) {
        return "disk";
    } catch (const std::exception&) {
        return "disk";
    } catch (...) {
        return "disk";
    }
}

} // namespace Jadyx
