// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <filesystem>
#include <random>
#include <fmt/ranges.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "input_common/drivers/virtual_amiibo.h"

namespace InputCommon {
constexpr PadIdentifier identifier = {
    .guid = Common::UUID{},
    .port = 0,
    .pad = 0,
};

VirtualAmiibo::VirtualAmiibo(std::string input_engine_) : InputEngine(std::move(input_engine_)) {}

VirtualAmiibo::~VirtualAmiibo() = default;

Common::Input::DriverResult VirtualAmiibo::SetPollingMode(
    [[maybe_unused]] const PadIdentifier& identifier_,
    const Common::Input::PollingMode polling_mode_) {
    polling_mode = polling_mode_;

    switch (polling_mode) {
    case Common::Input::PollingMode::NFC:
        state = State::Initialized;
        return Common::Input::DriverResult::Success;
    default:
        if (state == State::TagNearby) {
            CloseAmiibo();
        }
        state = State::Disabled;
        return Common::Input::DriverResult::NotSupported;
    }
}

Common::Input::NfcState VirtualAmiibo::SupportsNfc(
    [[maybe_unused]] const PadIdentifier& identifier_) const {
    return Common::Input::NfcState::Success;
}
Common::Input::NfcState VirtualAmiibo::StartNfcPolling(const PadIdentifier& identifier_) {
    if (state != State::Initialized) {
        return Common::Input::NfcState::WrongDeviceState;
    }
    state = State::WaitingForAmiibo;
    return Common::Input::NfcState::Success;
}

Common::Input::NfcState VirtualAmiibo::StopNfcPolling(const PadIdentifier& identifier_) {
    if (state == State::Disabled) {
        return Common::Input::NfcState::WrongDeviceState;
    }
    if (state == State::TagNearby) {
        CloseAmiibo();
    }
    state = State::Initialized;
    return Common::Input::NfcState::Success;
}

Common::Input::NfcState VirtualAmiibo::ReadAmiiboData(const PadIdentifier& identifier_,
                                                      std::vector<u8>& out_data) {
    if (state != State::TagNearby) {
        return Common::Input::NfcState::WrongDeviceState;
    }

    if (status.tag_type != 1U << 1) {
        return Common::Input::NfcState::InvalidTagType;
    }

    out_data.resize(nfc_data.size());
    memcpy(out_data.data(), nfc_data.data(), nfc_data.size());
    return Common::Input::NfcState::Success;
}

Common::Input::NfcState VirtualAmiibo::WriteNfcData(
    [[maybe_unused]] const PadIdentifier& identifier_, const std::vector<u8>& data) {
    // The path is UTF-8: a narrow string would be read in the system code page.
    const Common::FS::IOFile nfc_file{std::filesystem::path{Common::FS::ToU8String(file_path)},
                                      Common::FS::FileAccessMode::ReadWrite,
                                      Common::FS::FileType::BinaryFile};

    if (!nfc_file.IsOpen()) {
        LOG_ERROR(Core, "Amiibo is already on use");
        return Common::Input::NfcState::WriteFailed;
    }

    if (!nfc_file.Write(data)) {
        LOG_ERROR(Service_NFP, "Error writing to file");
        return Common::Input::NfcState::WriteFailed;
    }

    nfc_data = data;

    return Common::Input::NfcState::Success;
}

Common::Input::NfcState VirtualAmiibo::ReadMifareData(const PadIdentifier& identifier_,
                                                      const Common::Input::MifareRequest& request,
                                                      Common::Input::MifareRequest& out_data) {
    if (state != State::TagNearby) {
        return Common::Input::NfcState::WrongDeviceState;
    }

    if (status.tag_type != 1U << 6) {
        return Common::Input::NfcState::InvalidTagType;
    }

    for (std::size_t i = 0; i < request.data.size(); i++) {
        if (request.data[i].command == 0) {
            continue;
        }
        out_data.data[i].command = request.data[i].command;
        out_data.data[i].sector = request.data[i].sector;

        const std::size_t sector_index =
            request.data[i].sector * sizeof(Common::Input::MifareData::data);

        if (nfc_data.size() < sector_index + sizeof(Common::Input::MifareData::data)) {
            return Common::Input::NfcState::WriteFailed;
        }

        // Ignore the sector key as we don't support it
        memcpy(out_data.data[i].data.data(), nfc_data.data() + sector_index,
               sizeof(Common::Input::MifareData::data));
    }

    return Common::Input::NfcState::Success;
}

Common::Input::NfcState VirtualAmiibo::WriteMifareData(
    const PadIdentifier& identifier_, const Common::Input::MifareRequest& request) {
    if (state != State::TagNearby) {
        return Common::Input::NfcState::WrongDeviceState;
    }

    if (status.tag_type != 1U << 6) {
        return Common::Input::NfcState::InvalidTagType;
    }

    for (std::size_t i = 0; i < request.data.size(); i++) {
        if (request.data[i].command == 0) {
            continue;
        }

        const std::size_t sector_index =
            request.data[i].sector * sizeof(Common::Input::MifareData::data);

        if (nfc_data.size() < sector_index + sizeof(Common::Input::MifareData::data)) {
            return Common::Input::NfcState::WriteFailed;
        }

        // Ignore the sector key as we don't support it
        memcpy(nfc_data.data() + sector_index, request.data[i].data.data(),
               sizeof(Common::Input::MifareData::data));
    }

    return Common::Input::NfcState::Success;
}

VirtualAmiibo::State VirtualAmiibo::GetCurrentState() const {
    return state;
}

VirtualAmiibo::Info VirtualAmiibo::LoadAmiibo(const std::string& filename, bool new_figure_) {
    const Common::FS::IOFile nfc_file{std::filesystem::path{Common::FS::ToU8String(filename)},
                                      Common::FS::FileAccessMode::Read,
                                      Common::FS::FileType::BinaryFile};
    std::vector<u8> data{};

    if (!nfc_file.IsOpen()) {
        return Info::UnableToLoad;
    }

    switch (nfc_file.GetSize()) {
    case AmiiboSize:
    case AmiiboSizeWithoutPassword:
    case AmiiboSizeWithSignature:
        data.resize(AmiiboSize);
        if (nfc_file.Read(data) < AmiiboSizeWithoutPassword) {
            return Info::NotAnAmiibo;
        }
        break;
    case MifareSize:
        data.resize(MifareSize);
        if (nfc_file.Read(data) < MifareSize) {
            return Info::NotAnAmiibo;
        }
        break;
    default:
        return Info::NotAnAmiibo;
    }

    file_path = filename;
    return LoadAmiibo(data, new_figure_);
}

VirtualAmiibo::Info VirtualAmiibo::LoadAmiibo(std::span<u8> data, bool new_figure_) {
    if (state != State::WaitingForAmiibo) {
        return Info::WrongDeviceState;
    }

    switch (data.size_bytes()) {
    case AmiiboSize:
    case AmiiboSizeWithoutPassword:
    case AmiiboSizeWithSignature:
        nfc_data.resize(AmiiboSize);
        status.tag_type = 1U << 1;
        status.uuid_length = 7;
        break;
    case MifareSize:
        nfc_data.resize(MifareSize);
        status.tag_type = 1U << 6;
        status.uuid_length = 4;
        break;
    default:
        return Info::NotAnAmiibo;
    }

    status.uuid = {};
    status.protocol = 1;
    state = State::TagNearby;
    status.state = Common::Input::NfcState::NewAmiibo,
    memcpy(nfc_data.data(), data.data(), data.size_bytes());
    memcpy(status.uuid.data(), nfc_data.data(), status.uuid_length);
    // Drawn here, once, so every read during this placement sees the same tag. NXP's maker byte
    // stays first, as on a real NTAG215.
    new_figure = new_figure_ && status.tag_type == 1U << 1;
    if (new_figure) {
        std::random_device seed;
        std::mt19937 rng{seed()};
        std::uniform_int_distribution<int> byte{0, 255};
        for (std::size_t i = 1; i < status.uuid_length; ++i) {
            status.uuid[i] = static_cast<u8>(byte(rng));
        }
    }
    SetNfc(identifier, status);
    return Info::Success;
}

VirtualAmiibo::Info VirtualAmiibo::ReloadAmiibo() {
    if (state == State::TagNearby) {
        SetNfc(identifier, status);
        return Info::Success;
    }

    return LoadAmiibo(file_path);
}

VirtualAmiibo::Info VirtualAmiibo::CloseAmiibo() {
    if (state != State::TagNearby) {
        return Info::Success;
    }

    state = State::WaitingForAmiibo;
    new_figure = false;
    status.state = Common::Input::NfcState::AmiiboRemoved;
    SetNfc(identifier, status);
    status.tag_type = 0;
    return Info::Success;
}

bool VirtualAmiibo::IsNewFigure() const {
    return new_figure && state == State::TagNearby;
}

std::string VirtualAmiibo::GetLastFilePath() const {
    return file_path;
}

} // namespace InputCommon
