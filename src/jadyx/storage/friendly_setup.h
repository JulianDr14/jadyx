// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <stdexcept>
#include "jadyx/i18n/localization.h"

namespace Jadyx {
using FirmwareVersion = std::array<unsigned, 5>;
inline bool FirmwareOlder(const FirmwareVersion &candidate, const FirmwareVersion &installed) {
    return candidate < installed;
}
inline bool KeysOlderThanFirmware(int available, int required) {
    return available >= 0 && required >= 0 && available < required;
}
inline bool NeedsGameMode(std::uint64_t limit) { return limit != 0 && limit < (5120ULL << 20); }
inline bool ShowSetupChecklist(bool keys_ready, unsigned firmware_files, bool requested = false) {
    return requested || !keys_ready || firmware_files == 0;
}
enum class UploadRoute { Base, Update, Dlc, Unsupported };
// CNMT types, never filenames: Application=0x80, Patch=0x81, AddOnContent=0x82.
inline UploadRoute GeneralUploadRoute(unsigned type, std::uint64_t title) {
    if (!title)
        return UploadRoute::Unsupported;
    if (type == 0x80 && (title & 0x1fff) == 0)
        return UploadRoute::Base;
    if (type == 0x81 && (title & 0x1fff) == 0x800)
        return UploadRoute::Update;
    if (type == 0x82 && (title & 0x1fff) >= 0x1000 && (title & 0x1fff) <= 0x17ff)
        return UploadRoute::Dlc;
    return UploadRoute::Unsupported;
}
inline bool IsDiskFull(std::uint32_t code) {
    return code == 112 || code == 39 || code == 0x80070070U || code == 0x80070027U;
}
inline std::wstring StorageGb(std::uint64_t bytes) {
    // Decimal GB; integer arithmetic keeps the separator independent of the process locale.
    const auto hundredths = bytes / 10000000;
    return std::to_wstring(hundredths / 100) + L"." + (hundredths % 100 < 10 ? L"0" : L"") +
           std::to_wstring(hundredths % 100);
}
inline std::wstring StorageFullMessage(std::uint64_t free, std::uint64_t needed) {
    return I18n::Format(I18n::Text::StorageFull, {StorageGb(free), StorageGb(needed)});
}
class StorageSpaceError : public std::runtime_error {
  public:
    StorageSpaceError(std::uint64_t free_bytes, std::uint64_t needed_bytes)
        : std::runtime_error{"space"}, free{free_bytes}, needed{needed_bytes} {}
    std::uint64_t free{}, needed{};
};
} // namespace Jadyx
