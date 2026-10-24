// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/compatibility.h"
#include <cassert>
#include <iostream>
#include <nlohmann/json.hpp>

int main() {
  using namespace Jadyx::Compatibility;
  auto old = nlohmann::json::parse(
      R"({"schema_version":1,"titles":{"0100000000000000":[{"title_id":"0100000000000000","name":"Fixture","status":"issues","console":"S","fps_note":"30","note":"","product":"Jadyx"}]}})");
  assert(
      Parse(old.dump())); // All new report fields and generated are optional.
  auto cached = old;
  cached["generated"] = "2026-10-24T12:00:00Z";
  cached["titles"]["0100000000000000"][0]["status"] = "playable";
  auto bundled = old;
  bundled["generated"] = "2026-10-24T13:00:00.001Z";
  bundled["titles"]["0100000000000000"][0]["status"] = "broken";
  const auto status = [](const Table &table) {
    return Lookup(table, 0x0100000000000000).status;
  };
  assert(status(Select(old.dump(), cached.dump(), bundled.dump())) ==
         Status::Broken);
  assert(status(Select(bundled.dump(), cached.dump(), old.dump())) ==
         Status::Broken);
  assert(status(Select(old.dump(), bundled.dump(), cached.dump())) ==
         Status::Broken);
  assert(status(Select("<html>bad gateway</html>", cached.dump(),
                       old.dump())) == Status::Playable);
  assert(status(Select("{", old.dump(), cached.dump())) == Status::Playable);
  for (const auto *invalid :
       {"tomorrow", "2026-02-30T00:00:00Z", "2026-10-24T24:00:00Z"}) {
    auto bad = bundled;
    bad["generated"] = invalid;
    assert(!Parse(bad.dump()));
    assert(status(Select(bad.dump(), cached.dump(), old.dump())) ==
           Status::Playable);
  }
  auto metadata = old;
  auto &row = metadata["titles"]["0100000000000000"][0];
  row["jadyx_version"] = "0.3.8.5";
  row["report_url"] = "https://discord.com/channels/1/2/3";
  row["date"] = "2026-10-24";
  row["source"] = "community";
  assert(Parse(metadata.dump()));
  row["report_url"] = 7;
  assert(!Parse(metadata.dump()));
  std::cout << "PASS optional metadata, newest-wins permutations, invalid "
               "download fallback\n";
}
