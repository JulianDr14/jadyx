// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/network/error_report.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include <thread>

#include <windows.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>

#include "jadyx_report_endpoint.h" // generated: the masked collector address

#include "common/bug_tracker.h"
#include "common/scm_rev.h"
#include "jadyx/app/boot_diag.h"
#include "jadyx/mods/work_queue.h"
#include "jadyx/network/http.h"
#include "video_core/perf_counters.h"

namespace Jadyx::ErrorReport {
namespace {

// Only keeps stray requests out of the collector; the collector checks the reports itself.
constexpr const char* ClientHeader = "X-Jadyx-Report";
constexpr const char* ClientValue = "jadyx-error-report-v1";

constexpr std::size_t MaxEntries = 200;
constexpr std::size_t MaxMessage = 300;
constexpr std::size_t MaxPending = 8;

// Free memory under the app's limit below which a sample counts as memory pressure.
constexpr std::uint64_t LowAppMemory = 512ULL << 20;

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

/// Kernel + user time of every thread of the process, in microseconds.
std::uint64_t ProcessCpuUs() {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    const auto ticks = [](const FILETIME& t) {
        return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) / 10; // 100 ns ticks
}

/// GPU time of the submitted command lists (timestamps), in microseconds. Not whole-GPU
/// utilization: the direct queue only.
std::uint64_t GpuBusyUs() {
    return VideoCore::Perf::counters[static_cast<std::size_t>(VideoCore::Perf::Counter::GpuBusyUs)]
        .load(std::memory_order_relaxed);
}

/// A game's load: running totals at its start and at the last sample, and the peaks between
/// samples.
struct Load {
    std::uint64_t start_ms = 0;
    std::uint64_t start_cpu_us = 0;
    std::uint64_t start_gpu_us = 0;
    std::uint64_t last_ms = 0;
    std::uint64_t last_cpu_us = 0;
    std::uint64_t last_gpu_us = 0;
    std::uint64_t app_peak = 0;
    std::uint64_t app_limit = 0;
    std::uint64_t app_low_ms = 0;
    std::uint64_t gpu_mem_peak = 0;
    std::uint64_t gpu_mem_budget = 0;
    double cpu_peak_cores = 0.0;
    double gpu_peak_pct = 0.0;
};

struct Game {
    std::uint64_t title_id = 0;
    std::string title;
    std::string version;
    Load load;
};

std::mutex game_mutex;
Game current_game;

constexpr double MiB = 1024.0 * 1024.0;

double Round1(double value) {
    return static_cast<double>(static_cast<std::int64_t>(value * 10.0 + 0.5)) / 10.0;
}

/// The session's load as sent: averages over the whole game, peaks over 10 s windows.
nlohmann::json DescribeLoad(const Load& load, std::uint64_t end_ms, std::uint64_t end_cpu_us,
                            std::uint64_t end_gpu_us) {
    if (end_ms <= load.start_ms) return nullptr;
    const double seconds = static_cast<double>(end_ms - load.start_ms) / 1000.0;
    const double cpu_avg =
        end_cpu_us >= load.start_cpu_us
            ? static_cast<double>(end_cpu_us - load.start_cpu_us) / 1e6 / seconds
            : 0.0;
    const double gpu_avg =
        end_gpu_us >= load.start_gpu_us
            ? static_cast<double>(end_gpu_us - load.start_gpu_us) / 1e4 / seconds
            : 0.0;
    return {
        {"seconds", static_cast<std::uint64_t>(seconds)},
        {"app_mem_peak_mib", static_cast<std::uint64_t>(load.app_peak / MiB)},
        {"app_mem_limit_mib", static_cast<std::uint64_t>(load.app_limit / MiB)},
        {"app_mem_low_seconds", load.app_low_ms / 1000},
        {"gpu_mem_peak_mib", static_cast<std::uint64_t>(load.gpu_mem_peak / MiB)},
        {"gpu_mem_budget_mib", static_cast<std::uint64_t>(load.gpu_mem_budget / MiB)},
        {"cpu_threads", std::thread::hardware_concurrency()},
        {"cpu_cores_avg", Round1(cpu_avg)},
        {"cpu_cores_peak", Round1((std::max)(load.cpu_peak_cores, cpu_avg))},
        {"gpu_busy_avg_pct", Round1(gpu_avg)},
        {"gpu_busy_peak_pct", Round1((std::max)(load.gpu_peak_pct, gpu_avg))},
    };
}

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

/// The tracked bugs worth reporting, capped. `take` moves them out of the tracker (the game
/// ended); otherwise they stay for the next read (a checkpoint of the running game).
std::vector<Common::BugTracker::ReportEntry> Collect(bool take) {
    constexpr auto All = std::numeric_limits<std::size_t>::max();
    auto report = take ? Common::BugTracker::TakeReport(All) : Common::BugTracker::PeekReport(All);
    std::erase_if(report, [](const auto& entry) { return !Worth(entry); });
    if (report.size() > MaxEntries) report.resize(MaxEntries);
    return report;
}

/// How the session ended: "closed" from GameEnded, "interrupted" for a checkpoint found at the
/// next launch (the app was closed by force, suspended and terminated, or crashed).
std::string Build(const Game& game, const std::vector<Common::BugTracker::ReportEntry>& report,
                  nlohmann::json load, const char* ended) {
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
        {"load", std::move(load)},
        {"ended", ended},
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

/// The running game's report, rewritten every sample. GameEnded removes it; one still there at
/// launch belongs to a session that never reached GameEnded. Not ".json", so SendKept skips it.
std::filesystem::path LiveFile(const std::filesystem::path& folder) {
    return folder / L"live.session";
}

/// Writes through a temporary file, so a kill mid-write leaves the previous checkpoint whole.
void WriteLive(const std::filesystem::path& folder, const std::string& body) {
    if (folder.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    const auto temp = folder / L"live.session.tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!file) return;
    }
    std::filesystem::rename(temp, LiveFile(folder), error);
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

/// A checkpoint waits in the queue (behind a slow send); the next sample skips its own.
std::atomic<bool> checkpoint_queued{false};

} // namespace

void SendLeftovers() {
    if (!Enabled()) return;
    static std::once_flag once; // the library launches several games in one process
    std::call_once(once, [] {
        Queue().Post([](std::stop_token stop) {
            const auto folder = PendingFolder();
            if (folder.empty()) return;
            std::error_code error;
            const auto live = LiveFile(folder);
            if (std::filesystem::exists(live, error)) {
                std::ifstream file(live, std::ios::binary);
                const std::string body{std::istreambuf_iterator<char>(file),
                                       std::istreambuf_iterator<char>()};
                file.close();
                if (!body.empty()) Keep(folder, body);
                std::filesystem::remove(live, error);
                WriteDiag("error report: kept the report of a session that did not end normally");
            }
            SendKept(folder, stop);
        });
    });
}

void GameStarted(std::uint64_t title_id, std::string title, std::string version) {
    if (!Enabled()) return; // a build without JADYX_REPORT_URL sends nothing
    // Bugs hit before the game (the library, a previous failed load) are not this game's.
    void(Common::BugTracker::TakeReport(0));
    const std::uint64_t now = NowMs();
    const std::uint64_t cpu = ProcessCpuUs();
    const std::uint64_t gpu = GpuBusyUs();
    const std::scoped_lock lock{game_mutex};
    current_game = {title_id, std::move(title), std::move(version),
                    Load{.start_ms = now, .start_cpu_us = cpu, .start_gpu_us = gpu,
                         .last_ms = now, .last_cpu_us = cpu, .last_gpu_us = gpu}};
}

void SampleLoad(const Common::MemoryCeilings& ceilings) {
    if (!Enabled()) return;
    const std::uint64_t now = NowMs();
    const std::uint64_t cpu = ProcessCpuUs();
    const std::uint64_t gpu = GpuBusyUs();
    const std::scoped_lock lock{game_mutex};
    if (current_game.title_id == 0) return;
    Load& load = current_game.load;
    const std::uint64_t elapsed_ms = now - load.last_ms;
    if (elapsed_ms > 0) {
        const double seconds = static_cast<double>(elapsed_ms) / 1000.0;
        if (cpu >= load.last_cpu_us) {
            load.cpu_peak_cores = (std::max)(
                load.cpu_peak_cores, static_cast<double>(cpu - load.last_cpu_us) / 1e6 / seconds);
        }
        if (gpu >= load.last_gpu_us) {
            load.gpu_peak_pct = (std::max)(
                load.gpu_peak_pct, static_cast<double>(gpu - load.last_gpu_us) / 1e4 / seconds);
        }
    }
    if (const auto& app = ceilings[Common::MemoryCeiling::App]; app.Known()) {
        load.app_peak = (std::max)(load.app_peak, app.used);
        load.app_limit = app.limit;
        if (app.Free() < LowAppMemory) load.app_low_ms += elapsed_ms;
    }
    if (const auto& gpu_mem = ceilings[Common::MemoryCeiling::Gpu]; gpu_mem.Known()) {
        load.gpu_mem_peak = (std::max)(load.gpu_mem_peak, gpu_mem.used);
        load.gpu_mem_budget = gpu_mem.limit;
    }
    load.last_ms = now;
    load.last_cpu_us = cpu;
    load.last_gpu_us = gpu;

    // Checkpoint the report on the report thread: if the session never reaches GameEnded, the
    // next launch sends what was saved here (at most one sample old).
    if (checkpoint_queued.exchange(true, std::memory_order_acq_rel)) return;
    Queue().Post([game = current_game](std::stop_token) {
        checkpoint_queued.store(false, std::memory_order_release);
        const auto report = Collect(false);
        if (report.empty()) return;
        const auto end = game.load.last_ms;
        WriteLive(PendingFolder(),
                  Build(game, report,
                        DescribeLoad(game.load, end, game.load.last_cpu_us, game.load.last_gpu_us),
                        "interrupted"));
    });
}

void GameEnded() {
    Game game;
    {
        const std::scoped_lock lock{game_mutex};
        game = std::exchange(current_game, {});
    }
    if (game.title_id == 0) return;
    // Take every bug (this also re-arms the tracker), keep the worthwhile ones, then cap.
    // The game's offline mode is not checked: it keeps the game off the internet, not the app.
    const auto report = Collect(true);
    std::string body;
    if (!report.empty()) {
        WriteDiag(fmt::format("error report: {} distinct bugs for {:016X}, sending",
                              report.size(), game.title_id));
        body = Build(game, report, DescribeLoad(game.load, NowMs(), ProcessCpuUs(), GpuBusyUs()),
                     "closed");
    }
    // Queued after this game's checkpoints, so none of them outlives this removal.
    Queue().Post([body = std::move(body)](std::stop_token stop) {
        const auto folder = PendingFolder();
        std::error_code error;
        if (!folder.empty()) std::filesystem::remove(LiveFile(folder), error);
        if (body.empty()) return;
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
