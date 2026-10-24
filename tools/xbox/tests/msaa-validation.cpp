// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_msaa_validation.h"
#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
  using D3D12::ValidateMsaaAllocation;
  using D3D12::ValidateMsaaShape;
  // The callback stands in for the typed-format, exact-count driver query.
  unsigned queries{};
  std::uint32_t requested{};
  bool query_ok = true;
  std::uint32_t levels = 1;
  const auto query = [&](std::uint32_t samples) {
    ++queries;
    requested = samples;
    return query_ok && levels != 0;
  };
  unsigned cases{};
  for (std::uint32_t samples = 0; samples <= 64; ++samples) {
    const bool valid = samples == 1 || samples == 2 || samples == 4 ||
                       samples == 8 || samples == 16;
    const auto reason = ValidateMsaaShape(samples, true, 1, 1, 64, 64, 1, 1);
    assert(reason.empty() == valid);
    ++cases;
  }
  for (const std::uint32_t samples : {2U, 4U, 8U, 16U}) {
    assert(!ValidateMsaaShape(samples, false, 1, 1, 64, 64, 1, 1).empty());
    assert(!ValidateMsaaShape(samples, true, 2, 1, 64, 64, 1, 1).empty());
    assert(!ValidateMsaaShape(samples, true, 1, 2, 64, 64, 1, 1).empty());
    assert(!ValidateMsaaShape(samples, true, 1, 1, 64, 64, 4, 4).empty());
    assert(!ValidateMsaaShape(samples, true, 1, 1, 63, 64, 1, 1).empty());
    assert(!ValidateMsaaShape(samples, true, 1, 1, 0, 64, 1, 1).empty());
    if (samples >= 4) {
      assert(!ValidateMsaaShape(samples, true, 1, 1, 64, 63, 1, 1).empty());
      ++cases;
    }
    for (const bool success : {false, true}) {
      for (const std::uint32_t quality_levels : {0U, 1U, 4U}) {
        query_ok = success;
        levels = quality_levels;
        const auto before = queries;
        const auto result = ValidateMsaaAllocation(samples, true, query);
        assert(result.empty() == (success && quality_levels != 0));
        assert(queries == before + 1 && requested == samples);
        ++cases;
      }
    }
    const auto before = queries;
    assert(!ValidateMsaaAllocation(samples, false, query).empty());
    assert(queries == before);
    cases += 7;
  }
  // Single-sample BC/mips retain their existing path; no MSAA capability query.
  assert(ValidateMsaaShape(1, false, 8, 8, 7, 7, 4, 4).empty());
  const auto before = queries;
  assert(ValidateMsaaAllocation(1, false, query).empty());
  assert(queries == before);
  cases += 2;
  std::cout << "PASS " << cases << " MSAA shape and exact capability cases\n";
}
