// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "jadyx/network/http.h"
#include "jadyx/updates/package_deployment.h"
#include "jadyx/updates/update_package.h"
#include "jadyx/updates/update_policy.h"
namespace Jadyx::Updates {
enum class Phase {
    Idle,
    Checking,
    Available,
    Downloading,
    Paused,
    Verifying,
    Ready,
    Preparing,
    Requested,
    Confirmed,
    Assistance,
    Failed
};
enum class Error { None, Network, Manifest, Disk, Integrity, Deployment, Persistence, Busy };
struct Snapshot {
    Phase phase{};
    Error error{};
    Channel channel{};
    Version installed{};
    std::optional<Release> release;
    std::uint64_t received{}, total{}, revision{};
    std::uint32_t native_error{};
    double bytes_per_second{};
    std::int64_t last_check{};
    bool notice{};
    std::vector<std::string> history;
};
// Explicit dependencies let the same coordinator run against deterministic fixtures. The app
// constructs ProductionRuntime only: no settings or network response can replace its trust key.
struct Runtime {
    Version installed{};
    std::filesystem::path folder;
    std::string public_key;
    std::function<Http::Response(const std::string&, const std::string&, std::stop_token,
                                 std::size_t)>
        fetch;
    std::function<Http::DownloadError(const std::string&, const std::filesystem::path&,
                                      const std::string&, std::uint64_t, std::stop_token,
                                      const Http::DownloadProgress&, const std::function<void()>&)>
        download;
    std::function<bool(const std::filesystem::path&, const Release&, std::stop_token)>
        verify_package;
    bool automatic_check{true};
    std::function<DeploymentResult(const std::filesystem::path&, std::stop_token)> deploy;
};
class Updater {
public:
    Updater();
    explicit Updater(Runtime runtime);
    ~Updater();
    Updater(const Updater&) = delete;
    Snapshot Status() const;
    Phase CurrentPhase() const;
    void Check(bool force = false);
    void SetChannel(Channel channel);
    void Download(bool install_when_ready = false);
    // UI grants storage admission again after verification; never deploy from a download worker.
    void InstallIfReady(bool storage_idle);
    void Pause();
    void Cancel();
    void Dismiss(bool skip);
    // Coordinator must hold the portal's local-operation admission before invoking Install.
    void Install();
    std::filesystem::path VerifiedPackage() const;
    void RecordAssistedRequest();
    std::optional<UpdatePackage> PrepareAssistedDownload();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Jadyx::Updates
