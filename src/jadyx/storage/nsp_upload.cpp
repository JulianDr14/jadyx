// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/nsp_upload.h"
#include <windows.h>
#include <fileapifromapp.h>
#include <openssl/rand.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace Jadyx {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
std::uint64_t Elapsed(Clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count());
}
namespace fs = std::filesystem;
constexpr std::uint64_t Reserve = 64ULL << 20;

struct File {
    HANDLE handle{INVALID_HANDLE_VALUE};
    File(const fs::path& path, DWORD access, DWORD mode) {
        CREATEFILE2_EXTENDED_PARAMETERS parameters{};
        parameters.dwSize = sizeof(parameters);
        parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        handle = CreateFile2FromAppW(path.c_str(), access, FILE_SHARE_READ, mode, &parameters);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("disk");
    }
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    void Seek(std::uint64_t offset) {
        LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) throw std::runtime_error("disk");
    }
    void Truncate(std::uint64_t offset) { Seek(offset); if (!SetEndOfFile(handle)) throw std::runtime_error("disk"); }
    void Write(const char* bytes, std::size_t length) {
        while (length) {
            DWORD written{};
            const auto count = static_cast<DWORD>((std::min)(length, std::size_t{1 << 20}));
            if (!WriteFile(handle, bytes, count, &written, nullptr) || !written) throw std::runtime_error("disk");
            bytes += written; length -= written;
        }
    }
    void Flush() { if (!FlushFileBuffers(handle)) throw std::runtime_error("disk"); }
    BY_HANDLE_FILE_INFORMATION Identity() const {
        BY_HANDLE_FILE_INFORMATION identity{};
        if (!GetFileInformationByHandle(handle, &identity)) throw std::runtime_error("disk");
        return identity;
    }
};

std::uint64_t FileId(const BY_HANDLE_FILE_INFORMATION& info) {
    return (std::uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow;
}

fs::path Filename(const std::string& name) {
    return fs::path{std::u8string{name.begin(), name.end()}};
}

bool ValidName(const std::string& name, bool system_files = false) {
    if (name.size() < 5 || name.size() > 200 || name.back() == ' ' || name.back() == '.') return false;
    for (const unsigned char c : name)
        if (c < 32 || std::string_view{"/\\:<>\"|?*"}.find(static_cast<char>(c)) != std::string_view::npos) return false;
    if (name.find("..") != std::string::npos) return false;
    auto lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (system_files ? !(lower.ends_with(".zip") || lower.ends_with(".nca") || lower=="prod.keys" || lower=="title.keys") :
                       !lower.ends_with(".nsp")) return false;
    const auto stem = lower.substr(0, lower.find('.'));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
        (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' && stem[3] <= '9')) return false;
    try { return Filename(name).native().size() <= 100; } catch (...) { return false; }
}

bool ValidTag(const std::string& tag) {
    return tag.size() <= 64 && tag.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyz:") == std::string::npos;
}

bool Hex(const std::string& value, std::size_t size) {
    return value.size() == size && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string Id() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) throw std::runtime_error("random");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

std::uint64_t Free(const fs::path& path) {
    std::error_code error;
    const auto space = fs::space(path, error);
    return error ? 0 : space.available;
}
} // namespace

struct NspUpload::Impl {
    fs::path games, folder, payload;
    mutable std::mutex mutex;
    std::mutex operation;
    UploadSnapshot info;
    std::uint64_t generation{}, file_id{};
    DWORD volume{};
    std::stop_source cancellation;
    bool system_files{}, consume{};

    explicit Impl(fs::path games_, bool system_files_, fs::path temporary, bool consume_) : games{std::move(games_)},
        folder{temporary.empty() ? games.parent_path() / "jadyx/transfers/nsp" : std::move(temporary)},
        payload{folder / "payload.part"}, system_files{system_files_}, consume{consume_} {
        fs::create_directories(games);
        fs::create_directories(folder);
        Recover();
    }
    void Checkpoint() {
        const Json json{{"version", 1}, {"generation", generation + 1}, {"id", info.id},
            {"name", info.name}, {"fingerprint", info.fingerprint}, {"tag", info.tag}, {"length", info.length},
            {"offset", info.offset}, {"phase", info.phase}, {"file_id", file_id}, {"volume", volume}};
        const auto bytes = json.dump();
        const auto temporary = folder / "checkpoint.tmp";
        { File out{temporary, GENERIC_WRITE, CREATE_ALWAYS}; out.Write(bytes.data(), bytes.size()); out.Flush(); }
        const auto target = folder / ((generation + 1) % 2 ? "checkpoint1.json" : "checkpoint0.json");
        std::error_code error;
        fs::remove(target, error); // The other generation remains intact until this one is published.
        fs::rename(temporary, target);
        ++generation;
    }
    void Recover() {
        for (const auto* name : {"checkpoint0.json", "checkpoint1.json"}) {
            try {
                const auto path = folder / name;
                if (!fs::exists(path) || fs::file_size(path) > 8192) continue;
                std::ifstream input{path};
                const auto json = Json::parse(input);
                const auto next = json.at("generation").get<std::uint64_t>();
                const auto length = json.at("length").get<std::uint64_t>();
                const auto offset = json.at("offset").get<std::uint64_t>();
                const auto filename = json.at("name").get<std::string>();
                const auto id = json.at("id").get<std::string>();
                const auto fingerprint = json.at("fingerprint").get<std::string>();
                const auto tag = json.value("tag", std::string{});
                if (json.at("version") != 1 || next <= generation || !ValidName(filename,system_files) || !ValidTag(tag) ||
                    !Hex(id, 32) || !Hex(fingerprint, 64) || !length || length > MaxLength || offset > length) continue;
                const auto identity = json.at("file_id").get<std::uint64_t>();
                const auto serial = json.at("volume").get<DWORD>();
                // A consumed payload is deleted only after its completion checkpoint.
                bool published = consume && json.at("phase") == "complete";
                // A game deleted since its upload is not published any more, and opening it would throw.
                std::error_code missing;
                if (!consume && (json.at("phase") == "publishing" || json.at("phase") == "complete") &&
                    fs::exists(games / Filename(filename), missing)) {
                    try {
                        File target{games / Filename(filename), GENERIC_READ, OPEN_EXISTING};
                        const auto target_id = target.Identity();
                        published = FileId(target_id) == identity && target_id.dwVolumeSerialNumber == serial &&
                            ((std::uint64_t{target_id.nFileSizeHigh} << 32) | target_id.nFileSizeLow) == length;
                    } catch (...) {}
                }
                if (!published) {
                    if (!fs::exists(payload) || fs::file_size(payload) < offset) continue;
                    File data{payload,GENERIC_READ,OPEN_EXISTING};
                    const auto data_id=data.Identity();
                    if (FileId(data_id) != identity || data_id.dwVolumeSerialNumber != serial) continue;
                }
                info = {.id=id, .name=filename, .fingerprint=fingerprint,
                    .phase=published ? "complete" : "paused", .tag=tag, .length=length, .offset=offset, .received=offset};
                generation = next; file_id = identity; volume = serial;
            } catch (...) {} // Try the independent older checkpoint; never trust a partial JSON write.
        }
        if (consume && info.phase == "complete") { std::error_code ignored; fs::remove(payload, ignored); }
        if (info.id.empty() && fs::exists(payload)) {
            info.id=Id(); info.phase="paused"; info.error="recovery";
        } else if (info.id.empty()) {
            // Nothing left to resume: drop the checkpoints so later starts do not read them again.
            std::error_code ignored;
            fs::remove(folder / "checkpoint0.json", ignored);
            fs::remove(folder / "checkpoint1.json", ignored);
        }
        if (!info.id.empty() && info.phase != "complete" && info.error != "recovery") {
            try { File file{payload, GENERIC_WRITE, OPEN_EXISTING}; file.Truncate(info.offset); }
            catch (...) { info.error = "disk"; }
        }
    }
};

NspUpload::NspUpload(fs::path games, bool system_files, fs::path temporary, bool consume) :
    impl{std::make_unique<Impl>(std::move(games),system_files,std::move(temporary),consume)} {}
NspUpload::~NspUpload() { Cancel(); }

UploadSnapshot NspUpload::Snapshot() const {
    UploadSnapshot result;
    { std::lock_guard lock{impl->mutex}; result = impl->info; }
    result.free_bytes = Free(impl->games);
    return result;
}

bool NspUpload::SetAvailable(bool enabled) {
    std::lock_guard lock{impl->mutex};
    impl->info.available = enabled;
    return !impl->info.busy;
}
bool NspUpload::BlockForLocalOperation() {
    std::lock_guard lock{impl->mutex};
    if (impl->info.busy) return false;
    impl->info.available = false;
    return true;
}
void NspUpload::Cancel() { impl->cancellation.request_stop(); }
void NspUpload::Suspend() {
    Cancel();
    std::lock_guard operation{impl->operation};
    std::lock_guard lock{impl->mutex};
    impl->info.available = impl->info.busy = false;
    if (!impl->info.id.empty() && impl->info.phase != "complete") impl->info.phase = "paused";
    impl->info.received = impl->info.offset;
}
void NspUpload::Resume() {
    std::lock_guard operation{impl->operation};
    std::lock_guard lock{impl->mutex};
    impl->cancellation = std::stop_source{};
}

UploadResult NspUpload::Create(std::string name, std::string fingerprint, std::uint64_t length, std::string tag) {
    if (!ValidName(name,impl->system_files) || !Hex(fingerprint, 64) || !ValidTag(tag) || !length || length > MaxLength) return {400, "file"};
    std::unique_lock operation{impl->operation, std::try_to_lock};
    if (!operation.owns_lock()) return {409, "busy"};
    std::lock_guard lock{impl->mutex};
    if (!impl->info.available || impl->cancellation.stop_requested()) return {409, "busy"};
    if ((!impl->info.id.empty() && impl->info.phase != "complete") || !impl->info.error.empty()) return {409, "partial"};
    try {
        if (!impl->consume && fs::exists(impl->games / Filename(name))) return {409, "exists"};
        // A consumed package is copied once more while it is installed.
        if (Free(impl->games) < (impl->consume ? 2 * length : length) + Reserve) return {507, "space"};
        // Previous complete state is cleared only after its destination has been published.
        for (const auto* checkpoint : {"checkpoint0.json", "checkpoint1.json", "checkpoint.tmp"})
            fs::remove(impl->folder / checkpoint);
        impl->generation = 0;
        File file{impl->payload, GENERIC_WRITE, CREATE_ALWAYS};
        const auto identity = file.Identity();
        impl->file_id = FileId(identity); impl->volume = identity.dwVolumeSerialNumber;
        impl->info = {.id=Id(), .name=std::move(name), .fingerprint=std::move(fingerprint),
            .phase="receiving", .tag=std::move(tag), .length=length, .busy=true, .available=true};
        file.Flush(); impl->Checkpoint();
        return {201, {}};
    } catch (...) { impl->info.busy=false; impl->info.error="disk"; return {507, "disk"}; }
}

UploadResult NspUpload::Append(const std::string& id, std::uint64_t offset, std::uint64_t length, const UploadReader& reader) {
    if (!length || length > MaxChunk) return {413, "chunk"};
    std::unique_lock operation{impl->operation, std::try_to_lock};
    if (!operation.owns_lock()) return {409, "busy"};
    {
        std::lock_guard lock{impl->mutex};
        if (id != impl->info.id || id.empty()) return {404, "upload"};
        if (impl->info.error == "recovery") return {409,"recovery"};
        if (!impl->info.available || impl->cancellation.stop_requested() ||
            impl->info.phase == "ready" || impl->info.phase == "validating" || impl->info.phase == "complete") return {409, "busy"};
        if (offset != impl->info.offset || length > impl->info.length - offset) return {409, "offset"};
        impl->info.busy=true; impl->info.phase="receiving"; impl->info.error.clear();
    }
    const auto started=Clock::now();
    UploadPerformance performance;
    std::string error;
    try {
        if (Free(impl->games) < length + Reserve) throw std::runtime_error("space");
        File file{impl->payload, GENERIC_WRITE, OPEN_EXISTING};
        file.Truncate(offset);
        // Aggregate httplib's small recv buffers into sequential 1 MiB WriteFile calls.
        std::vector<char> buffer(1 << 20);
        std::size_t buffered{};
        std::uint64_t received{};
        const auto write=[&](const char* bytes,std::size_t count) {
            const auto at=Clock::now(); file.Write(bytes,count);
            performance.write_us+=Elapsed(at);
        };
        const bool read = reader([&](const char* bytes, std::size_t count) {
            ++performance.reader_calls;
            if (impl->cancellation.stop_requested() || count > length - received) return false;
            received += count;
            while (count) {
                const auto n = (std::min)(count, buffer.size() - buffered);
                std::copy_n(bytes, n, buffer.data() + buffered);
                bytes += n; count -= n; buffered += n;
                if (buffered == buffer.size()) { write(buffer.data(), buffered); buffered=0; }
            }
            { std::lock_guard lock{impl->mutex}; impl->info.received=offset+received; }
            return true;
        });
        if (!read || received != length || impl->cancellation.stop_requested()) throw std::runtime_error("interrupted");
        if (buffered) write(buffer.data(), buffered);
        const auto flush_at=Clock::now();
        file.Flush(); // Once per HTTP chunk, not per network packet. Checkpoint never leads data.
        performance.flush_us=Elapsed(flush_at);
        std::lock_guard lock{impl->mutex};
        impl->info.offset = offset + length;
        const auto checkpoint_at=Clock::now();
        try { impl->Checkpoint(); }
        catch (...) { impl->info.offset=offset; throw; }
        performance.checkpoint_us=Elapsed(checkpoint_at);
        performance.total_us=Elapsed(started);
        performance.bytes=length; performance.confirmed_offset=offset+length;
        impl->info.performance=performance;
        return {};
    } catch (const std::exception& e) { error=e.what(); }
    catch (...) { error="disk"; }
    try { File file{impl->payload, GENERIC_WRITE, OPEN_EXISTING}; file.Truncate(offset); } catch (...) {}
    std::lock_guard lock{impl->mutex};
    impl->info.offset=impl->info.received=offset;
    impl->info.busy=false; impl->info.phase="paused"; impl->info.error=error;
    return {error == "interrupted" ? 400 : 507, error};
}

UploadResult NspUpload::Discard(const std::string& id) {
    std::unique_lock operation{impl->operation, std::try_to_lock};
    if (!operation.owns_lock()) return {409, "busy"};
    std::lock_guard lock{impl->mutex};
    if (id != impl->info.id) return {404, "upload"};
    try {
        for (const auto* file : {"checkpoint0.json", "checkpoint1.json", "checkpoint.tmp", "payload.part"}) fs::remove(impl->folder / file);
        const bool available=impl->info.available;
        impl->info = {}; impl->info.available=available; impl->generation=0;
        return {};
    } catch (...) { return {507, "disk"}; }
}

UploadResult NspUpload::Pause(const std::string& id) {
    std::unique_lock operation{impl->operation, std::try_to_lock};
    if (!operation.owns_lock()) return {409, "busy"};
    std::lock_guard lock{impl->mutex};
    if (id.empty() || id != impl->info.id) return {404, "upload"};
    if (impl->info.error == "recovery") return {409,"recovery"};
    if (impl->info.phase == "validating" || impl->info.phase == "complete") return {409, "busy"};
    impl->info.phase="paused"; impl->info.busy=false;
    return {};
}
UploadResult NspUpload::Ready(const std::string& id) {
    std::unique_lock operation{impl->operation, std::try_to_lock};
    if (!operation.owns_lock()) return {409, "busy"};
    std::lock_guard lock{impl->mutex};
    if (id.empty() || id != impl->info.id) return {404, "upload"};
    if (impl->info.error == "recovery") return {409,"recovery"};
    if (!impl->info.available || impl->info.offset != impl->info.length ||
        impl->info.phase == "validating" || impl->info.phase == "complete") return {409, "offset"};
    impl->info.phase="ready"; impl->info.busy=true; impl->info.error.clear();
    return {};
}
UploadResult NspUpload::Rename(const std::string& id, std::string name) {
    if (!ValidName(name,impl->system_files)) return {400, "file"};
    std::unique_lock operation{impl->operation, std::try_to_lock};
    if (!operation.owns_lock()) return {409, "busy"};
    std::lock_guard lock{impl->mutex};
    if (id.empty() || id != impl->info.id) return {404, "upload"};
    if (impl->info.error == "recovery") return {409,"recovery"};
    if (impl->info.phase == "validating" || impl->info.phase == "ready" || impl->info.phase == "complete") return {409, "busy"};
    try {
        if (!impl->consume && fs::exists(impl->games / Filename(name))) return {409, "exists"};
        const auto previous=impl->info.name;
        impl->info.name=std::move(name);
        try { impl->Checkpoint(); } catch (...) { impl->info.name=previous; throw; }
        return {};
    } catch (...) { return {507, "disk"}; }
}

std::string NspUpload::Finalize(const NspValidator& validate) {
    std::lock_guard operation{impl->operation};
    {
        std::lock_guard lock{impl->mutex};
        if (impl->info.phase != "ready") return "interrupted";
        impl->info.phase="validating";
    }
    std::string error;
    try {
        error=validate(impl->payload, impl->cancellation.get_token());
        if (impl->cancellation.stop_requested()) error="interrupted";
        if (error.empty() && impl->consume) {
            std::lock_guard lock{impl->mutex};
            impl->info.phase="complete"; impl->info.busy=false;
            // Installed: a crash before the removal deletes the payload on recovery.
            try { impl->Checkpoint(); } catch (...) {}
            std::error_code ignored;
            fs::remove(impl->payload, ignored);
            return {};
        }
        if (error.empty()) {
            std::lock_guard lock{impl->mutex};
            const auto target=impl->games / Filename(impl->info.name);
            if (fs::exists(target)) error="exists";
            else {
                impl->info.phase="publishing"; impl->Checkpoint();
                // Win32 move without replace: never overwrite a file that arrived since the check.
                if (!MoveFileFromAppW(impl->payload.c_str(), target.c_str())) {
                    const auto failure=GetLastError();
                    error=(failure == ERROR_ALREADY_EXISTS || failure == ERROR_FILE_EXISTS) ? "exists" : "disk";
                }
                else {
                    impl->info.phase="complete"; impl->info.busy=false;
                    // The publishing checkpoint + FileId recover a crash at this exact point.
                    try { impl->Checkpoint(); } catch (...) {}
                    return {};
                }
            }
        }
    } catch (...) { error="disk"; }
    std::lock_guard lock{impl->mutex};
    impl->info.phase="paused"; impl->info.busy=false; impl->info.error=error;
    return error;
}
} // namespace Jadyx
