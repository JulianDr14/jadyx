// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace VideoCommon::FullCapture {

// Opt-in, synchronous forensic capture. Never enabled by a release package.
void Enable(bool enabled) noexcept;
bool Enabled() noexcept;
bool Active() noexcept;
void SetSession(std::string_view detail) noexcept;
void RequestSelfTest(bool value) noexcept;
// Skips the before-operation render target readback; the previous operation's after copy
// usually holds the same bytes, and the targets are most of a capture's cost.
void SetTargetsAfterOnly(bool value) noexcept;
bool TargetsAfterOnly() noexcept;
bool TakeSelfTest() noexcept;
bool Begin(const std::filesystem::path& directory) noexcept;
void End(bool intervals_complete) noexcept;
// detail is a JSON object assembled by the caller (not untrusted free text).
void Event(std::string_view kind, std::string_view detail = "{}") noexcept;
void Text(std::string_view kind, std::string_view text) noexcept;
void Omit(std::string_view reason, std::string_view detail = "{}") noexcept;
// Exports all meaningful bytes, skipping GPU pitch padding; never exports a prefix.
void Data(std::string_view kind, std::string_view detail, std::span<const std::uint8_t> data,
          std::size_t row_bytes = 0, std::size_t row_pitch = 0,
          std::size_t rows = 1) noexcept;

} // namespace VideoCommon::FullCapture
