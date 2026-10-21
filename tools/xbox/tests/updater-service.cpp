// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/updates/updater.cpp"
#include "updater-fixture.h"
#include <atomic>
#include <future>
#include <iostream>
// These defaults are never used by injected fixtures. Real HTTP is covered
// separately.
namespace Jadyx::Http {
Response Fetch(const std::string &, const std::string &, std::stop_token,
               std::size_t) {
  assert(false);
  return {};
}
DownloadError DownloadVerified(const std::string &,
                               const std::filesystem::path &,
                               const std::string &, std::uint64_t,
                               std::stop_token, const DownloadProgress &,
                               const std::function<void()> &) {
  assert(false);
  return DownloadError::Network;
}
} // namespace Jadyx::Http
using namespace Jadyx::Updates;
using Json = nlohmann::json;
static UpdateSigner signer;
struct Fixture {
  std::filesystem::path folder;
  Json manifest;
  std::string body, sig;
  std::atomic<unsigned> fetches{}, downloads{};
  std::atomic<bool> offline{}, not_modified{}, hold{}, entered{}, bad_package{};
  unsigned case_id{};
  Fixture(const std::filesystem::path &base, unsigned id)
      : folder{base / std::to_string(id)}, case_id{id} {
    SetRelease("0.3.6.0");
  }
  void SetRelease(const std::string &v, bool preview = false) {
    manifest = {
        {"schema", 1},
        {"version", v},
        {"tag", "v" + v},
        {"channel", preview ? "preview" : "stable"},
        {"identity",
         {{"name", "Jadyx"},
          {"publisher", "CN=JadyxDev"},
          {"architecture", "x64"}}},
        {"diagnostic", false},
        {"min_os", "10.0.17763.0"},
        {"vclibs_min", "14.0.33519.0"},
        {"url", "https://github.com/JulianDr14/jadyx/releases/download/v" + v +
                    "/Jadyx_" + v + "_x64.appx"},
        {"size", 8},
        {"sha256", std::string(64, 'a')},
        {"notes", "test"},
        {"published", "2026-10-19"}};
    body = manifest.dump();
    sig = signer.Sign(body);
  }
  Runtime Env(Version installed = {0, 3, 4, 0}) {
    Runtime env;
    env.installed = installed;
    env.folder = folder;
    env.public_key = signer.public_key;
    env.automatic_check = false;
    env.fetch = [this](const std::string &url, const std::string &,
                       std::stop_token stop, std::size_t) {
      ++fetches;
      if (offline || stop.stop_requested())
        return Jadyx::Http::Response{};
      if (url.find("api.github.com") != url.npos) {
        if (not_modified)
          return Jadyx::Http::Response{304};
        const std::string base =
            "https://github.com/JulianDr14/jadyx/releases/download/" +
            manifest.at("tag").get<std::string>() + "/";
        Json assets =
            Json::array({{{"name", "update.json"},
                          {"browser_download_url", base + "update.json"}},
                         {{"name", "update.sig"},
                          {"browser_download_url", base + "update.sig"}},
                         {{"name", "package"},
                          {"browser_download_url", manifest.at("url")},
                          {"size", 8}}});
        Json releases =
            Json::array({{{"draft", false},
                          {"prerelease", manifest.at("channel") == "preview"},
                          {"tag_name", manifest.at("tag")},
                          {"assets", assets}}});
        return Jadyx::Http::Response{200, releases.dump(), "etag"};
      }
      return Jadyx::Http::Response{200,
                                   url.ends_with("update.json") ? body : sig};
    };
    env.download = [this](const auto &, const auto &path, const auto &, auto,
                          std::stop_token stop, const auto &progress,
                          const auto &verify) {
      ++downloads;
      entered = true;
      while (hold && !stop.stop_requested())
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      if (stop.stop_requested())
        return Jadyx::Http::DownloadError::Cancelled;
      {
        std::ofstream out{path, std::ios::binary};
        out << "verified";
      }
      progress(8, 8);
      verify();
      return Jadyx::Http::DownloadError::None;
    };
    env.verify_package = [this](const auto &, const auto &, std::stop_token) {
      return !bad_package;
    };
    return env;
  }
  Json Journal() {
    return Json::parse(Jadyx::ReadWholeFile(folder / "state.json").value());
  }
};
void Settle(Updater &updater) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (Active(updater.CurrentPhase())) {
    assert(std::chrono::steady_clock::now() < until);
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}
void Ready(Updater &updater) {
  updater.Check(true);
  Settle(updater);
  assert(updater.CurrentPhase() == Phase::Available);
  updater.Download();
  Settle(updater);
  if (updater.CurrentPhase() != Phase::Ready)
    std::cerr << "ready failed phase="
              << static_cast<int>(updater.CurrentPhase())
              << " error=" << static_cast<int>(updater.Status().error) << "\n";
  assert(updater.CurrentPhase() == Phase::Ready);
  assert(!updater.VerifiedPackage().empty());
}
int main() {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  const auto root =
      std::filesystem::absolute("build-uwp/test-results/updater/service-" +
                                std::to_string(GetTickCount64()));
  std::filesystem::create_directories(root);
  unsigned cases = 0;
  {
    Fixture f{root, ++cases};
    std::cout << "service case " << cases << std::endl;
    {
      Updater u{f.Env()};
      Ready(u);
      u.Check(true);
      Settle(u);
      assert(u.CurrentPhase() == Phase::Ready && u.Status().received == 8);
      u.Install();
      Settle(u);
      assert(u.CurrentPhase() == Phase::Assistance);
      assert(f.Journal().at("target") == "0.3.6.0");
    }
    {
      Updater u{f.Env()};
      assert(u.CurrentPhase() == Phase::Paused && u.VerifiedPackage().empty());
      u.Download();
      Settle(u);
      assert(u.CurrentPhase() == Phase::Ready);
      u.Install();
      Settle(u);
    }
    {
      Updater u{f.Env({0, 3, 6, 0})};
      assert(u.CurrentPhase() == Phase::Confirmed);
      assert(f.Journal().at("target") == "");
    }
    {
      Updater u{f.Env({0, 3, 6, 0})};
      assert(u.CurrentPhase() == Phase::Idle);
    }
  }
  {
    Fixture f{root, ++cases};
    std::cout << "service case " << cases << std::endl;
    {
      Updater u{f.Env()};
      Ready(u);
      u.Install();
      Settle(u);
      u.Cancel();
      assert(u.CurrentPhase() == Phase::Available);
      assert(f.Journal().at("target") == "");
      assert(!std::filesystem::exists(f.folder / "jadyx-update.appx"));
    }
    {
      Updater u{f.Env()};
      assert(u.CurrentPhase() == Phase::Paused);
      assert(u.Status().error == Error::None);
    }
  }
  {
    Fixture f{root, ++cases};
    std::cout << "service case " << cases << std::endl;
    {
      Updater u{f.Env()};
      u.Check(true);
      Settle(u);
      u.Dismiss(true);
      assert(!u.Status().notice);
    }
    {
      Updater u{f.Env()};
      assert(!u.Status().notice);
      u.Check(true);
      Settle(u);
      assert(!u.Status().notice);
      f.SetRelease("0.3.7.0");
      u.Check(true);
      Settle(u);
      assert(u.Status().notice);
    }
  }
  {
    Fixture f{root, ++cases};
    std::cout << "service case " << cases << std::endl;
    Updater u{f.Env()};
    Ready(u);
    f.offline = true;
    u.Check(true);
    Settle(u);
    std::cerr << "offline phase=" << static_cast<int>(u.CurrentPhase())
              << " error=" << static_cast<int>(u.Status().error)
              << " native=" << u.Status().native_error << "\n";
    assert(u.CurrentPhase() == Phase::Failed &&
           u.Status().error == Error::Network);
    const auto count = f.fetches.load();
    u.Check();
    assert(f.fetches == count);
    f.offline = false;
    u.Check(true);
    Settle(u);
    std::cerr << "after offline " << static_cast<int>(u.CurrentPhase()) << " "
              << static_cast<int>(u.Status().error)
              << " win=" << u.Status().native_error << "\n";
    assert(u.CurrentPhase() == Phase::Ready);
    f.not_modified = true;
    u.Check(true);
    Settle(u);
    std::cerr << "after 304 " << static_cast<int>(u.CurrentPhase()) << " "
              << static_cast<int>(u.Status().error)
              << " win=" << u.Status().native_error << "\n";
    assert(u.CurrentPhase() == Phase::Ready);
  }
  {
    Fixture f{root, ++cases};
    std::cout << "service case " << cases << std::endl;
    Updater u{f.Env()};
    Ready(u);
    u.Install();
    Settle(u);
    u.SetChannel(Channel::Preview);
    Settle(u);
    assert(u.Status().channel == Channel::Preview);
    assert(f.Journal().at("target") == "");
    assert(u.VerifiedPackage().empty());
  }
  {
    Fixture f{root, ++cases};
    std::cout << "service case " << cases << std::endl;
    Updater u{f.Env()};
    u.Check(true);
    Settle(u);
    f.hold = true;
    u.Download();
    while (!f.entered)
      std::this_thread::yield();
    auto pause = std::async(std::launch::async, [&] { u.Pause(); });
    auto cancel = std::async(std::launch::async, [&] { u.Cancel(); });
    pause.get();
    cancel.get();
    assert(u.CurrentPhase() == Phase::Available);
    assert(u.VerifiedPackage().empty());
  }
  {
    Fixture f{root, ++cases};
    std::cout << "service case " << cases << std::endl;
    std::filesystem::create_directories(f.folder / "state.json");
    Updater u{f.Env()};
    u.Check(true);
    assert(u.CurrentPhase() == Phase::Failed &&
           u.Status().error == Error::Persistence);
    assert(f.fetches == 0);
  }
  {
    Fixture f{root, ++cases};
    std::cout << "service case " << cases << std::endl;
    f.sig[0] ^= 1;
    Updater u{f.Env()};
    u.Check(true);
    Settle(u);
    assert(u.CurrentPhase() == Phase::Failed &&
           u.Status().error == Error::Manifest);
    assert(u.VerifiedPackage().empty());
  }
  {
    Fixture f{root, ++cases};
    std::cout << "service case " << cases << std::endl;
    Updater u{f.Env()};
    Ready(u);
    auto file = u.VerifiedPackage();
    HANDLE pin = CreateFile2FromAppW(file.c_str(), GENERIC_READ,
                                     FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    assert(pin != INVALID_HANDLE_VALUE);
    u.Cancel();
    assert(u.Status().error == Error::Disk && u.VerifiedPackage().empty());
    CloseHandle(pin);
    u.Cancel();
    assert(u.CurrentPhase() == Phase::Available);
  }
  {
    Fixture f{root, ++cases};
    std::cout << "service case " << cases << std::endl;
    Updater u{f.Env()};
    Ready(u);
    {
      std::ofstream out{u.VerifiedPackage()};
      out << "tampered-longer";
    }
    assert(u.VerifiedPackage().empty());
    u.Install();
    Settle(u);
    assert(u.CurrentPhase() == Phase::Failed &&
           u.Status().error == Error::Integrity);
  }
  {
    Fixture f{root, ++cases};
    std::cout << "service case " << cases << std::endl;
    Updater u{f.Env()};
    u.Check(true);
    Settle(u);
    f.bad_package = true;
    u.Download();
    Settle(u);
    assert(u.Status().error == Error::Integrity && u.VerifiedPackage().empty());
  }
  {
    Fixture f{root, ++cases};
    std::cout << "service case " << cases << std::endl;
    Updater u{f.Env()};
    Ready(u);
    f.manifest["sha256"] = std::string(64, 'b');
    f.body = f.manifest.dump();
    f.sig = signer.Sign(f.body);
    u.Check(true);
    Settle(u);
    assert(u.CurrentPhase() == Phase::Failed &&
           u.Status().error == Error::Manifest);
  }
  {
    Fixture f{root, ++cases};
    Updater u{f.Env()};
    Ready(u);
    const auto ticket = u.PrepareAssistedDownload();
    assert(ticket && ticket->version == "0.3.6.0" && ticket->size == 8);
    assert(f.Journal().at("target") == "0.3.6.0");
    u.Cancel();
    assert(!u.PrepareAssistedDownload());
  }
  {
    Release release;
    release.version = {0, 3, 6, 0};
    for (const auto &item : std::filesystem::directory_iterator(
             "build-uwp/test-results/updater/packages")) {
      const bool expected = item.path().stem().string().starts_with("valid");
      assert(ValidatePackage(item.path(), release) == expected);
    }
  }
  // The same native-deployment boundary runs in production; fixtures never
  // install on this PC.
  for (const auto result :
       {DeploymentResult{true, 0}, DeploymentResult{false, 0},
        DeploymentResult{false, 0x80070005u},
        DeploymentResult{true, 0x80004005u}, DeploymentResult{false, 0, true}}) {
    Fixture f{root, ++cases};
    auto env = f.Env();
    std::atomic<unsigned> deployments{};
    env.deploy = [&](const auto &path, std::stop_token) {
      assert(f.Journal().at("target") == "0.3.6.0");
      assert(std::filesystem::file_size(path) == 8);
      ++deployments;
      return result;
    };
    {
      Updater u{env};
      u.Check(true);
      Settle(u);
      u.Download(true);
      Settle(u);
      assert(deployments == 0 && u.CurrentPhase() == Phase::Ready);
      u.InstallIfReady(false);
      assert(deployments == 0 && f.Journal().at("target") == "");
      u.InstallIfReady(true);
      Settle(u);
      const bool accepted = (result.registered || result.submitted) && !result.error;
      assert(u.CurrentPhase() ==
             (accepted ? Phase::Requested : Phase::Assistance));
      assert(u.Status().error == (accepted ? Error::None : Error::Deployment));
      assert(u.Status().native_error == (accepted ? 0u : result.error ? result.error : 0x80004005u));
      assert(f.Journal().at("native_error") == u.Status().native_error);
      u.InstallIfReady(true);
      Settle(u);
      assert(deployments == 1); // Never repeat deployment automatically.
      if (accepted) {
        u.Pause(); u.Cancel(); u.SetChannel(Channel::Preview);
        assert(u.CurrentPhase() == Phase::Requested);
        assert(u.Status().channel == Channel::Stable);
        assert(f.Journal().at("target") == "0.3.6.0");
        assert(std::filesystem::exists(f.folder / "jadyx-update.appx"));
      }
    }
    {
      Updater old{env};
      assert(old.CurrentPhase() ==
             Phase::Paused); // Old version is not a success.
      old.InstallIfReady(true);
      assert(deployments == 1);
    }
    env.installed = {0, 3, 6, 0};
    Updater updated{env};
    assert(updated.CurrentPhase() == Phase::Confirmed);
  }
  {
    Fixture f{root, ++cases};
    auto env = f.Env();
    unsigned deployments{};
    env.deploy = [&](const auto &, std::stop_token) {
      ++deployments;
      return DeploymentResult{true, 0};
    };
    Updater u{env};
    u.Check(true);
    Settle(u);
    u.Download(true);
    Settle(u);
    u.Pause();
    u.InstallIfReady(true);
    assert(deployments ==
           0); // Leaving/cancelling the flow revokes auto-install intent.
  }
  std::cout << "updater service: " << cases
            << " state/restart/storage/concurrency scenarios passed\n";
  winrt::uninit_apartment();
}
