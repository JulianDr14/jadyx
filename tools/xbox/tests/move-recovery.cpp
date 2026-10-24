// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/move_recovery.h"
#include <cassert>
#include <string>
#include <vector>

int main() {
  using namespace Jadyx::VerifiedMove;
  const std::vector<std::string> folders{".jadyx-move", "Jadyx-0123",
                                         ".jadyx-move (2)", "saves",
                                         "prefix.jadyx-move"};
  std::vector<std::string> removed;
  CleanupStaging(
      folders, [&](const auto &name) { removed.push_back(name); },
      [] { return false; });
  assert(
      (removed == std::vector<std::string>{".jadyx-move", ".jadyx-move (2)"}));
  CleanupStaging(
      folders, [&](const auto &) { assert(false); }, [] { return true; });
  const std::vector<File> files{{.relative = "base/a.nro", .bytes = 4},
                                {.relative = "content/update", .bytes = 8}};
  unsigned checked{};
  const auto verify = [&](const File &) {
    ++checked;
    return true;
  };
  assert(CheckPublished(files, verify, [] { return false; }) ==
         ExistingCopy::Verified);
  assert(checked ==
         2); // Every file must match before a published copy is recognized.
  assert(CheckPublished(
             files, [](const File &) { return false; }, [] { return false; }) ==
         ExistingCopy::Conflict);
  assert(CheckPublished(files, verify, [] { return true; }) ==
         ExistingCopy::Cancelled);
  assert(CheckPublished({}, verify, [] { return false; }) ==
         ExistingCopy::Conflict);
  checked = 0;
  assert(CheckPublished(files, verify, [&] { return checked != 0; }) ==
         ExistingCopy::Cancelled);
  // A completed hash can race cancellation; do not claim that verification
  // completed.
  checked = 0;
  assert(CheckPublished({files.front()}, verify, [&] {
           return checked != 0;
         }) == ExistingCopy::Cancelled);
}
