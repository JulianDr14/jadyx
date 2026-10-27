// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <mutex>
#include "video_core/diagnostics/full_capture.h"
#include "video_core/diagnostics/full_capture_archive.h"

namespace VideoCommon::FullCapture {
namespace {
std::atomic_bool enabled{}, active{};
std::atomic_bool self_test{};
std::atomic_bool targets_after_only{};
std::mutex mutex;
Archive archive;
std::string session{"{}"};
}

void Enable(bool value) noexcept { enabled.store(value, std::memory_order_relaxed); }
bool Enabled() noexcept { return enabled.load(std::memory_order_relaxed); }
bool Active() noexcept { return active.load(std::memory_order_relaxed); }
void RequestSelfTest(bool value) noexcept { self_test.store(value); }
bool TakeSelfTest() noexcept { return self_test.exchange(false); }
void SetTargetsAfterOnly(bool value) noexcept { targets_after_only.store(value); }
bool TargetsAfterOnly() noexcept { return targets_after_only.load(std::memory_order_relaxed); }
void SetSession(std::string_view detail) noexcept {
    try { std::scoped_lock lock{mutex}; session = detail; } catch (...) {}
}

bool Begin(const std::filesystem::path& directory) noexcept {
    if (!Enabled()) return false;
    try {
        std::scoped_lock lock{mutex};
        const bool started = archive.Begin(directory);
        if (started) archive.Event("session", session);
        active.store(archive.Active(), std::memory_order_relaxed);
        return started;
    } catch (...) { return false; }
}

void End(bool complete) noexcept {
    try {
        std::scoped_lock lock{mutex};
        archive.End(complete);
        active.store(false, std::memory_order_relaxed);
    } catch (...) { active.store(false, std::memory_order_relaxed); }
}

void Event(std::string_view kind, std::string_view detail) noexcept {
    if (!Active()) return;
    try { std::scoped_lock lock{mutex}; archive.Event(kind, detail); }
    catch (...) { Omit("event_exception"); }
}

void Omit(std::string_view reason, std::string_view detail) noexcept {
    if (!Active()) return;
    try { std::scoped_lock lock{mutex}; archive.Omit(reason, detail); } catch (...) {}
}

void Text(std::string_view kind, std::string_view text) noexcept {
    if (!Active()) return;
    try { Event(kind, "{\"text\":" + Archive::Quote(text) + "}"); }
    catch (...) { Omit("text_exception"); }
}

void Data(std::string_view kind, std::string_view detail, std::span<const std::uint8_t> data,
          std::size_t row_bytes, std::size_t row_pitch, std::size_t rows) noexcept {
    if (!Active()) return;
    try {
        std::scoped_lock lock{mutex};
        archive.Data(kind, detail, data, row_bytes, row_pitch, rows);
    } catch (...) { Omit("data_exception", detail); }
}
} // namespace VideoCommon::FullCapture
