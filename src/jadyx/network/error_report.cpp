// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/network/error_report.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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

/// Replaces dotted IPv4 addresses (with an optional :port) by "<ip>": peers of an online game
/// appear in some network warnings.
std::string Scrub(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
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
    auto report = Common::BugTracker::TakeReport(MaxEntries);
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
