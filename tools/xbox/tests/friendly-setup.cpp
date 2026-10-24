// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#undef NDEBUG
#include <cassert>
#include "jadyx/storage/friendly_setup.h"
#include "jadyx/storage/system_import.h"

int main() {
    using namespace Jadyx;
    assert(FirmwareOlder({9, 0, 0, 0, 0}, {22, 5, 0, 0, 0}));
    assert(FirmwareOlder({22, 5, 0, 1, 9}, {22, 5, 0, 2, 0}));
    assert(!FirmwareOlder({22, 5, 0, 0, 0}, {22, 5, 0, 0, 0}));
    assert(!FirmwareOlder({22, 10, 0, 0, 0}, {22, 5, 0, 0, 0}));
    assert(KeysOlderThanFirmware(18, 19));
    assert(!KeysOlderThanFirmware(-1, 19)); // Unknown is not proof of old keys.
    assert(!KeysOlderThanFirmware(19, 19));
    assert(GeneralUploadRoute(0x80, 0x0100000000010000ULL) == UploadRoute::Base);
    assert(GeneralUploadRoute(0x81, 0x0100000000010800ULL) == UploadRoute::Update);
    assert(GeneralUploadRoute(0x82, 0x0100000000011001ULL) == UploadRoute::Dlc);
    assert(GeneralUploadRoute(0x80, 0x0100000000010800ULL) == UploadRoute::Unsupported);
    assert(GeneralUploadRoute(0x81, 0x0100000000010000ULL) == UploadRoute::Unsupported);
    assert(GeneralUploadRoute(0x03, 0x0100000000000816ULL) == UploadRoute::Unsupported);
    assert(GeneralUploadRoute(0x82, 0x0100000000011000ULL) == UploadRoute::Dlc);
    assert(GeneralUploadRoute(0x82, 0x0100000000011800ULL) == UploadRoute::Unsupported);
    assert(GeneralUploadRoute(0x80, 0) == UploadRoute::Unsupported);
    assert(IsDiskFull(112));
    assert(IsDiskFull(0x80070070));
    assert(IsDiskFull(39));
    assert(!IsDiskFull(5));
    for (const auto *tag : {L"en", L"es", L"pt-BR"}) {
        I18n::SetUiLanguage(*I18n::FindUiLanguage(tag));
        const auto message = StorageFullMessage(1500000000ULL, 3250000000ULL);
        assert(message.find(L"1.50") != std::wstring::npos);
        assert(message.find(L"3.25") != std::wstring::npos);
        assert(message.find(L"{0}") == std::wstring::npos);
        assert(message.find(L"{1}") == std::wstring::npos);
    }
    unsigned completed{};
    std::string current_file;
    SystemProgress progress{[&](unsigned done, unsigned) { completed = done; }};
    progress.current_file = [&](std::string_view name) { current_file = name; };
    const SystemProgress copied = progress;
    copied.File("example.nca");
    copied(12, 100);
    assert(completed == 12 && current_file == "example.nca");
    SystemProgress{}(0, 0);
    SystemProgress{}.File("example.nca");
    assert(ShowSetupChecklist(false, 0));
    assert(ShowSetupChecklist(false, 200));
    assert(ShowSetupChecklist(true, 0));
    assert(!ShowSetupChecklist(true, 200));
    assert(ShowSetupChecklist(true, 200, true));
    assert(!NeedsGameMode(0));
    assert(NeedsGameMode(1ULL << 30));
    assert(!NeedsGameMode(5120ULL << 20));
}
