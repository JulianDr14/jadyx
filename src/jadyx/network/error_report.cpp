// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/network/error_report.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>

#include "jadyx_report_endpoint.h" // generated: the masked collector address

#include "common/bug_tracker.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "jadyx/app/boot_diag.h"
#include "jadyx/mods/work_queue.h"
#include "jadyx/network/http.h"

namespace Jadyx::ErrorReport {
namespace {

// Only keeps stray requests out of the collector; the collector checks the reports itself.
constexpr const char* ClientHeader = "X-Jadyx-Report";
constexpr const char* ClientValue = "jadyx-error-report-v1";

constexpr std::size_t MaxEntries = 200;
constexpr std::size_t MaxMessage = 300;
constexpr std::size_t MaxPending = 8;

struct Game {
    std::uint64_t title_id = 0;
    std::string title;
    std::string version;
};

std::mutex game_mutex;
Game current_game;

std::string AppVersion() {
    try {
        const auto v = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
        return fmt::format("{}.{}.{}.{}", v.Major, v.Minor, v.Build, v.Revision);
    } catch (...) {
        return "?"; // the native harness has no package identity
    }
}

std::string PackageName() {
    try {
        return winrt::to_string(winrt::Windows::ApplicationModel::Package::Current().Id().Name());
    } catch (...) {
        return {};
    }
}

std::filesystem::path PendingFolder() {
    try {
        return std::filesystem::path{std::wstring{
                   winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()}} /
               L"jadyx" / L"reports";
    } catch (...) {
        return {};
    }
}

bool Enabled() {
    return !Endpoint::Masked.empty();
}

/// Unmasks the collector address for one request. The volatile read keeps the optimizer from
/// folding the plain address back into the binary.
std::string CollectorUrl() {
    const volatile std::uint8_t* masked = Endpoint::Masked.data();
    std::string url(Endpoint::Masked.size(), '\0');
    for (std::size_t i = 0; i < url.size(); ++i) {
        const auto mask = static_cast<std::uint8_t>((Endpoint::Seed + i * Endpoint::Step) % 256);
        url[i] = static_cast<char>(masked[i] ^ mask);
    }
    return url;
}

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

bool IsHex(char c) {
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool IsWordChar(char c) {
    return IsHex(c) || (c >= 'g' && c <= 'z') || (c >= 'G' && c <= 'Z') || c == '_';
}

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool StartsWithNoCase(std::string_view text, std::size_t at, std::string_view prefix) {
    if (text.size() - at < prefix.size()) return false;
    for (std::size_t k = 0; k < prefix.size(); ++k) {
        if (Lower(text[at + k]) != prefix[k]) return false;
    }
    return true;
}

/// An absolute path at `at` ("C:\..." or "C:/..."): its length, or 0. Ends at whitespace, quotes,
/// brackets, commas and the ':' that follows a file name.
std::size_t PathLength(std::string_view text, std::size_t at) {
    if (text.size() - at < 3 || !IsWordChar(text[at]) || IsDigit(text[at]) || text[at + 1] != ':' ||
        (text[at + 2] != '\\' && text[at + 2] != '/') || (at > 0 && IsWordChar(text[at - 1]))) {
        return 0;
    }
    std::size_t j = at + 3;
    while (j < text.size()) {
        const char c = text[j];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\'' || c == '"' || c == ',' || c == ':' ||
            c == ')' || c == ']' || c == '>') {
            break;
        }
        ++j;
    }
    return j - at;
}

/// A source path keeps only its part below src/ ("core/hle/service/service.cpp"); any other path
/// (the build machine's folders, the player's storage) becomes "<path>".
std::string ScrubPath(std::string_view path) {
    std::size_t below = std::string_view::npos;
    for (std::size_t k = 0; k + 5 <= path.size(); ++k) {
        if ((path[k] == '\\' || path[k] == '/') && StartsWithNoCase(path, k + 1, "src") &&
            (path[k + 4] == '\\' || path[k + 4] == '/')) {
            below = k + 5;
        }
    }
    if (below == std::string_view::npos || below >= path.size()) return "<path>";
    std::string out{path.substr(below)};
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

/// A player identifier at `at`: a dashed UUID (8-4-4-4-12) or a run of 32+ hex digits, with an
/// optional 0x. Its length, or 0. Account and friend services print the profile's UUID.
std::size_t IdLength(std::string_view text, std::size_t at) {
    if (at > 0 && IsWordChar(text[at - 1])) return 0;
    std::size_t j = at;
    if (text.size() - j >= 2 && text[j] == '0' && (text[j + 1] == 'x' || text[j + 1] == 'X')) j += 2;
    const std::size_t digits_at = j;
    while (j < text.size() && IsHex(text[j])) ++j;
    if (j - digits_at >= 32 && (j == text.size() || !IsWordChar(text[j]))) return j - at;
    // Dashed form, no 0x.
    static constexpr std::size_t Groups[] = {8, 4, 4, 4, 12};
    j = at;
    for (std::size_t g = 0; g < std::size(Groups); ++g) {
        const std::size_t start = j;
        while (j < text.size() && IsHex(text[j])) ++j;
        if (j - start != Groups[g]) return 0;
        if (g + 1 < std::size(Groups)) {
            if (j >= text.size() || text[j] != '-') return 0;
            ++j;
        }
    }
    return j == text.size() || !IsWordChar(text[j]) ? j - at : 0;
}

/// Takes out what could point at a person: dotted IPv4 addresses (with an optional :port) become
/// "<ip>" (online peers in network warnings), absolute paths lose everything above src/ or become
/// "<path>" (assert messages print the build machine's full source path), and profile UUIDs become
/// "<id>" (account and friend services print them).
std::string Scrub(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        if (const std::size_t path = PathLength(text, i)) {
            out += ScrubPath(text.substr(i, path));
            i += path;
            continue;
        }
        if (const std::size_t id = IdLength(text, i)) {
            out += "<id>";
            i += id;
            continue;
        }
        if (IsDigit(text[i]) && (i == 0 || !IsDigit(text[i - 1]))) {
            std::size_t j = i;
            int groups = 0;
            while (groups < 4) {
                const std::size_t start = j;
                while (j < text.size() && IsDigit(text[j]) && j - start < 3) ++j;
                if (j == start) break;
                ++groups;
                if (groups < 4) {
                    if (j < text.size() && text[j] == '.') ++j;
                    else break;
                }
            }
            if (groups == 4 && (j == text.size() || !IsDigit(text[j]))) {
                if (j < text.size() && text[j] == ':') {
                    std::size_t k = j + 1;
                    while (k < text.size() && IsDigit(text[k])) ++k;
                    if (k > j + 1) j = k;
                }
                out += "<ip>";
                i = j;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

/// Whether a tracked bug belongs in a report. Only what points at an emulation gap: graphics and
/// texture failures, unimplemented functions and failed asserts. HLE stubs (every game calls the
/// same ones), input/loader/network chatter and renderer diagnostics stay in the local log. Runs
/// once per ended game on the report thread, so it costs the game nothing.
bool Worth(const Common::BugTracker::ReportEntry& entry) {
    using Common::BugTracker::Category;
    switch (entry.category) {
    case Category::Unimplemented:
    case Category::AssertFailed:
    case Category::UnsupportedState:
    case Category::UnsupportedFormat:
    case Category::ShaderCompile:
    case Category::PipelineRejected:
    case Category::DrawSkipped:
    case Category::CopySkipped:
    case Category::ApiCallFailed:
    case Category::DeviceRemoved:
    case Category::NonFinite:
    case Category::GpuHle:
        return true;
    case Category::Renderer:
        // Renderer warnings are mostly trace output (readbacks, dispatch counters); keep errors.
        return entry.location.find("readback_trace") == std::string::npos &&
               (entry.message.starts_with("Error ") || entry.message.starts_with("Critical "));
    default:
        return false; // Stubbed, Other
    }
}

std::string Build(const Game& game, const std::vector<Common::BugTracker::ReportEntry>& report) {
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& entry : report) {
        std::string message = Scrub(entry.message);
        if (message.size() > MaxMessage) message.resize(MaxMessage);
        entries.push_back({
            {"category", std::string{Common::BugTracker::Name(entry.category)}},
            {"location", entry.location},
            {"key", entry.keyed ? fmt::format("{:#x}", entry.key) : std::string{}},
            {"hits", entry.hits},
            {"message", std::move(message)},
        });
    }
    const nlohmann::json body{
        {"schema", 1},
        {"app_version", AppVersion()},
        {"package", PackageName()},
        {"build", std::string{Common::g_scm_desc}},
        {"title_id", fmt::format("{:016X}", game.title_id)},
        {"title", game.title},
        {"game_version", game.version},
        {"entries", std::move(entries)},
    };
    return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool Post(const std::string& body, std::stop_token stop) {
    Http::ApiRequest request;
    request.method = "POST";
    request.url = CollectorUrl();
    request.content_type = "application/json";
    request.body = body;
    request.headers.emplace_back(ClientHeader, ClientValue);
    request.max_bytes = 64u << 10;
    const auto response = Http::Send(request, stop);
    return response.status >= 200 && response.status < 300;
}

void Keep(const std::filesystem::path& folder, const std::string& body) {
    if (folder.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::vector<std::filesystem::path> files;
    for (const auto& item : std::filesystem::directory_iterator(folder, error)) {
        if (item.path().extension() == L".json") files.push_back(item.path());
    }
    std::sort(files.begin(), files.end()); // names start with the time, oldest first
    while (files.size() >= MaxPending) {
        std::filesystem::remove(files.front(), error);
        files.erase(files.begin());
    }
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto name = fmt::format("{:020}.json", std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    std::ofstream file(folder / name, std::ios::binary | std::ios::trunc);
    file.write(body.data(), static_cast<std::streamsize>(body.size()));
}

/// Sends the reports kept by earlier failures; stops at the first that fails.
void SendKept(const std::filesystem::path& folder, std::stop_token stop) {
    if (folder.empty()) return;
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (const auto& item : std::filesystem::directory_iterator(folder, error)) {
        if (item.path().extension() == L".json") files.push_back(item.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
        std::ifstream file(path, std::ios::binary);
        const std::string body{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        file.close();
        if (!body.empty() && !Post(body, stop)) return;
        std::filesystem::remove(path, error);
    }
}

Mods::WorkQueue& Queue() {
    static Mods::WorkQueue queue;
    return queue;
}

} // namespace

void GameStarted(std::uint64_t title_id, std::string title, std::string version) {
    if (!Enabled()) return; // a build without JADYX_REPORT_URL sends nothing
    // Bugs hit before the game (the library, a previous failed load) are not this game's.
    void(Common::BugTracker::TakeReport(0));
    const std::scoped_lock lock{game_mutex};
    current_game = {title_id, std::move(title), std::move(version)};
}

void GameEnded() {
    Game game;
    {
        const std::scoped_lock lock{game_mutex};
        game = std::exchange(current_game, {});
    }
    if (game.title_id == 0) return;
    // Take every bug (this also re-arms the tracker), keep the worthwhile ones, then cap.
    auto report = Common::BugTracker::TakeReport(std::numeric_limits<std::size_t>::max());
    std::erase_if(report, [](const auto& entry) { return !Worth(entry); });
    if (report.size() > MaxEntries) report.resize(MaxEntries);
    // Airplane mode: no network at all; the counts are dropped with the session.
    if (report.empty() || Settings::values.airplane_mode.GetValue()) return;
    WriteDiag(fmt::format("error report: {} distinct bugs for {:016X}, sending", report.size(),
                          game.title_id));
    Queue().Post([body = Build(game, report)](std::stop_token stop) {
        const auto folder = PendingFolder();
        if (!Post(body, stop)) {
            Keep(folder, body);
            WriteDiag("error report: not sent, kept for the next game");
            return;
        }
        WriteDiag("error report: sent");
        SendKept(folder, stop);
    });
}

} // namespace Jadyx::ErrorReport
