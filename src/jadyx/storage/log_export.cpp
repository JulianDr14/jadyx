// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/log_export.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <windows.h>
#include <fileapifromapp.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

namespace Jadyx {
namespace {
struct Failure : std::runtime_error { using runtime_error::runtime_error; };
void CheckZstd(std::size_t result) { if (ZSTD_isError(result)) throw Failure{"compression"}; }
std::string Utf8(const std::filesystem::path& path) {
    const auto name = path.generic_u8string();
    return {reinterpret_cast<const char*>(name.data()), name.size()};
}
void CheckPath(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &attributes))
        throw Failure{"read"};
    if (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) throw Failure{"unsafe"};
}
struct Entry { std::filesystem::path path; std::string name; std::uint64_t size; bool directory; };
std::array<char, 512> Header(std::string_view name, std::uint64_t size, char type) {
    std::array<char, 512> header{};
    std::memcpy(header.data(), name.data(), std::min(name.size(), std::size_t{100}));
    std::snprintf(header.data() + 100, 8, "%07o", type == '5' ? 0755 : 0644);
    std::snprintf(header.data() + 108, 8, "%07o", 0);
    std::snprintf(header.data() + 116, 8, "%07o", 0);
    std::snprintf(header.data() + 124, 12, "%011llo", size);
    std::snprintf(header.data() + 136, 12, "%011o", 0);
    std::memset(header.data() + 148, ' ', 8);
    header[156] = type;
    std::memcpy(header.data() + 257, "ustar\0" "00", 8);
    unsigned checksum{};
    for (const unsigned char byte : header) checksum += byte;
    std::snprintf(header.data() + 148, 7, "%06o", checksum);
    header[155] = ' ';
    return header;
}
std::string Pax(std::string_view key, std::string_view value) {
    const auto body = " " + std::string(key) + "=" + std::string(value) + "\n";
    auto length = body.size() + 1;
    while (std::to_string(length).size() + body.size() != length)
        length = std::to_string(length).size() + body.size();
    return std::to_string(length) + body;
}
struct Reader {
    HANDLE handle{INVALID_HANDLE_VALUE};
    ~Reader() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
} // namespace

LogExport::LogExport(std::filesystem::path path)
    : root{std::filesystem::absolute(std::move(path)).lexically_normal()},
      directory{root.parent_path() / "transfers" / "log-export"} {}
LogExport::~LogExport() { Stop(); }
LogExportStatus LogExport::Status() const { std::lock_guard lock{mutex}; return status; }
std::filesystem::path LogExport::ArchivePath() const { return directory / "logs.tar.zst"; }
std::filesystem::path LogExport::PartialPath() const { return directory / "logs.partial"; }

void* LogExport::OpenArchive(std::uint64_t run, const std::function<bool()>& alive) const {
    std::unique_lock lock{mutex};
    while (status.run == run && status.phase == "scanning" && alive())
        progress.wait_for(lock, std::chrono::milliseconds{100});
    if (status.run != run) return INVALID_HANDLE_VALUE;
    // The writer renames the partial under this same lock, so the phase names the file. A
    // streaming reader shares delete so that rename can publish the archive under it.
    const bool partial = status.phase == "compressing";
    if (!partial && status.phase != "ready") return INVALID_HANDLE_VALUE;
    CREATEFILE2_EXTENDED_PARAMETERS options{sizeof(options)};
    options.dwFileFlags = FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT;
    return CreateFile2FromAppW((partial ? PartialPath() : ArchivePath()).c_str(), GENERIC_READ,
        partial ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE : FILE_SHARE_READ,
        OPEN_EXISTING, &options);
}

LogExport::Follow LogExport::WaitFor(std::uint64_t run, std::uint64_t offset, std::uint64_t& available,
                                     const std::function<bool()>& alive) const {
    std::unique_lock lock{mutex};
    while (status.run == run && alive()) {
        const bool writing = status.phase == "scanning" || status.phase == "compressing";
        if (!writing && status.phase != "ready") return Follow::Gone;
        if (status.compressed > offset) { available = status.compressed; return Follow::Data; }
        if (!writing) return status.compressed == offset ? Follow::End : Follow::Gone;
        progress.wait_for(lock, std::chrono::milliseconds{100});
    }
    return Follow::Gone;
}
void LogExport::Resume() { std::lock_guard lock{lifecycle}; enabled = true; }
void LogExport::Cancel() { std::lock_guard lock{lifecycle}; worker.request_stop(); }
void LogExport::Stop() {
    std::lock_guard lock{lifecycle};
    enabled = false;
    worker.request_stop();
    if (worker.joinable()) worker.join();
}
std::string LogExport::Start() {
    std::lock_guard lock{lifecycle};
    if (!enabled) return "unavailable";
    if (Status().busy) return "busy";
    if (worker.joinable()) worker.join();
    try {
        std::filesystem::create_directories(directory);
        // An active HTTP reader denies deletion: preserve its immutable archive.
        std::error_code error;
        std::filesystem::remove(ArchivePath(), error);
        if (error) return "busy";
        // Named now, so a download that starts while compressing already has its file name.
        SYSTEMTIME time{}; GetSystemTime(&time);
        char filename[80]{};
        std::snprintf(filename, sizeof(filename), "jadyx-logs-%04u%02u%02u-%02u%02u%02u.tar.zst",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
        {
            std::lock_guard state_lock{mutex};
            status = {.phase = "scanning", .filename = filename, .run = status.run + 1, .busy = true};
        }
        progress.notify_all();
        worker = std::jthread([this](std::stop_token stop) { Run(stop); });
        return {};
    } catch (...) {
        {
            std::lock_guard state_lock{mutex};
            status.phase = "failed"; status.error = "disk"; status.busy = false;
        }
        progress.notify_all();
        return "disk";
    }
}

void LogExport::Run(std::stop_token stop) {
    const auto temporary = PartialPath();
    try {
        const auto cancelled = [&] { if (stop.stop_requested()) throw Failure{"cancelled"}; };
        CheckPath(root);
        std::vector<Entry> entries{{root, "log/", 0, true}};
        std::uint64_t total{}, files{};
        for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
            cancelled();
            CheckPath(entry.path());
            const bool folder = entry.is_directory();
            if (!folder && !entry.is_regular_file()) throw Failure{"unsafe"};
            const auto size = folder ? 0 : entry.file_size();
            if (entries.size() >= 100000 || size > (1ULL << 40) - total) throw Failure{"limit"};
            auto name = "log/" + Utf8(entry.path().lexically_relative(root)) + (folder ? "/" : "");
            entries.push_back({entry.path(), std::move(name), size, folder});
            total += size;
            files += !folder;
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
        std::unique_ptr<ZSTD_CCtx_params, decltype(&ZSTD_freeCCtxParams)>
            parameters{ZSTD_createCCtxParams(), ZSTD_freeCCtxParams};
        if (!parameters) throw Failure{"compression"};
        // The portal only exports while the console sits idle in network mode, so trade a few
        // percent of ratio for every core: a lazy level still packs repetitive text logs well.
        for (const auto [parameter, value] : {
                 std::pair{ZSTD_c_compressionLevel, 7}, std::pair{ZSTD_c_windowLog, 22},
                 std::pair{ZSTD_c_checksumFlag, 1}, std::pair{ZSTD_c_nbWorkers, 0}})
            CheckZstd(ZSTD_CCtxParams_setParameter(parameters.get(), parameter, value));
        const auto estimate = ZSTD_estimateCStreamSize_usingCCtxParams(parameters.get());
        CheckZstd(estimate);
        if (estimate > (64ULL << 20)) throw Failure{"compression_memory"};
        // Small jobs keep every worker busy even on a modest log folder; each worker also holds
        // its input job and output, so the worker count stays inside a fixed memory budget.
        const auto cores = std::clamp<unsigned>(std::thread::hardware_concurrency(), 1, 16);
        const auto job = std::clamp<std::uint64_t>(total / (cores * 4ULL), 1ULL << 20, 8ULL << 20);
        const auto per_worker = estimate + 3 * job;
        const auto workers = static_cast<int>(
            std::clamp<std::uint64_t>((256ULL << 20) / per_worker, 1, cores));
        if (total > job &&
            !ZSTD_isError(ZSTD_CCtxParams_setParameter(parameters.get(), ZSTD_c_nbWorkers, workers))) {
            CheckZstd(ZSTD_CCtxParams_setParameter(parameters.get(), ZSTD_c_jobSize, static_cast<int>(job)));
            CheckZstd(ZSTD_CCtxParams_setParameter(parameters.get(), ZSTD_c_overlapLog, 6));
        }
        std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)>
            compressor{ZSTD_createCCtx(), ZSTD_freeCCtx};
        if (!compressor) throw Failure{"compression"};
        CheckZstd(ZSTD_CCtx_setParametersUsingCCtxParams(compressor.get(), parameters.get()));
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output) throw Failure{"disk"};
        // Only now does the partial file exist: a streaming reader may open it from here on.
        {
            std::lock_guard lock{mutex};
            status.phase = "compressing"; status.total = total; status.files = files;
        }
        progress.notify_all();
        std::vector<char> input(4U << 20), packed(1U << 20);
        std::uint64_t processed{}, compressed{}, completed{}, unchecked{16ULL << 20};
        const auto emit = [&](const char* data, std::size_t size, bool end = false) {
            ZSTD_inBuffer source{data, size, 0};
            std::size_t remaining;
            do {
                cancelled();
                ZSTD_outBuffer destination{packed.data(), packed.size(), 0};
                remaining = ZSTD_compressStream2(compressor.get(), &destination, &source,
                                                end ? ZSTD_e_end : ZSTD_e_continue);
                CheckZstd(remaining);
                if (destination.pos) {
                    // One free-space query per 16 MiB written, not per flush; the margin adds those
                    // 16 MiB so the 32 MiB reserve still holds between two checks.
                    if (unchecked + destination.pos > (16ULL << 20)) {
                        if (std::filesystem::space(directory).available < destination.pos + (48ULL << 20))
                            throw Failure{"space"};
                        unchecked = 0;
                    }
                    unchecked += destination.pos;
                    // Flushed before it is published: a streaming reader reads up to `compressed`.
                    output.write(packed.data(), destination.pos);
                    output.flush();
                    if (!output) throw Failure{"disk"};
                    compressed += destination.pos;
                }
                {
                    std::lock_guard lock{mutex};
                    status.processed = processed; status.compressed = compressed;
                    status.completed_files = completed;
                }
                if (destination.pos) progress.notify_all();
            } while (source.pos < source.size || (end && remaining));
        };
        const std::array<char, 1024> zeros{};
        std::uint64_t ordinal{};
        for (const auto& entry : entries) {
            cancelled();
            // PAX carries exact UTF-8 paths and 64-bit sizes; readers need no custom extractor.
            const auto pax = Pax("path", entry.name) + Pax("size", std::to_string(entry.size));
            const auto extended = Header("PaxHeader/entry-" + std::to_string(ordinal), pax.size(), 'x');
            emit(extended.data(), extended.size()); emit(pax.data(), pax.size());
            if (pax.size() % 512) emit(zeros.data(), 512 - pax.size() % 512);
            const auto header = Header("log/entry-" + std::to_string(ordinal++),
                                       entry.size < (1ULL << 33) ? entry.size : 0,
                                       entry.directory ? '5' : '0');
            emit(header.data(), header.size());
            if (entry.directory) continue;
            CheckPath(entry.path);
            CREATEFILE2_EXTENDED_PARAMETERS options{sizeof(options)};
            options.dwFileFlags = FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT;
            Reader reader;
            reader.handle = CreateFile2FromAppW(entry.path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, &options);
            if (reader.handle == INVALID_HANDLE_VALUE) throw Failure{"read"};
            FILE_ATTRIBUTE_TAG_INFO tag{};
            if (!GetFileInformationByHandleEx(reader.handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
                throw Failure{"unsafe"};
            std::uint64_t remaining = entry.size;
            while (remaining) {
                cancelled();
                const auto take = static_cast<DWORD>(std::min<std::uint64_t>(remaining, input.size()));
                DWORD received{};
                if (!ReadFile(reader.handle, input.data(), take, &received, nullptr) || received != take)
                    throw Failure{"read"};
                emit(input.data(), received);
                processed += received; remaining -= received;
            }
            if (entry.size % 512) emit(zeros.data(), 512 - entry.size % 512);
            ++completed;
        }
        emit(zeros.data(), zeros.size()); emit(nullptr, 0, true);
        output.flush();
        if (!output) throw Failure{"disk"};
        output.close();
        cancelled();
        {
            // Under the state lock, so OpenArchive never looks for the file between the names.
            std::lock_guard lock{mutex};
            std::filesystem::rename(temporary, ArchivePath());
            status.phase = "ready"; status.busy = false;
            status.processed = processed; status.compressed = compressed; status.completed_files = completed;
        }
        progress.notify_all();
    } catch (...) {
        std::string reason{"disk"};
        try { throw; } catch (const Failure& error) { reason = error.what(); } catch (...) {}
        std::error_code ignored; std::filesystem::remove(temporary, ignored);
        {
            std::lock_guard lock{mutex};
            const bool cancelled = stop.stop_requested();
            status.phase = cancelled ? "cancelled" : "failed"; status.busy = false;
            status.error = cancelled ? "" : reason;
        }
        progress.notify_all();
    }
}
} // namespace Jadyx
