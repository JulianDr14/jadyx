// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the production WinRT transport; silence logging only for this
// standalone harness.
#include "common/logging.h"
#undef LOG_WARNING
#define LOG_WARNING(...) ((void)0)
#include "jadyx/network/http.cpp"
#include <cassert>
#include <iostream>
int main(int argc, char **argv) {
  assert(argc == 7);
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  const std::string base = argv[1], sha = argv[3], mode = argv[4];
  const std::filesystem::path part = argv[2];
  const auto size = std::stoull(argv[5]);
  std::stop_source cancel;
  bool verified = false;
  const auto progress = [&](auto done, auto) {
    if (mode == "pause" && done >= 262144)
      cancel.request_stop();
  };
  if (mode == "fetch") {
    assert(Jadyx::Http::Fetch(base + "/valid", "", {}, 65536).status == 0);
    assert(Jadyx::Http::Fetch(base + "/cache", "test", {}, 65536).status ==
           304);
  } else {
    const auto endpoint = mode == "pause" ? "valid" : mode;
    const auto result = Jadyx::Http::DownloadVerified(
        base + "/" + endpoint, part, sha, size, cancel.get_token(), progress,
        [&] { verified = true; });
    using E = Jadyx::Http::DownloadError;
    if (std::string{argv[6]} == "cut")
      assert(result == E::Network && std::filesystem::file_size(part) > 0);
    else if (mode == "pause")
      assert(result == E::Cancelled && std::filesystem::file_size(part) > 0);
    else if (mode == "wronghash" || mode == "oversized")
      assert(result == E::Checksum);
    else if (mode == "cycle" || mode == "foreign")
      assert(result == E::Network);
    else if (mode == "badrange") {
      std::cerr << "bad range rejection=" << static_cast<int>(result) << "\n";
      assert(result == E::Network || result == E::Checksum);
    } else {
      assert(result == E::None && verified &&
             std::filesystem::file_size(part) == size);
    }
  }
  std::cout << "updater HTTP " << mode << ": OK\n";
  winrt::uninit_apartment();
}
