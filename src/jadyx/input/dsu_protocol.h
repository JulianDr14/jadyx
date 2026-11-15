// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The DSU ("Cemuhook") protocol as a client speaks it: an app on a phone or a PC is the server and
// listens on UDP (26760 by default); the console asks it for its pads (up to four) and then for
// their data, again at least every few seconds, and the server streams one packet per sample with
// the pad's buttons, sticks and motion. Acceleration comes in g, rotation in degrees per second
// (pitch, yaw, roll).
// PhoneMotionFilter turns those samples into what the emulated pad reports (motion_emu.h's frame
// and units) and an orientation for the on-screen controller.
// Header-only and free of the frontend's headers, so the test gate builds it on the PC.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "jadyx/input/motion_emu.h"

namespace Jadyx::Dsu {

inline constexpr std::uint16_t DefaultPort = 26760;
inline constexpr std::uint16_t ProtocolVersion = 1001;
inline constexpr std::size_t HeaderSize = 16;   ///< Magic, version, length, CRC, id.
inline constexpr std::size_t MaxPacketSize = 100; ///< A pad data response, the largest.

enum class MessageType : std::uint32_t {
    Version = 0x00100000,
    PortInfo = 0x00100001,
    PadData = 0x00100002,
};

/// CRC-32 (IEEE, as zlib's) of `bytes`.
[[nodiscard]] inline std::uint32_t Crc32(std::span<const std::uint8_t> bytes) {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::uint8_t b : bytes) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

namespace Detail {
inline void Put16(std::uint8_t* at, std::uint16_t v) {
    at[0] = static_cast<std::uint8_t>(v);
    at[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void Put32(std::uint8_t* at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) at[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
[[nodiscard]] inline std::uint16_t Get16(const std::uint8_t* at) {
    return static_cast<std::uint16_t>(at[0] | (at[1] << 8));
}
[[nodiscard]] inline std::uint32_t Get32(const std::uint8_t* at) {
    return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) |
           (static_cast<std::uint32_t>(at[2]) << 16) | (static_cast<std::uint32_t>(at[3]) << 24);
}
[[nodiscard]] inline std::uint64_t Get64(const std::uint8_t* at) {
    return static_cast<std::uint64_t>(Get32(at)) | (static_cast<std::uint64_t>(Get32(at + 4)) << 32);
}
[[nodiscard]] inline float GetFloat(const std::uint8_t* at) {
    const std::uint32_t bits = Get32(at);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
} // namespace Detail

/// A client message: the header, the type and `payload`, with its CRC.
struct Packet {
    std::array<std::uint8_t, MaxPacketSize> bytes{};
    std::size_t size{};
    [[nodiscard]] std::span<const std::uint8_t> Span() const { return {bytes.data(), size}; }
};

[[nodiscard]] inline Packet MakeRequest(MessageType type, std::span<const std::uint8_t> payload,
                                        std::uint32_t client_id) {
    Packet packet;
    std::uint8_t* b = packet.bytes.data();
    std::memcpy(b, "DSUC", 4);
    Detail::Put16(b + 4, ProtocolVersion);
    Detail::Put16(b + 6, static_cast<std::uint16_t>(4 + payload.size()));
    Detail::Put32(b + 8, 0);
    Detail::Put32(b + 12, client_id);
    Detail::Put32(b + 16, static_cast<std::uint32_t>(type));
    std::memcpy(b + 20, payload.data(), payload.size());
    packet.size = 20 + payload.size();
    Detail::Put32(b + 8, Crc32(packet.Span()));
    return packet;
}
/// Asks for the server's protocol version: what the search sends, as any server answers it.
[[nodiscard]] inline Packet MakeVersionRequest(std::uint32_t client_id) {
    return MakeRequest(MessageType::Version, {}, client_id);
}
/// Asks what the server's four pad slots hold.
[[nodiscard]] inline Packet MakePortInfoRequest(std::uint32_t client_id) {
    static constexpr std::uint8_t payload[]{4, 0, 0, 0, 0, 1, 2, 3};
    return MakeRequest(MessageType::PortInfo, payload, client_id);
}
/// Subscribes to every pad's data for a few seconds (the server stops if it is not asked again).
[[nodiscard]] inline Packet MakePadDataRequest(std::uint32_t client_id) {
    static constexpr std::uint8_t payload[8]{}; // Flags 0: all pads; slot and MAC unused.
    return MakeRequest(MessageType::PadData, payload, client_id);
}

/// What a server's slot says about its pad.
struct SlotInfo {
    std::uint8_t slot{};
    bool connected{};      ///< State 2: a pad is there.
    std::uint8_t model{};  ///< 2: full gyro.
    std::uint8_t battery{};
};

/// The DualShock-style digital buttons of a pad data packet.
namespace Button {
inline constexpr std::uint16_t Share = 0x0001, L3 = 0x0002, R3 = 0x0004, Options = 0x0008;
inline constexpr std::uint16_t Up = 0x0010, Right = 0x0020, Down = 0x0040, Left = 0x0080;
inline constexpr std::uint16_t L2 = 0x0100, R2 = 0x0200, L1 = 0x0400, R1 = 0x0800;
inline constexpr std::uint16_t Triangle = 0x1000, Circle = 0x2000, Cross = 0x4000, Square = 0x8000;
} // namespace Button

/// One sample from the server, in its own units.
struct PadSample {
    SlotInfo info;
    std::uint16_t buttons{};  ///< Button bits.
    bool home{}, touch{};
    std::uint8_t lx{127}, ly{127}, rx{127}, ry{127}; ///< 0..255, 127 centered, Y up.
    std::uint32_t counter{};
    std::uint64_t timestamp_us{}; ///< The phone's clock; 0 if it sends none.
    float accel_x{}, accel_y{}, accel_z{}; ///< g.
    float pitch{}, yaw{}, roll{};          ///< Degrees per second.
};

/// A server message, checked (magic, length, CRC).
struct Response {
    MessageType type{};
    std::uint32_t server_id{};
    std::uint16_t version{};    ///< Version.
    SlotInfo slot;              ///< PortInfo.
    PadSample pad;              ///< PadData.
};

[[nodiscard]] inline std::optional<Response> ParseResponse(std::span<const std::uint8_t> bytes) {
    using namespace Detail;
    if (bytes.size() < 20 || std::memcmp(bytes.data(), "DSUS", 4) != 0) return std::nullopt;
    const std::size_t length = Get16(bytes.data() + 6);
    if (HeaderSize + length > bytes.size() || length < 4) return std::nullopt;
    std::array<std::uint8_t, 512> copy{};
    const std::size_t total = HeaderSize + length;
    if (total > copy.size()) return std::nullopt;
    std::memcpy(copy.data(), bytes.data(), total);
    const std::uint32_t crc = Get32(copy.data() + 8);
    Put32(copy.data() + 8, 0);
    if (Crc32({copy.data(), total}) != crc) return std::nullopt;

    Response response;
    response.server_id = Get32(bytes.data() + 12);
    response.type = static_cast<MessageType>(Get32(bytes.data() + 16));
    const std::uint8_t* p = bytes.data() + 20;
    const std::size_t size = length - 4;
    const auto slot_of = [](const std::uint8_t* at) {
        return SlotInfo{at[0], at[1] == 2, at[2], at[10]};
    };
    switch (response.type) {
    case MessageType::Version:
        if (size < 2) return std::nullopt;
        response.version = Get16(p);
        return response;
    case MessageType::PortInfo:
        if (size < 11) return std::nullopt;
        response.slot = slot_of(p);
        return response;
    case MessageType::PadData: {
        if (size < 80) return std::nullopt;
        PadSample& pad = response.pad;
        pad.info = slot_of(p);
        pad.counter = Get32(p + 12);
        // The first byte holds Share..Left from the low bit, the second L2..Square.
        pad.buttons = static_cast<std::uint16_t>(p[16] | (p[17] << 8));
        pad.home = p[18] != 0;
        pad.touch = p[19] != 0;
        pad.lx = p[20];
        pad.ly = p[21];
        pad.rx = p[22];
        pad.ry = p[23];
        pad.timestamp_us = Get64(p + 48);
        pad.accel_x = GetFloat(p + 56);
        pad.accel_y = GetFloat(p + 60);
        pad.accel_z = GetFloat(p + 64);
        pad.pitch = GetFloat(p + 68);
        pad.yaw = GetFloat(p + 72);
        pad.roll = GetFloat(p + 76);
        return response;
    }
    }
    return std::nullopt;
}

/// A pad's buttons as a Switch controller's, by position (Circle, the east one, is A), and its
/// sticks in -1..1, up positive.
struct PadInput {
    bool a{}, b{}, x{}, y{}, l{}, r{}, zl{}, zr{}, plus{}, minus{}, lstick{}, rstick{};
    bool up{}, down{}, left{}, right{}, home{};
    float lx{}, ly{}, rx{}, ry{};

    [[nodiscard]] bool AnyButton() const {
        return a || b || x || y || l || r || zl || zr || plus || minus || lstick || rstick || up || down ||
               left || right || home;
    }
    bool operator==(const PadInput&) const = default;
};
[[nodiscard]] inline float StickAxis(std::uint8_t value) {
    return std::clamp((static_cast<float>(value) - 127.0f) / 127.0f, -1.0f, 1.0f);
}
[[nodiscard]] inline PadInput ToSwitch(const PadSample& pad) {
    const auto has = [&](std::uint16_t bit) { return (pad.buttons & bit) != 0; };
    PadInput in;
    in.a = has(Button::Circle);
    in.b = has(Button::Cross);
    in.x = has(Button::Triangle);
    in.y = has(Button::Square);
    in.l = has(Button::L1);
    in.r = has(Button::R1);
    in.zl = has(Button::L2);
    in.zr = has(Button::R2);
    in.plus = has(Button::Options);
    in.minus = has(Button::Share);
    in.lstick = has(Button::L3);
    in.rstick = has(Button::R3);
    in.up = has(Button::Up);
    in.down = has(Button::Down);
    in.left = has(Button::Left);
    in.right = has(Button::Right);
    in.home = pad.home;
    in.lx = StickAxis(pad.lx);
    in.ly = StickAxis(pad.ly);
    in.rx = StickAxis(pad.rx);
    in.ry = StickAxis(pad.ry);
    return in;
}

/// The sample in the emulated pad's frame and units (motion_emu.h: X right, Y forward, Z up;
/// rev/s and g, a pad lying flat reading (0, 0, -1)), as Eden's own DSU client maps it.
[[nodiscard]] inline MotionSample ToPadFrame(const PadSample& pad) {
    constexpr float rev = 1.0f / 360.0f;
    MotionSample sample;
    sample.gyro = {pad.pitch * rev, pad.roll * rev, -pad.yaw * rev};
    sample.accel = {pad.accel_x, -pad.accel_z, pad.accel_y};
    return sample;
}

/// Follows the phone's orientation for the on-screen controller: the gyro turns it and gravity
/// slowly pulls its tilt back to the truth (a complementary filter), and since no sensor says
/// where the screen is, its heading eases back to facing ahead, as a player's hands come back.
class PhoneMotionFilter {
public:
    /// How fast gravity corrects the tilt, and the heading returns to ahead, per second.
    static constexpr float TiltGain = 2.0f;
    static constexpr float HeadingReturn = 0.6f;

    void Reset() {
        orientation = {};
        settled = false;
    }

    /// Advances `dt` seconds with `sample` (pad frame); fills its orientation.
    MotionSample Step(MotionSample sample, float dt) {
        dt = std::clamp(dt, 0.0f, 0.1f);
        const Vec3 measured = sample.accel;
        const float g = std::sqrt(measured.x * measured.x + measured.y * measured.y + measured.z * measured.z);
        if (!settled && g > 0.5f) {
            // The first sample: start at the tilt gravity says, heading ahead.
            orientation = TiltFrom({measured.x / g, measured.y / g, measured.z / g});
            settled = true;
        }
        constexpr float two_pi = 2.0f * std::numbers::pi_v<float>;
        Vec3 w{sample.gyro.x * two_pi, sample.gyro.y * two_pi, sample.gyro.z * two_pi};
        // Only while it is not shaken: then gravity is what the accelerometer feels.
        if (g > 0.75f && g < 1.25f) {
            const Vec3 down = orientation.Conjugate().Rotate({0.0f, 0.0f, -1.0f});
            const Vec3 a{measured.x / g, measured.y / g, measured.z / g};
            // Turns the predicted down toward the measured one: a body turn w moves the pad-frame
            // down by -w, so w goes along measured × predicted.
            const Vec3 e{a.y * down.z - a.z * down.y, a.z * down.x - a.x * down.z, a.x * down.y - a.y * down.x};
            w.x += TiltGain * e.x;
            w.y += TiltGain * e.y;
            w.z += TiltGain * e.z;
        }
        const float angle = std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z) * dt;
        if (angle > 1e-7f) {
            const float n = angle / dt;
            orientation = (orientation * Quat::AxisAngle({w.x / n, w.y / n, w.z / n}, angle)).Normalized();
        }
        // The heading: where the pad's forward points on the floor, eased back to ahead (+Y).
        const Vec3 forward = orientation.Rotate({0.0f, 1.0f, 0.0f});
        if (forward.x * forward.x + forward.y * forward.y > 0.04f) {
            const float heading = std::atan2(-forward.x, forward.y);
            const float back = -heading * std::min(1.0f, HeadingReturn * dt);
            orientation = (Quat::AxisAngle({0, 0, 1}, back) * orientation).Normalized();
        }
        sample.orientation = orientation;
        return sample;
    }

    [[nodiscard]] Quat Orientation() const { return orientation; }

private:
    /// The rotation with no heading whose down, in the pad's frame, is `down`.
    [[nodiscard]] static Quat TiltFrom(Vec3 down) {
        // Rotate the pad's down onto the world's (0, 0, -1): about down × (0,0,-1).
        const Vec3 axis{-down.y, down.x, 0.0f};
        const float s = std::sqrt(axis.x * axis.x + axis.y * axis.y);
        const float c = std::clamp(-down.z, -1.0f, 1.0f);
        if (s < 1e-6f) return c > 0.0f ? Quat{} : Quat::AxisAngle({1, 0, 0}, std::numbers::pi_v<float>);
        // q rotates the pad frame into the world: world_down = q * down.
        return Quat::AxisAngle({axis.x / s, axis.y / s, 0.0f}, std::atan2(s, c));
    }

    Quat orientation;
    bool settled{};
};

/// "a.b.c.d" or "a.b.c.d:port" as an IPv4 address (host order) and a port.
struct Address {
    std::uint32_t ipv4{};
    std::uint16_t port{DefaultPort};
};
[[nodiscard]] inline std::optional<Address> ParseAddress(std::wstring_view text) {
    while (!text.empty() && text.front() == L' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == L' ') text.remove_suffix(1);
    Address address;
    std::uint32_t part = 0;
    int parts = 0, digits = 0;
    std::size_t i = 0;
    for (; i <= text.size(); ++i) {
        const wchar_t c = i < text.size() ? text[i] : L'\0';
        if (c >= L'0' && c <= L'9') {
            part = part * 10 + static_cast<std::uint32_t>(c - L'0');
            if (++digits > 3 || part > 255) return std::nullopt;
            continue;
        }
        if (digits == 0 || (c != L'.' && c != L':' && c != L'\0')) return std::nullopt;
        address.ipv4 = (address.ipv4 << 8) | part;
        ++parts;
        part = 0;
        digits = 0;
        if (c != L'.') break;
        if (parts == 4) return std::nullopt;
    }
    if (parts != 4) return std::nullopt;
    if (i < text.size()) {
        // ":port"
        std::uint32_t port = 0;
        std::size_t j = i + 1;
        if (j >= text.size()) return std::nullopt;
        for (; j < text.size(); ++j) {
            if (text[j] < L'0' || text[j] > L'9') return std::nullopt;
            port = port * 10 + static_cast<std::uint32_t>(text[j] - L'0');
            if (port > 65535) return std::nullopt;
        }
        if (port == 0) return std::nullopt;
        address.port = static_cast<std::uint16_t>(port);
    }
    if (address.ipv4 == 0) return std::nullopt;
    return address;
}

/// "a.b.c.d", with ":port" unless it is the default one.
[[nodiscard]] inline std::wstring FormatAddress(Address address) {
    std::wstring text = std::to_wstring(address.ipv4 >> 24) + L"." + std::to_wstring((address.ipv4 >> 16) & 255) +
                        L"." + std::to_wstring((address.ipv4 >> 8) & 255) + L"." +
                        std::to_wstring(address.ipv4 & 255);
    if (address.port != DefaultPort) text += L":" + std::to_wstring(address.port);
    return text;
}

} // namespace Jadyx::Dsu
