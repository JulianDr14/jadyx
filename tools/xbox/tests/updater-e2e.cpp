// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/logging.h"
#undef LOG_WARNING
#define LOG_WARNING(...) ((void)0)
#include "jadyx/network/http.cpp"
#include "jadyx/updates/updater.cpp"
#include <cassert>
#include <iostream>

using namespace Jadyx::Updates;

int main(int argc, char **argv) {
  assert(argc == 3);
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  const std::string base = argv[1];
  Runtime runtime;
  runtime.installed = {0, 3, 4, 0};
  runtime.folder = std::filesystem::absolute(argv[2]);
  runtime.public_key = ReleasePublicKey;
  runtime.automatic_check = false;
  const auto local = [&](const std::string &url) {
    return base + (url.starts_with("https://api.github.com/")
                       ? "/releases"
                       : "/" + url.substr(url.find_last_of('/') + 1));
  };
  runtime.fetch = [&](const auto &url, const auto &etag, std::stop_token stop,
                      std::size_t max) {
    return Jadyx::Http::Fetch(local(url), etag, stop, max);
  };
  runtime.download = [&](const auto &url, const auto &path, const auto &hash,
                         auto size, std::stop_token stop, const auto &progress,
                         const auto &verify) {
    return Jadyx::Http::DownloadVerified(local(url), path, hash, size, stop,
                                         progress, verify);
  };
  runtime.verify_package = ValidatePackage;
  unsigned deployments{};
  runtime.deploy = [&](const auto& path, std::stop_token) {
    assert(path.extension() == L".appx");
    ++deployments;
    return DeploymentResult{true, 0}; // Simulated OS response; never deploy the user's app.
  };
  const auto settle = [](Updater &updater) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (Active(updater.CurrentPhase())) {
      assert(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  };
  {
    Updater updater{runtime};
    updater.Check(true);
    settle(updater);
    assert(updater.CurrentPhase() == Phase::Available);
    assert(updater.Status().release->version == Version({0, 3, 6, 0}));
    updater.Download();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (updater.Status().received < (1u << 20)) {
      assert(Active(updater.CurrentPhase()));
      assert(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    updater.Pause();
    assert(updater.CurrentPhase() == Phase::Paused &&
           updater.Status().received > 0);
    const auto start = std::chrono::steady_clock::now();
    updater.Download();
    settle(updater);
    assert(updater.CurrentPhase() == Phase::Ready &&
           !updater.VerifiedPackage().empty());
    const auto download = updater.PrepareAssistedDownload();
    assert(download && download->version == "0.3.6.0");
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    std::cout << "real signed APPX, pause/resume/hash/XML: " << download->size
              << " bytes, " << elapsed << " s (local fixture)\n";
    const auto cpu_start = std::chrono::steady_clock::now();
    std::uint64_t observed{};
    for (unsigned i = 0; i < 10000; ++i)
      observed += updater.Status().received;
    const auto us = std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - cpu_start)
                        .count() /
                    10000;
    assert(observed > 0);
    std::cout << "Status snapshot mean: " << us << " us; UI polls at 2 Hz\n";
    updater.Install();
    settle(updater);
    assert(updater.CurrentPhase() == Phase::Requested && deployments == 1);
  }
  {
    Updater updater{runtime};
    assert(updater.CurrentPhase() == Phase::Paused &&
           updater.VerifiedPackage().empty());
    updater.Download();
    settle(updater);
    assert(updater.CurrentPhase() == Phase::Ready);
    updater.Install();
    settle(updater);
  }
  runtime.installed = {0, 3, 6, 0};
  {
    Updater updater{runtime};
    assert(updater.CurrentPhase() == Phase::Confirmed);
  }
  std::cout << "end-to-end coordinator/real WinRT HTTP/pinned RSA/real signed "
               "APPX/restart/"
               "confirmation: PASS\n";
  winrt::uninit_apartment();
}
