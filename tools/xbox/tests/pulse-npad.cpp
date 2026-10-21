// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <iostream>
#include "common/pulse_version.h"
#include "hid_core/resources/npad/npad_pulse.h"

static_assert(Common::Pulse::Compatible(Common::Pulse::Version{1, 1, 0},
                                       Common::Pulse::CurrentVersion));

namespace {
using namespace Service;

void Require(bool condition) {
    if (!condition) throw std::runtime_error("Npad Pulse regression");
}

void Load(std::span<const u8> bytes, u32 revision) {
    Pulse::StateReader in{bytes};
    Require(in.U64() == 83); // IHidServer's aruid precedes the Npad state.
    Require(in.Bool());
    HID::NPadData restored;
    Require(static_cast<u32>(HID::LoadNpadPulseState(in, restored)) == revision);
    for (size_t i = 0; i < HID::MaxSupportedNpadIdTypes; ++i) {
        Require(in.Bool() == (i % 2 == 0));
        Require(in.U32() == i % 2);
    }
    in.End();
    Pulse::StateWriter roundtrip;
    roundtrip.U64(83);
    roundtrip.Bool(true);
    HID::SaveNpadPulseState(roundtrip, static_cast<HID::NpadRevision>(revision), restored);
    for (size_t i = 0; i < HID::MaxSupportedNpadIdTypes; ++i) {
        roundtrip.Bool(i % 2 == 0);
        roundtrip.U32(static_cast<u32>(i % 2));
    }
    Require(std::ranges::equal(bytes, roundtrip.bytes));
}

template <class F>
void Reject(F&& f) {
    try { f(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Malformed Npad state accepted");
}
} // namespace

int main() {
    try {
        HID::NPadData data;
        data.SetNpadSystemCommonPolicy(true);
        data.SetNpadJoyHoldType(HID::NpadJoyHoldType::Horizontal);
        data.SetHomeProtectionEnabled(true, Core::HID::NpadIdType::Player1);
        data.SetCaptureButtonAssignment(Core::HID::NpadButton::A, 0);
        const std::array ids{Core::HID::NpadIdType::Player1, Core::HID::NpadIdType::Handheld};
        Require(data.SetSupportedNpadIdType(ids).IsSuccess());
        for (const u32 revision : {0U, 1U, 2U, 3U, 4U, 5U, 0x80000000U, 0xffffffffU}) {
            Pulse::StateWriter out;
            out.U64(83);
            out.Bool(true);
            HID::SaveNpadPulseState(out, static_cast<HID::NpadRevision>(revision), data);
            // Independent legacy writer layout: no length prefix, no format migration.
            Pulse::StateWriter legacy;
            legacy.U64(83);
            legacy.Bool(true);
            legacy.U32(revision);
            data.PulseSave(legacy);
            Require(legacy.bytes == out.bytes);
            for (size_t i = 0; i < HID::MaxSupportedNpadIdTypes; ++i) {
                out.Bool(i % 2 == 0);
                out.U32(static_cast<u32>(i % 2));
            }
            Load(out.bytes, revision);
            const Pulse::HandlerRecord record{.port = "hid", .kind = "IHidServer",
                                             .root = true, .state = out.bytes};
            Load(Pulse::DecodeHandlerRecord(Pulse::EncodeHandlerRecord(record)).state, revision);
            for (size_t size = 0; size < out.bytes.size(); ++size) {
                Reject([&] { Load(std::span{out.bytes}.first(size), revision); });
            }
            auto bad = out.bytes;
            bad.push_back(0);
            Reject([&] { Load(bad, revision); });
            // Invalid Npad supported-id count, after aruid/bool/revision/status/3xU64.
            bad = out.bytes;
            bad[8 + 1 + 4 + 4 + 24] = 0xff;
            Reject([&] { Load(bad, revision); });
            bad = out.bytes;
            bad[8] = 2;
            Reject([&] { Load(bad, revision); });
        }
        Pulse::StateWriter absent;
        absent.U64(83);
        absent.Bool(false);
        Pulse::StateReader in{absent.bytes};
        Require(in.U64() == 83 && !in.Bool());
        in.End();
        std::cout << "Pulse Npad: 8 revisions, legacy bytes, handler envelope, configuration, "
                     "controller tail, truncations and malformed states passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
