// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// DSU controllers (dsu_protocol.h, dsu_status.h): the packets the console sends and reads, a
// pad's buttons and sticks as a Switch controller's, its motion in the emulated pad's frame, the orientation the on-screen controller shows, and the
// addresses a player types, and the pages' rows and assignments.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <vector>

#include "jadyx/input/dsu_protocol.h"
#include "jadyx/input/dsu_status.h"

namespace {

int failures = 0;
void Check(bool ok, int line) {
    if (!ok) {
        std::printf("dsu-phone: check failed at line %d\n", line);
        ++failures;
    }
}
#define CHECK(x) Check((x), __LINE__)

bool Near(float a, float b, float eps) { return std::abs(a - b) <= eps; }

using namespace Jadyx;

void Put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void PutFloat(std::vector<std::uint8_t>& b, std::size_t at, float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    Put32(b, at, v);
}

/// A server's pad data packet, as a phone app sends it.
std::vector<std::uint8_t> ServerPad(std::uint32_t counter, float ax, float ay, float az, float pitch, float yaw,
                                    float roll, std::uint64_t timestamp) {
    std::vector<std::uint8_t> b(100, 0);
    std::memcpy(b.data(), "DSUS", 4);
    b[4] = 1001 & 255;
    b[5] = 1001 >> 8;
    b[6] = 84; // Type + 80.
    Put32(b, 12, 0x1234);
    Put32(b, 16, 0x00100002);
    std::uint8_t* p = b.data() + 20;
    p[0] = 0;  // Slot.
    p[1] = 2;  // Connected.
    p[2] = 2;  // Full gyro.
    p[10] = 5; // Battery full.
    p[11] = 1;
    Put32(b, 20 + 12, counter);
    Put32(b, 20 + 48, static_cast<std::uint32_t>(timestamp));
    Put32(b, 20 + 52, static_cast<std::uint32_t>(timestamp >> 32));
    PutFloat(b, 20 + 56, ax);
    PutFloat(b, 20 + 60, ay);
    PutFloat(b, 20 + 64, az);
    PutFloat(b, 20 + 68, pitch);
    PutFloat(b, 20 + 72, yaw);
    PutFloat(b, 20 + 76, roll);
    Put32(b, 8, Dsu::Crc32(b));
    return b;
}

void Packets() {
    // The standard check value.
    const char* text = "123456789";
    CHECK(Dsu::Crc32({reinterpret_cast<const std::uint8_t*>(text), 9}) == 0xCBF43926u);

    const auto version = Dsu::MakeVersionRequest(7);
    CHECK(version.size == 20);
    CHECK(std::memcmp(version.bytes.data(), "DSUC", 4) == 0);
    CHECK(version.bytes[6] == 4);
    const auto ports = Dsu::MakePortInfoRequest(7);
    CHECK(ports.size == 28);
    CHECK(ports.bytes[6] == 12);
    CHECK(ports.bytes[20] == 4 && ports.bytes[27] == 3);
    const auto pads = Dsu::MakePadDataRequest(7);
    CHECK(pads.size == 28);
    // The CRC covers the packet with its own field zeroed.
    auto copy = pads.bytes;
    std::memset(copy.data() + 8, 0, 4);
    const std::uint32_t crc = Dsu::Crc32({copy.data(), pads.size});
    CHECK((pads.bytes[8] | (pads.bytes[9] << 8) | (pads.bytes[10] << 16) | (static_cast<std::uint32_t>(pads.bytes[11]) << 24)) == crc);
    // A client's own packet is not a server's.
    CHECK(!Dsu::ParseResponse(pads.Span()));

    auto pad = ServerPad(42, 0.1f, -1.0f, 0.2f, 90.0f, -45.0f, 180.0f, 1'000'000'000'123ull);
    const auto parsed = Dsu::ParseResponse(pad);
    CHECK(parsed && parsed->type == Dsu::MessageType::PadData);
    if (parsed) {
        CHECK(parsed->pad.counter == 42);
        CHECK(parsed->pad.info.connected && parsed->pad.info.model == 2 && parsed->pad.info.battery == 5);
        CHECK(parsed->pad.timestamp_us == 1'000'000'000'123ull);
        CHECK(Near(parsed->pad.accel_y, -1.0f, 1e-6f) && Near(parsed->pad.roll, 180.0f, 1e-6f));
        // Degrees per second to rev/s, in the pad's frame: pitch about X, roll about Y, yaw
        // about -Z; gravity of a phone lying flat ends up as the emulated pad's (0, 0, -1).
        const MotionSample s = Dsu::ToPadFrame(parsed->pad);
        CHECK(Near(s.gyro.x, 0.25f, 1e-6f) && Near(s.gyro.y, 0.5f, 1e-6f) && Near(s.gyro.z, 0.125f, 1e-6f));
        CHECK(Near(s.accel.x, 0.1f, 1e-6f) && Near(s.accel.y, -0.2f, 1e-6f) && Near(s.accel.z, -1.0f, 1e-6f));
    }
    // A flipped bit fails the CRC; a cut packet fails the length.
    pad[60] ^= 1;
    CHECK(!Dsu::ParseResponse(pad));
    pad[60] ^= 1;
    CHECK(!Dsu::ParseResponse({pad.data(), 60}));
}

float TiltDegrees(Quat q) {
    // Angle between the pad's up and the world's.
    const Vec3 up = q.Rotate({0, 0, 1});
    return std::acos(std::clamp(up.z, -1.0f, 1.0f)) * 180.0f / std::numbers::pi_v<float>;
}

void Filter() {
    // Lying flat and still: flat, and stays so.
    {
        Dsu::PhoneMotionFilter f;
        MotionSample s{};
        s.accel = {0, 0, -1};
        for (int i = 0; i < 200; ++i) s = f.Step(s, 0.01f);
        CHECK(TiltDegrees(s.orientation) < 0.5f);
    }
    // Held tilted 30° nose up (about X): the first sample already shows it.
    {
        Dsu::PhoneMotionFilter f;
        const float a = 30.0f * std::numbers::pi_v<float> / 180.0f;
        const Quat truth = Quat::AxisAngle({1, 0, 0}, a);
        MotionSample s{};
        s.accel = truth.Conjugate().Rotate({0, 0, -1});
        s = f.Step(s, 0.01f);
        CHECK(Near(TiltDegrees(s.orientation), 30.0f, 0.5f));
        // Its front edge is up, as the truth's.
        CHECK(s.orientation.Rotate({0, 1, 0}).z > 0.4f);
        CHECK(truth.Rotate({0, 1, 0}).z > 0.4f);
    }
    // A wrong start is pulled to gravity's tilt: start flat, then gravity says 40° roll.
    {
        Dsu::PhoneMotionFilter f;
        MotionSample s{};
        s.accel = {0, 0, -1};
        s = f.Step(s, 0.01f);
        const Quat truth = Quat::AxisAngle({0, 1, 0}, 40.0f * std::numbers::pi_v<float> / 180.0f);
        for (int i = 0; i < 400; ++i) {
            MotionSample in{};
            in.accel = truth.Conjugate().Rotate({0, 0, -1});
            s = f.Step(in, 0.01f);
        }
        CHECK(Near(TiltDegrees(s.orientation), 40.0f, 2.0f));
        // Same side as the truth.
        CHECK(s.orientation.Rotate({1, 0, 0}).z * truth.Rotate({1, 0, 0}).z > 0.0f);
    }
    // A turn at 90°/s about the pad's up for half a second shows about 45° of heading, then
    // eases back to facing ahead while it stays still.
    {
        Dsu::PhoneMotionFilter f;
        MotionSample s{};
        s.accel = {0, 0, -1};
        s = f.Step(s, 0.01f);
        for (int i = 0; i < 50; ++i) {
            MotionSample in{};
            in.accel = {0, 0, -1};
            in.gyro = {0, 0, 0.25f}; // rev/s
            s = f.Step(in, 0.01f);
        }
        const Vec3 forward = s.orientation.Rotate({0, 1, 0});
        const float heading = std::atan2(-forward.x, forward.y) * 180.0f / std::numbers::pi_v<float>;
        CHECK(heading > 30.0f && heading < 46.0f);
        for (int i = 0; i < 800; ++i) {
            MotionSample in{};
            in.accel = {0, 0, -1};
            s = f.Step(in, 0.01f);
        }
        const Vec3 back = s.orientation.Rotate({0, 1, 0});
        CHECK(std::abs(std::atan2(-back.x, back.y)) * 180.0f / std::numbers::pi_v<float> < 2.0f);
    }
    // Pitch from the gyro tips the front edge up as gravity agrees (the two senses match).
    {
        Dsu::PhoneMotionFilter f;
        MotionSample s{};
        s.accel = {0, 0, -1};
        s = f.Step(s, 0.01f);
        Quat truth{};
        for (int i = 0; i < 50; ++i) {
            truth = (truth * Quat::AxisAngle({1, 0, 0}, 0.25f * 2.0f * std::numbers::pi_v<float> * 0.01f)).Normalized();
            MotionSample in{};
            in.accel = truth.Conjugate().Rotate({0, 0, -1});
            in.gyro = {0.25f, 0, 0};
            s = f.Step(in, 0.01f);
        }
        CHECK(Near(TiltDegrees(s.orientation), 45.0f, 3.0f));
        CHECK(s.orientation.Rotate({0, 1, 0}).z > 0.5f);
    }
    // A shake (|a| far from 1 g) does not drag the tilt.
    {
        Dsu::PhoneMotionFilter f;
        MotionSample s{};
        s.accel = {0, 0, -1};
        s = f.Step(s, 0.01f);
        for (int i = 0; i < 100; ++i) {
            MotionSample in{};
            in.accel = {(i % 2) ? 3.0f : -3.0f, 0, -1};
            s = f.Step(in, 0.01f);
        }
        CHECK(TiltDegrees(s.orientation) < 1.0f);
    }
}

void Addresses() {
    const auto a = Dsu::ParseAddress(L"192.168.1.23");
    CHECK(a && a->ipv4 == 0xC0A80117u && a->port == Dsu::DefaultPort);
    const auto b = Dsu::ParseAddress(L" 10.0.0.5:26761 ");
    CHECK(b && b->ipv4 == 0x0A000005u && b->port == 26761);
    CHECK(!Dsu::ParseAddress(L""));
    CHECK(!Dsu::ParseAddress(L"192.168.1"));
    CHECK(!Dsu::ParseAddress(L"192.168.1.256"));
    CHECK(!Dsu::ParseAddress(L"192.168.1.2.3"));
    CHECK(!Dsu::ParseAddress(L"192.168..3"));
    CHECK(!Dsu::ParseAddress(L"192.168.1.3:"));
    CHECK(!Dsu::ParseAddress(L"192.168.1.3:70000"));
    CHECK(!Dsu::ParseAddress(L"0.0.0.0"));
    CHECK(!Dsu::ParseAddress(L"phone"));
    CHECK(Dsu::FormatAddress(*a) == L"192.168.1.23");
    CHECK(Dsu::FormatAddress(*b) == L"10.0.0.5:26761");
}

void Buttons() {
    auto pad = ServerPad(1, 0, 0, -1, 0, 0, 0, 0);
    std::uint8_t* p = pad.data() + 20;
    // Circle (east) is A, Cross B, Triangle X, Square Y; L1/R1 are L/R, L2/R2 ZL/ZR.
    const std::uint16_t buttons = Dsu::Button::Circle | Dsu::Button::Square | Dsu::Button::L2 |
                                  Dsu::Button::Options | Dsu::Button::Up | Dsu::Button::R3;
    p[16] = static_cast<std::uint8_t>(buttons & 0xFF);
    p[17] = static_cast<std::uint8_t>(buttons >> 8);
    p[18] = 1; // Home.
    p[20] = 255; // Left stick right.
    p[21] = 0;   // Left stick down.
    p[22] = 127;
    p[23] = 127;
    Put32(pad, 8, 0);
    Put32(pad, 8, Dsu::Crc32(pad));
    const auto parsed = Dsu::ParseResponse(pad);
    CHECK(parsed && parsed->pad.buttons == buttons && parsed->pad.home);
    if (parsed) {
        const Dsu::PadInput in = Dsu::ToSwitch(parsed->pad);
        CHECK(in.a && !in.b && !in.x && in.y && in.zl && !in.zr && !in.l && !in.r);
        CHECK(in.plus && !in.minus && in.up && !in.down && in.rstick && !in.lstick && in.home);
        CHECK(Near(in.lx, 1.0f, 1e-6f) && Near(in.ly, -1.0f, 1e-2f) && Near(in.rx, 0.0f, 1e-6f));
        CHECK(in.AnyButton());
    }
    CHECK(!Dsu::ToSwitch(Dsu::PadSample{}).AnyButton());
}

void Status() {
    // Auto, players 1..8, gyro only, off, and around.
    CHECK(StepDsuAssign(DsuAssign::Auto, false) == DsuAssign::Player1);
    CHECK(StepDsuAssign(DsuAssign::Auto, true) == DsuAssign::Off);
    CHECK(StepDsuAssign(DsuAssign::Player8, false) == DsuAssign::GyroOnly);
    CHECK(StepDsuAssign(DsuAssign::Off, false) == DsuAssign::Auto);
    CHECK(DsuPinnedPlayer(DsuAssign::Player3) == 3 && DsuPinnedPlayer(DsuAssign::GyroOnly) == 0 &&
          DsuPinnedPlayer(DsuAssign::Auto) == 0);
    const std::uint64_t key = DsuPadKey(0xC0A80117u, 26760, 2);
    CHECK(DsuPadSlot(key) == 2 && key != DsuPadKey(0xC0A80117u, 26761, 2) && key != DsuPadKey(0xC0A80118u, 26760, 2));

    DsuStatus status{};
    CHECK(DsuRows(status).empty() && status.PadCount() == 0 && status.MotionPads().empty());
    status.servers.push_back({L"192.168.1.23", DsuServerState::Connected, 60,
                              {DsuPadStatus{key, 2, DsuAssign::Auto, 0, true},
                               DsuPadStatus{key + 1, 3, DsuAssign::Off, 0, false}}});
    status.servers.push_back({L"192.168.1.40", DsuServerState::Connecting, 0, {}});
    const auto rows = DsuRows(status);
    CHECK(rows.size() == 4);
    CHECK(rows.size() == 4 && rows[0].server == 0 && !rows[0].pad && rows[1].pad == 0u && rows[2].pad == 1u &&
          rows[3].server == 1 && !rows[3].pad);
    CHECK(status.PadCount() == 2 && status.MotionPads() == std::vector<std::uint64_t>{key});
    const auto found = status.Find(key + 1);
    CHECK(found && found->first == 0 && found->second == 1 && !status.Find(42));
}

} // namespace

int main() {
    Packets();
    Filter();
    Addresses();
    Buttons();
    Status();
    if (failures) {
        std::printf("dsu-phone: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("dsu-phone: all checks passed\n");
    return 0;
}
