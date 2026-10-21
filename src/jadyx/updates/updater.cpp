// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include <fileapifromapp.h>

#include <chrono>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.h>
#include "jadyx/network/http.h"
#include "jadyx/storage/atomic_file.h"
#include "jadyx/updates/package_validation.h"
#include "jadyx/updates/release_key.h"
#include "jadyx/updates/update_storage.h"
#include "jadyx/updates/updater.h"

namespace Jadyx::Updates {
namespace {
using Json = nlohmann::json;
using Storage::Persist;
using Storage::ReadBounded;
std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
bool Active(Phase phase) {
    return phase == Phase::Checking || phase == Phase::Downloading || phase == Phase::Verifying ||
           phase == Phase::Preparing;
}
} // namespace
struct Updater::Impl {
    std::recursive_mutex operation;
    mutable std::mutex mutex;
    Runtime runtime;
    bool package_verified{};
    bool install_when_ready{};
    std::filesystem::file_time_type verified_write{};
    Snapshot state;
    std::jthread worker;
    std::filesystem::path folder, part, journal;
    std::string body, signature, skipped, target;
    std::int64_t dismissed_until{}, last_attempt{};
    explicit Impl(Runtime dependencies) : runtime{std::move(dependencies)} {
        state.installed = runtime.installed;
        folder = runtime.folder;
        std::error_code ec;
        if (folder.empty() || !folder.is_absolute()) {
            state.phase = Phase::Failed;
            state.error = Error::Persistence;
            return;
        }
        std::filesystem::create_directories(folder, ec);
        part = folder / "jadyx-update.appx";
        journal = folder / "state.json";
        if (ec) {
            state.phase = Phase::Failed;
            state.error = Error::Persistence;
            return;
        }
        try {
            auto raw = ReadBounded(journal);
            if (!raw || raw->size() > (1u << 20))
                return;
            auto j = Json::parse(*raw);
            state.channel = j.value("preview", false) ? Channel::Preview : Channel::Stable;
            state.last_check = j.value("last_check", std::int64_t{});
            last_attempt = j.value("last_attempt", std::int64_t{});
            skipped = j.value("skipped", "");
            dismissed_until = j.value("dismissed_until", std::int64_t{});
            target = j.value("target", "");
            state.native_error = j.value("native_error", std::uint32_t{});
            body = j.value("manifest", "");
            signature = j.value("signature", "");
            {
                auto encoded = signature;
                signature.clear();
                if (encoded.size() % 2)
                    throw Error::Persistence;
                for (std::size_t i = 0; i < encoded.size(); i += 2)
                    signature += static_cast<char>(std::stoul(encoded.substr(i, 2), nullptr, 16));
            }
            state.history = j.value("history", std::vector<std::string>{});
            if (state.history.size() > 20)
                state.history.resize(20);
            state.release =
                VerifyManifest(body, signature, runtime.public_key, state.installed, state.channel);
            if (state.release) {
                state.phase = Phase::Paused;
                state.total = state.release->size;
                state.notice = Visible();
                const auto size = std::filesystem::file_size(part, ec);
                state.received = ec ? 0 : std::min(size, state.total);
            }
            if (!target.empty()) {
                const auto expected = ParseVersion(target);
                if (expected && state.installed == *expected) {
                    state.phase = Phase::Confirmed;
                    state.native_error = 0;
                    Event("confirmed " + target);
                    target.clear();
                    Save();
                } else {
                    state.phase = state.release ? Phase::Paused : Phase::Failed;
                    state.error = Error::Deployment;
                    Event("unconfirmed " + target);
                }
            }
        } catch (...) {
            state.phase = Phase::Failed;
            state.error = Error::Persistence;
        }
    }
    bool HasVerifiedFile() const {
        if (!package_verified || !state.release)
            return false;
        std::error_code ec;
        const auto size = std::filesystem::file_size(part, ec);
        if (ec || size != state.release->size)
            return false;
        const auto stamp = std::filesystem::last_write_time(part, ec);
        return !ec && stamp == verified_write;
    }
    bool Visible() const {
        return state.release && skipped != FormatVersion(state.release->version) &&
               Now() >= dismissed_until;
    }
    void Event(const std::string& event) {
        state.history.insert(state.history.begin(), std::to_string(Now()) + " " + event);
        if (state.history.size() > 20)
            state.history.resize(20);
    }
    bool Save() {
        try {
            if (folder.empty() || !folder.is_absolute())
                return false;
            std::string encoded;
            constexpr char hex[] = "0123456789abcdef";
            for (unsigned char c : signature) {
                encoded += hex[c >> 4];
                encoded += hex[c & 15];
            }
            const Json j{{"schema", 1},
                         {"preview", state.channel == Channel::Preview},
                         {"last_check", state.last_check},
                         {"last_attempt", last_attempt},
                         {"skipped", skipped},
                         {"dismissed_until", dismissed_until},
                         {"target", target},
                         {"native_error", state.native_error},
                         {"manifest", body},
                         {"signature", encoded},
                         {"history", state.history}};
            if (!Persist(journal, j.dump())) {
                state.native_error = GetLastError();
                state.error = Error::Persistence;
                state.phase = Phase::Failed;
                ++state.revision;
                return false;
            }
            return true;
        } catch (...) {
            state.error = Error::Persistence;
            state.phase = Phase::Failed;
            ++state.revision;
            return false;
        }
    }
    void PhaseTo(Phase phase, Error error = Error::None) {
        state.phase = phase;
        state.error = error;
        state.native_error = 0;
        ++state.revision;
    }
    template <class Job>
    void Start(Job job) {
        try {
            worker = std::jthread{std::move(job)};
        } catch (...) {
            std::lock_guard lock{mutex};
            PhaseTo(Phase::Failed, Error::Busy);
        }
    }
    void Stop() {
        worker.request_stop();
        if (worker.joinable())
            worker.join();
    }
};
namespace {
Runtime ProductionRuntime() {
    Runtime runtime;
    const auto v = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
    runtime.installed = {v.Major, v.Minor, v.Build, v.Revision};
    runtime.folder =
        std::filesystem::path{
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()} /
        "updates";
    runtime.public_key = ReleasePublicKey;
    runtime.fetch = Http::Fetch;
    runtime.download = Http::DownloadVerified;
    runtime.verify_package = ValidatePackage;
    runtime.deploy = DeployPackage;
    return runtime;
}
} // namespace
Updater::Updater() : Updater{ProductionRuntime()} {}
Updater::Updater(Runtime runtime) : impl{std::make_unique<Impl>(std::move(runtime))} {
    // Every launch asks once: the list answers 304 while nothing changed. A remembered release
    // (Paused) is checked too, so a newer one or its notes show without opening the page.
    if (impl->runtime.automatic_check && impl->target.empty() &&
        (impl->state.phase == Phase::Idle || impl->state.phase == Phase::Paused))
        Check(true);
}
Updater::~Updater() {
    std::lock_guard lock{impl->operation};
    impl->Stop();
}
Phase Updater::CurrentPhase() const {
    std::lock_guard lock{impl->mutex};
    return impl->state.phase;
}
Snapshot Updater::Status() const {
    std::lock_guard lock{impl->mutex};
    return impl->state;
}
void Updater::Check(bool force) {
    std::lock_guard operation_lock{impl->operation};
    auto& p = *impl;
    {
        std::lock_guard lock{p.mutex};
        if (Active(p.state.phase) || p.state.phase == Phase::Requested ||
            (!force && p.last_attempt <= Now() && Now() - p.last_attempt < 21600))
            return;
    }
    p.Stop();
    {
        std::lock_guard lock{p.mutex};
        p.last_attempt = Now();
        if (!p.Save())
            return;
        p.PhaseTo(Phase::Checking);
    }
    p.Start([&p](std::stop_token stop) {
        bool apartment = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartment = true;
            // The list contains preview releases too; never use /latest for the preview channel.
            // Pagination makes selection independent of the order in which releases were published.
            std::optional<Release> best;
            std::string best_body, best_signature, best_notes;
            bool failed = false;
            for (unsigned page = 1; page <= 10 && !stop.stop_requested(); ++page) {
                const auto cache = p.folder / ("releases-" + std::to_string(page) + ".json");
                const auto etag_file = std::filesystem::path{cache.wstring() + L".etag"};
                auto fetched = p.runtime.fetch(
                    "https://api.github.com/repos/JulianDr14/jadyx/releases?per_page=100&page=" +
                        std::to_string(page),
                    ReadBounded(etag_file, 8192).value_or(""), stop, 1u << 20);
                std::string list;
                if (fetched.status == 304)
                    list = ReadBounded(cache).value_or("");
                else if (fetched.status == 200) {
                    list = fetched.body;
                    if (!Persist(cache, list) || !Persist(etag_file, fetched.etag))
                        throw Error::Persistence;
                } else {
                    failed = true;
                    break;
                }
                auto releases = Json::parse(list, nullptr, false);
                if (!releases.is_array())
                    throw Error::Manifest;
                for (const auto& release : releases) {
                    if (stop.stop_requested())
                        break;
                    if (release.value("draft", true) ||
                        (p.state.channel == Channel::Stable && release.value("prerelease", true)))
                        continue;
                    const auto tag = release.value("tag_name", "");
                    const auto version =
                        ParseVersion(tag.starts_with("v") ? std::string_view{tag}.substr(1)
                                                          : std::string_view{});
                    if (!version || *version <= p.state.installed ||
                        (best && *version <= best->version))
                        continue;
                    const auto base =
                        "https://github.com/JulianDr14/jadyx/releases/download/" + tag + "/";
                    bool manifest = false, sig = false;
                    for (const auto& asset : release.at("assets")) {
                        manifest |= asset.value("name", "") == "update.json" &&
                                    asset.value("browser_download_url", "") == base + "update.json";
                        sig |= asset.value("name", "") == "update.sig" &&
                               asset.value("browser_download_url", "") == base + "update.sig";
                    }
                    if (!manifest || !sig)
                        continue; // Releases predating the updater remain available manually.
                    auto m = p.runtime.fetch(base + "update.json", "", stop, 65536);
                    auto s = p.runtime.fetch(base + "update.sig", "", stop, 8192);
                    if (m.status != 200 || s.status != 200)
                        throw Error::Network;
                    auto verified = VerifyManifest(m.body, s.body, p.runtime.public_key,
                                                   p.state.installed, p.state.channel);
                    if (!verified || verified->tag != tag ||
                        (verified->channel == Channel::Preview) !=
                            release.value("prerelease", false))
                        throw Error::Manifest;
                    bool package = false;
                    for (const auto& asset : release.at("assets"))
                        package |= asset.value("browser_download_url", "") == verified->url &&
                                   asset.value("size", std::uint64_t{}) == verified->size;
                    if (!package)
                        throw Error::Manifest;
                    best = verified;
                    best_body = m.body;
                    best_signature = s.body;
                    // Display text only: the signed manifest stays the authority on the package.
                    const auto notes = release.find("body");
                    best_notes = notes != release.end() && notes->is_string()
                                     ? notes->get<std::string>()
                                     : std::string{};
                }
                if (releases.size() < 100)
                    break;
                if (page == 10)
                    failed = true; // Do not claim a complete check of a truncated history.
            }
            std::lock_guard lock{p.mutex};
            if (stop.stop_requested())
                p.PhaseTo(p.state.release ? Phase::Paused : Phase::Idle);
            else if (failed)
                p.PhaseTo(Phase::Failed, Error::Network);
            else {
                const bool same =
                    p.state.release && best && p.state.release->version == best->version &&
                    p.state.release->sha256 == best->sha256 && p.state.release->url == best->url &&
                    p.state.release->size == best->size;
                if (p.state.release && best && p.state.release->version == best->version && !same)
                    throw Error::Manifest;
                const bool ready = same && p.package_verified;
                if (!same) {
                    p.package_verified = false;
                    p.target.clear();
                }
                p.state.release = best;
                if (best && best->notes.empty())
                    p.state.release->notes = ReleaseNotesText(best_notes);
                p.body = best_body;
                p.signature = best_signature;
                p.state.total = best ? best->size : 0;
                p.state.received = ready ? p.state.total : 0;
                p.state.last_check = Now();
                p.state.notice = p.Visible();
                p.PhaseTo(ready ? Phase::Ready : best ? Phase::Available : Phase::Idle);
                p.Event("check completed");
                p.Save();
            }
        } catch (Error error) {
            std::lock_guard lock{p.mutex};
            p.PhaseTo(Phase::Failed, error);
        } catch (...) {
            std::lock_guard lock{p.mutex};
            p.PhaseTo(Phase::Failed, Error::Network);
        }
        if (apartment)
            winrt::uninit_apartment();
    });
}
void Updater::SetChannel(Channel channel) {
    std::lock_guard operation_lock{impl->operation};
    {
        std::lock_guard lock{impl->mutex};
        if (impl->state.phase == Phase::Preparing || impl->state.phase == Phase::Requested)
            return;
    }
    Pause();
    {
        std::lock_guard lock{impl->mutex};
        impl->state.channel = channel;
        impl->skipped.clear();
        impl->dismissed_until = 0;
        impl->state.release.reset();
        impl->package_verified = false;
        impl->target.clear();
        impl->body.clear();
        impl->signature.clear();
        impl->state.notice = false;
        impl->Save();
    }
    Check(true);
}
void Updater::Download(bool install_when_ready) {
    std::lock_guard operation_lock{impl->operation};
    auto& p = *impl;
    {
        std::lock_guard lock{p.mutex};
        if (Active(p.state.phase) || p.state.phase == Phase::Requested || !p.state.release)
            return;
    }
    p.Stop();
    {
        std::lock_guard lock{p.mutex};
        p.package_verified = false;
        p.install_when_ready = install_when_ready;
        p.PhaseTo(Phase::Downloading);
        p.state.bytes_per_second = 0;
        if (!p.Save())
            return;
    }
    p.Start([&p](std::stop_token stop) {
        bool apartment = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartment = true;
            const auto release = *p.state.release;
            auto start = std::chrono::steady_clock::now();
            std::uint64_t initial = 0;
            bool first = true;
            auto error = p.runtime.download(
                release.url, p.part, release.sha256, release.size, stop,
                [&](auto done, auto total) {
                    std::lock_guard lock{p.mutex};
                    if (first) {
                        first = false;
                        initial = done;
                        start = std::chrono::steady_clock::now();
                    }
                    p.state.received = done;
                    p.state.total = total;
                    const auto seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                            .count();
                    p.state.bytes_per_second = seconds > 0 ? (done - initial) / seconds : 0;
                    ++p.state.revision;
                },
                [&] {
                    std::lock_guard lock{p.mutex};
                    p.PhaseTo(Phase::Verifying);
                });
            if (error == Http::DownloadError::None &&
                !p.runtime.verify_package(p.part, release, stop))
                error = Http::DownloadError::Checksum;
            {
                std::lock_guard lock{p.mutex};
                if (error == Http::DownloadError::None) {
                    p.verified_write = std::filesystem::last_write_time(p.part);
                    p.package_verified = true;
                    p.state.received = release.size;
                    p.PhaseTo(Phase::Ready);
                    p.Event("verified " + FormatVersion(release.version));
                } else if (stop.stop_requested())
                    p.PhaseTo(Phase::Paused);
                else
                    p.PhaseTo(Phase::Failed,
                              error == Http::DownloadError::Checksum ? Error::Integrity
                              : error == Http::DownloadError::Write  ? Error::Disk
                                                                     : Error::Network);
                p.Save();
            }
        } catch (...) {
            std::lock_guard lock{p.mutex};
            p.PhaseTo(Phase::Failed, Error::Disk);
        }
        if (apartment)
            winrt::uninit_apartment();
    });
}
void Updater::Pause() {
    std::lock_guard lock{impl->operation};
    {
        std::lock_guard state_lock{impl->mutex};
        if (impl->state.phase == Phase::Preparing || impl->state.phase == Phase::Requested)
            return; // Suspending the old app must not cancel its replacement.
    }
    impl->Stop();
    std::lock_guard state_lock{impl->mutex};
    impl->install_when_ready = false;
}
void Updater::Cancel() {
    std::lock_guard operation_lock{impl->operation};
    {
        std::lock_guard lock{impl->mutex};
        if (impl->state.phase == Phase::Preparing || impl->state.phase == Phase::Requested)
            return;
    }
    Pause();
    std::lock_guard lock{impl->mutex};
    std::error_code ec;
    impl->package_verified = false;
    for (const auto& path : {impl->part, std::filesystem::path{impl->part.wstring() + L".identity"},
                             std::filesystem::path{impl->part.wstring() + L".validator"}}) {
        std::filesystem::remove(path, ec);
        if (ec) {
            impl->PhaseTo(Phase::Failed, Error::Disk);
            return;
        }
    }
    impl->target.clear();
    impl->state.received = 0;
    impl->PhaseTo(impl->state.release ? Phase::Available : Phase::Idle);
    impl->Event("download cancelled");
    impl->Save();
}
void Updater::Dismiss(bool skip) {
    std::lock_guard operation_lock{impl->operation};
    std::lock_guard lock{impl->mutex};
    if (skip && impl->state.release)
        impl->skipped = FormatVersion(impl->state.release->version);
    else
        impl->dismissed_until = Now() + 86400;
    impl->state.notice = false;
    ++impl->state.revision;
    impl->Save();
}
std::filesystem::path Updater::VerifiedPackage() const {
    std::lock_guard lock{impl->mutex};
    return impl->HasVerifiedFile() &&
                   (impl->state.phase == Phase::Ready || impl->state.phase == Phase::Assistance ||
                    impl->state.phase == Phase::Requested)
               ? impl->part
               : std::filesystem::path{};
}
std::optional<UpdatePackage> Updater::PrepareAssistedDownload() {
    std::lock_guard operation_lock{impl->operation};
    std::lock_guard lock{impl->mutex};
    if (!impl->HasVerifiedFile() ||
        (impl->state.phase != Phase::Ready && impl->state.phase != Phase::Assistance))
        return {};
    const auto& release = *impl->state.release;
    const auto version = FormatVersion(release.version);
    if (impl->target != version) {
        impl->target = version;
        impl->Event("assisted installation prepared " + version);
        if (!impl->Save())
            return {};
    }
    return UpdatePackage{impl->part, version, release.sha256, release.size};
}
void Updater::RecordAssistedRequest() {
    PrepareAssistedDownload();
}
void Updater::InstallIfReady(bool storage_idle) {
    std::lock_guard operation_lock{impl->operation};
    {
        std::lock_guard lock{impl->mutex};
        if (!storage_idle || !impl->install_when_ready || impl->state.phase != Phase::Ready)
            return;
        impl->install_when_ready = false;
    }
    Install();
}
void Updater::Install() {
    std::lock_guard operation_lock{impl->operation};
    auto& p = *impl;
    {
        std::lock_guard lock{p.mutex};
        if (p.state.phase != Phase::Ready)
            return;
        if (!p.HasVerifiedFile()) {
            p.package_verified = false;
            p.PhaseTo(Phase::Failed, Error::Integrity);
            return;
        }
        p.target = FormatVersion(p.state.release->version);
        p.Event("installation prepared " + p.target);
        if (!p.Save())
            return;
        p.install_when_ready = false;
        p.PhaseTo(Phase::Preparing);
    }
    p.Stop();
    p.Start([&p](std::stop_token stop) {
        bool apartment = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartment = true;
            // Intent and target were persisted before Windows can terminate this process.
            const auto result = p.runtime.deploy ? p.runtime.deploy(p.part, stop)
                                                 : DeploymentResult{false, 0x80004001u};
            std::lock_guard lock{p.mutex};
            const bool accepted = (result.registered || result.submitted) && result.error == 0;
            p.PhaseTo(accepted ? Phase::Requested : Phase::Assistance,
                      accepted ? Error::None : Error::Deployment);
            p.Event(accepted ? "deployment requested; awaiting new launch"
                             : "deployment failed " + std::to_string(result.error));
            p.state.native_error = accepted ? 0u : result.error ? result.error : 0x80004005u;
            p.Save();
        } catch (const winrt::hresult_error& error) {
            std::lock_guard lock{p.mutex};
            p.PhaseTo(Phase::Assistance, Error::Deployment);
            p.Event("deployment exception; use assistance");
            p.state.native_error = static_cast<std::uint32_t>(error.code().value);
            p.Save();
        } catch (...) {
            std::lock_guard lock{p.mutex};
            p.PhaseTo(Phase::Assistance, Error::Deployment);
            p.Event("deployment unavailable; use assistance");
            p.state.native_error = 0x80004005u;
            p.Save();
        }
        if (apartment)
            winrt::uninit_apartment();
    });
}
} // namespace Jadyx::Updates
