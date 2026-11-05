// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/save_transfer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <set>
#include <string_view>
#include <system_error>

namespace Jadyx {
namespace {
namespace fs = std::filesystem;

/// Left free after the new save is extracted, so a save never takes the last of the disk.
constexpr std::uint64_t SpaceMargin = 64ull << 20;
constexpr std::size_t DetailBytes = 120;

std::string Lower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<std::string_view> Steps(std::string_view path) {
    std::vector<std::string_view> steps;
    for (std::size_t start = 0;;) {
        const auto slash = path.find('/', start);
        steps.push_back(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
        if (slash == std::string_view::npos) return steps;
        start = slash + 1;
    }
}

bool Hex(std::string_view text, std::size_t size) {
    return text.size() == size &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

/// A program id folder (0100...): the title a save belongs to. Other 16-digit folders are the
/// device and user folders around it ("0000000000000000", Ryujinx's "0000000000000001").
bool TitleFolder(std::string_view name) {
    return Hex(name, 16) && name.starts_with("01");
}

/// A folder a save is found inside of when copied from an emulator's tree, never one of a game's.
bool PathFolder(std::string_view name) {
    const auto lower = Lower(name);
    return lower == "bis" || lower == "nand" || lower == "user" || lower == "save" || lower == "account" ||
           (Hex(name, 16) && !TitleFolder(name)) || Hex(name, 32);
}

/// What Mac and Windows add to the archives they make; never part of a save.
bool Junk(std::string_view path) {
    const auto steps = Steps(path);
    if (Lower(steps.front()) == "__macosx") return true;
    const auto name = Lower(steps.back());
    return name == ".ds_store" || name == "thumbs.db" || name == "desktop.ini" || name.starts_with("._");
}

/// A game, update or DLC file: the player picked the wrong file.
bool Package(std::string_view path) {
    const auto name = Lower(Steps(path).back());
    for (const auto extension : {".nsp", ".xci", ".nca", ".nro", ".nsz", ".xcz"})
        if (name.ends_with(extension)) return true;
    return false;
}

/// A name Windows cannot keep: reserved device names, characters it refuses, a trailing dot or space.
bool BadName(std::string_view name) {
    if (name.size() > 255 || name.back() == '.' || name.back() == ' ') return true;
    for (const unsigned char c : name)
        if (c < 0x20 || std::string_view{"<>:\"|?*\\"}.find(static_cast<char>(c)) != std::string_view::npos)
            return true;
    const auto stem = Lower(name.substr(0, name.find('.')));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || stem == "conin$" || stem == "conout$")
        return true;
    return stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' && stem[3] <= '9';
}

/// A path that leaves the folder it is extracted to: from a root or a drive, or through "..".
bool Escapes(std::string_view path) {
    if (path.front() == '/' || path.find('\\') != std::string_view::npos) return true;
    const auto steps = Steps(path);
    if (steps.front().size() == 2 && steps.front()[1] == ':') return true;
    return std::any_of(steps.begin(), steps.end(),
                       [](std::string_view step) { return step.empty() || step == "." || step == ".."; });
}

/// `text` as valid UTF-8 without control characters, cut to a short length: the page shows it,
/// and the portal's JSON refuses anything else.
std::string Clean(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size() && out.size() < DetailBytes;) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            out += c < 0x20 || c == 0x7F ? '?' : static_cast<char>(c);
            ++i;
            continue;
        }
        const std::size_t length = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        bool valid = length != 0 && i + length <= text.size();
        for (std::size_t k = 1; valid && k < length; ++k)
            valid = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
        if (valid) {
            const auto second = static_cast<unsigned char>(text[i + 1]);
            valid = !(c == 0xE0 && second < 0xA0) && !(c == 0xED && second >= 0xA0) &&
                    !(c == 0xF0 && second < 0x90) && !(c == 0xF4 && second >= 0x90);
        }
        if (!valid) {
            out += '?';
            ++i;
            continue;
        }
        out.append(text.substr(i, length));
        i += length;
    }
    return out;
}

std::string TitleName(std::uint64_t application) {
    char name[17];
    std::snprintf(name, sizeof(name), "%016llX", static_cast<unsigned long long>(application));
    return name;
}

std::string Utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

/// The folders and whether there are files right under `prefix` (an archive folder ending in '/').
struct Level {
    std::set<std::string> folders;
    bool files{};
};

/// Where to go from a level: into one of its folders, nowhere, or it is another game's save.
struct Step {
    enum class Kind { Stay, Enter, WrongGame } kind{};
    std::string name;
};

Step Next(const Level& level, std::string_view ours) {
    if (level.files || level.folders.empty()) return {};
    const bool titles = std::all_of(level.folders.begin(), level.folders.end(),
                                    [](const std::string& f) { return TitleFolder(f); });
    if (titles) {
        for (const auto& folder : level.folders)
            if (Lower(folder) == Lower(ours)) return {Step::Kind::Enter, folder};
        std::string found;
        std::size_t listed = 0;
        for (const auto& folder : level.folders) {
            if (listed++ == 5) break;
            found += (found.empty() ? "" : ",") + folder;
        }
        return {Step::Kind::WrongGame, found};
    }
    // The newer layout keeps the save in "0" (and Ryujinx its previous commit in "1").
    if (level.folders == std::set<std::string>{"0"} || level.folders == std::set<std::string>{"0", "1"})
        return {Step::Kind::Enter, "0"};
    return {};
}

std::string ArchiveWord(Mods::ArchiveError error) {
    switch (error) {
    case Mods::ArchiveError::None:
        return {};
    case Mods::ArchiveError::Encrypted:
        return "encrypted";
    case Mods::ArchiveError::TooLarge:
        return "save_limit";
    case Mods::ArchiveError::Write:
        return "disk";
    case Mods::ArchiveError::Cancelled:
        return "interrupted";
    case Mods::ArchiveError::Unreadable:
        break;
    }
    return "archive";
}

// A ZIP of stored entries: the save is small and libarchive is linked here only to read.
constexpr std::array<std::uint32_t, 256> CrcTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) value = (value & 1) ? 0xEDB88320u ^ (value >> 1) : value >> 1;
        table[i] = value;
    }
    return table;
}();

std::uint32_t Crc32(std::string_view data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char c : data) crc = CrcTable[(crc ^ c) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void Put16(std::string& out, std::uint32_t value) {
    out += static_cast<char>(value & 0xFF);
    out += static_cast<char>((value >> 8) & 0xFF);
}

void Put32(std::string& out, std::uint32_t value) {
    Put16(out, value & 0xFFFF);
    Put16(out, value >> 16);
}

struct ZipEntry {
    std::string name; ///< '/'-separated, ending in '/' for a folder.
    fs::path path;
    std::uint64_t size{};
};

std::pair<std::uint16_t, std::uint16_t> DosNow() {
    const auto now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const auto time = (local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2);
    const auto date = (std::max(local.tm_year - 80, 0) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday;
    return {static_cast<std::uint16_t>(time), static_cast<std::uint16_t>(date)};
}

} // namespace

SavePlan PlanSaveImport(const std::vector<Mods::ArchiveItem>& items, std::uint64_t application) {
    const auto fail = [](std::string error, std::string_view detail = {}) {
        SavePlan plan;
        plan.error = std::move(error);
        plan.detail = Clean(detail);
        return plan;
    };
    std::vector<const Mods::ArchiveItem*> kept;
    std::uint64_t declared = 0;
    for (const auto& item : items) {
        if (item.path.empty()) continue;
        if (!Mods::SafeRelativePath(item.path)) {
            // A name Windows refuses (a ':' from a Mac, a control character) is the player's to
            // rename; a path that climbs out or starts at a root is not used at all.
            if (Escapes(item.path)) return fail("archive_unsafe", item.path);
            for (const auto step : Steps(item.path))
                if (BadName(step)) return fail("bad_name", step);
            return fail("archive_unsafe", item.path);
        }
        if (Junk(item.path)) continue;
        if (Package(item.path)) return fail("not_save", Steps(item.path).back());
        kept.push_back(&item);
        declared += item.size;
        if (kept.size() > SaveMaxFiles || declared > SaveMaxImportBytes) return fail("save_limit");
    }
    if (kept.empty()) return fail("empty");

    const auto level = [&](const std::string& prefix) {
        Level result;
        for (const auto* item : kept) {
            if (!item->path.starts_with(prefix)) continue;
            const auto rest = std::string_view{item->path}.substr(prefix.size());
            const auto slash = rest.find('/');
            if (slash == std::string_view::npos) result.files = true;
            else result.folders.emplace(rest.substr(0, slash));
        }
        return result;
    };
    const auto ours = TitleName(application);
    std::string root;
    for (int depth = 0; depth < 16; ++depth) {
        const auto here = level(root);
        const auto next = Next(here, ours);
        if (next.kind == Step::Kind::WrongGame) return fail("wrong_game", next.name);
        if (next.kind == Step::Kind::Enter) {
            root += next.name + "/";
            continue;
        }
        // An emulator's folders around the save (user/save/<device>/<user>/...) are stepped
        // through only when they lead to a title or "0" folder: a game's own "save" folder stays.
        if (here.files || here.folders.size() != 1 || !PathFolder(*here.folders.begin())) break;
        auto probe = root + *here.folders.begin() + "/";
        bool reached = false;
        for (int chain = 0; chain < 8; ++chain) {
            const auto inner = level(probe);
            if (Next(inner, ours).kind != Step::Kind::Stay) {
                reached = true;
                break;
            }
            if (inner.files || inner.folders.size() != 1 || !PathFolder(*inner.folders.begin())) break;
            probe += *inner.folders.begin() + "/";
        }
        if (!reached) break;
        root = std::move(probe);
    }

    SavePlan plan;
    std::set<std::string> files, folders;
    for (const auto* item : kept) {
        if (!item->path.starts_with(root)) continue;
        auto to = item->path.substr(root.size());
        if (to.empty()) continue;
        for (const auto step : Steps(to))
            if (BadName(step)) return fail("bad_name", step);
        // Windows folds case: two names that differ only in it would be one file.
        const auto key = Lower(to);
        if (!files.insert(key).second) return fail("bad_name", to);
        for (auto slash = key.find('/'); slash != std::string::npos; slash = key.find('/', slash + 1))
            folders.insert(key.substr(0, slash));
        plan.bytes += item->size;
        plan.files.push_back({item->path, std::move(to), item->size});
    }
    for (const auto& folder : folders)
        if (files.contains(folder)) return fail("bad_name", folder);
    if (plan.files.empty()) return fail("empty");
    return plan;
}

std::string ExportSaveFolder(const fs::path& folder, std::string& zip) {
    zip.clear();
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return "no_save";
    std::vector<ZipEntry> entries;
    std::uint64_t total = 0;
    bool any_file = false;
    for (fs::recursive_directory_iterator it{folder, ec}, end; it != end; it.increment(ec)) {
        if (ec) return "disk";
        if (it->is_symlink(ec)) {
            it.disable_recursion_pending();
            continue;
        }
        const auto relative = Utf8(it->path().lexically_relative(folder));
        if (it->is_directory(ec)) {
            entries.push_back({relative + "/", {}, 0});
        } else if (it->is_regular_file(ec)) {
            const auto size = it->file_size(ec);
            if (ec) return "disk";
            entries.push_back({relative, it->path(), size});
            total += size;
            any_file = true;
        } else {
            continue;
        }
        if (entries.size() > SaveMaxFiles || total > SaveMaxExportBytes) return "save_limit";
    }
    if (ec) return "disk";
    if (!any_file) return "no_save";
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });

    const auto [time, date] = DosNow();
    std::string central;
    zip.reserve(static_cast<std::size_t>(total) + entries.size() * 128);
    for (const auto& entry : entries) {
        std::string data;
        if (!entry.path.empty()) {
            std::ifstream file{entry.path, std::ios::binary};
            data.resize(static_cast<std::size_t>(entry.size));
            if (!file || !file.read(data.data(), static_cast<std::streamsize>(data.size()))) {
                zip.clear();
                return "disk";
            }
        }
        const auto crc = Crc32(data);
        const auto offset = static_cast<std::uint32_t>(zip.size());
        const auto size = static_cast<std::uint32_t>(data.size());
        const bool directory = entry.name.ends_with('/');
        // Local header: stored, UTF-8 names (bit 11).
        Put32(zip, 0x04034B50);
        Put16(zip, 20);
        Put16(zip, 0x0800);
        Put16(zip, 0);
        Put16(zip, time);
        Put16(zip, date);
        Put32(zip, crc);
        Put32(zip, size);
        Put32(zip, size);
        Put16(zip, static_cast<std::uint32_t>(entry.name.size()));
        Put16(zip, 0);
        zip += entry.name;
        zip += data;
        Put32(central, 0x02014B50);
        Put16(central, 20);
        Put16(central, 20);
        Put16(central, 0x0800);
        Put16(central, 0);
        Put16(central, time);
        Put16(central, date);
        Put32(central, crc);
        Put32(central, size);
        Put32(central, size);
        Put16(central, static_cast<std::uint32_t>(entry.name.size()));
        Put16(central, 0);
        Put16(central, 0);
        Put16(central, 0);
        Put16(central, 0);
        Put32(central, directory ? 0x10 : 0);
        Put32(central, offset);
        central += entry.name;
    }
    const auto central_offset = static_cast<std::uint32_t>(zip.size());
    zip += central;
    Put32(zip, 0x06054B50);
    Put16(zip, 0);
    Put16(zip, 0);
    Put16(zip, static_cast<std::uint32_t>(entries.size()));
    Put16(zip, static_cast<std::uint32_t>(entries.size()));
    Put32(zip, static_cast<std::uint32_t>(central.size()));
    Put32(zip, central_offset);
    Put16(zip, 0);
    return {};
}

std::string ImportSaveArchive(const fs::path& archive, std::uint64_t application, const fs::path& target,
                              const fs::path& backups, std::stop_token stop, const Mods::ArchiveProgress& progress,
                              std::string* log) {
    const auto fail = [&](const std::string& word, const std::string& detail, const std::string& why) {
        if (log) *log = why;
        return detail.empty() ? word : word + ":" + detail;
    };
    std::vector<std::unique_ptr<Mods::ArchiveSource>> volumes;
    if (auto source = Mods::OpenFileSource(archive)) volumes.push_back(std::move(source));
    else return fail("disk", {}, "cannot open the upload");
    const auto listing = Mods::ListArchive(volumes, stop);
    if (listing.error != Mods::ArchiveError::None)
        return fail(ArchiveWord(listing.error), {}, listing.detail);
    const auto plan = PlanSaveImport(listing.items, application);
    if (!plan.error.empty()) return fail(plan.error, plan.detail, plan.detail);
    if (stop.stop_requested()) return fail("interrupted", {}, "cancelled");

    std::error_code ec;
    const auto parent = target.parent_path();
    fs::create_directories(parent, ec);
    if (ec) return fail("disk", {}, "cannot create the save folder: " + ec.message());
    const auto space = fs::space(parent, ec);
    if (!ec && (space.available < SpaceMargin || plan.bytes > space.available - SpaceMargin))
        return fail("space", {}, std::to_string(plan.bytes) + " bytes needed");

    // Built beside the save, so placing it is a rename on the same volume.
    auto staging = target;
    staging += ".import";
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) return fail("disk", {}, "cannot create the staging folder: " + ec.message());
    std::string detail;
    const auto extracted = Mods::ExtractArchive(volumes, plan.files, staging, SaveMaxImportBytes, stop, progress, &detail);
    if (extracted != Mods::ArchiveError::None) {
        fs::remove_all(staging, ec);
        return fail(ArchiveWord(extracted), {}, detail);
    }

    fs::path backup;
    if (fs::exists(target, ec)) {
        const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count();
        backup = backups / TitleName(application) / std::to_string(stamp);
        for (int n = 2; fs::exists(backup, ec); ++n)
            backup = backups / TitleName(application) / (std::to_string(stamp) + "-" + std::to_string(n));
        fs::create_directories(backup.parent_path(), ec);
        fs::rename(target, backup, ec);
        if (ec) {
            std::error_code undo;
            fs::remove_all(staging, undo);
            return fail("disk", {}, "cannot back up the save: " + ec.message());
        }
    }
    fs::rename(staging, target, ec);
    if (ec) {
        std::error_code undo;
        if (!backup.empty()) fs::rename(backup, target, undo);
        fs::remove_all(staging, undo);
        return fail("disk", {}, "cannot place the save: " + ec.message());
    }
    if (log)
        *log = std::to_string(plan.files.size()) + " files, " + std::to_string(plan.bytes) + " bytes" +
               (backup.empty() ? "" : ", the previous save kept in " + Utf8(backup));
    return {};
}

} // namespace Jadyx
