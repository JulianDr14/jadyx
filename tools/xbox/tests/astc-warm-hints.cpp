// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_astc_warm.h"
#include <cassert>
#include <sstream>
#include <string>
struct Key {
  std::uint64_t shader, state;
  bool operator==(const Key &) const = default;
};
int main() {
  using Record = D3D12::AstcWarmRecord<Key>;
  std::vector<Record> records{{{1, 2}, {0, 7}}, {{1, 3}, {2}}, {{4, 5}, {127}}};
  std::ostringstream out(std::ios::binary);
  assert(D3D12::WriteAstcWarm<Key>(out, records));
  const auto bytes = out.str();
  auto read = [](const std::string &b) {
    std::istringstream in(b, std::ios::binary);
    return D3D12::ReadAstcWarm<Key>(in);
  };
  auto restored = read(bytes);
  assert(restored.size() == 3);
  for (size_t i = 0; i < records.size(); ++i) {
    assert(restored[i].key == records[i].key);
    assert(restored[i].descriptors == records[i].descriptors);
  }
  for (size_t n = 0; n < bytes.size(); ++n)
    assert(read(bytes.substr(0, n)).empty());
  assert(read(bytes + "x").empty());
  auto bad = bytes;
  bad[0] ^= 1;
  assert(read(bad).empty());
  bad = bytes;
  bad[4] ^= 1;
  assert(read(bad).empty());
  bad = bytes;
  bad[8] = 1;
  bad[9] = 1;
  assert(read(bad).empty());
  bad = bytes;
  bad[28] = char(129);
  assert(read(bad).empty());
  bad = bytes;
  bad[36] = 0;
  assert(read(bad).empty());
  bad = bytes;
  bad[32] = char(128);
  assert(read(bad).empty());
  std::vector<Record> huge(257, records[0]);
  std::ostringstream invalid;
  assert(!D3D12::WriteAstcWarm<Key>(invalid, huge));
  std::vector<Record> duplicate{{{1, 2}, {7, 7}}};
  assert(!D3D12::WriteAstcWarm<Key>(invalid, duplicate));
  std::vector<Record> reversed{{{1, 2}, {7, 0}}};
  assert(!D3D12::WriteAstcWarm<Key>(invalid, reversed));
  std::puts("ASTC variant hint gate PASS: round-trip, distinct state, "
            "truncation, corruption, bounds, duplicate/order");
}