// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The emulated gyro (motion_emu.h) and the on-screen controller (motion_view.cpp): the samples a
// game reads follow the Switch's IMU convention, and the picture draws a controller that moves.
// With a folder as the first argument it also saves the pictures there as BMP files to look at.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <string>

#include "jadyx/game/motion_view.h"
#include "jadyx/input/motion_emu.h"
#include "jadyx/input/rumble_settings.h"

namespace {

int failures = 0;
void Check(bool ok, int line) {
    if (!ok) {
        std::printf("motion-emu: check failed at line %d\n", line);
        ++failures;
    }
}
#define CHECK(x) Check((x), __LINE__)

bool Near(float a, float b, float eps) {
    return std::abs(a - b) <= eps;
}
float Length(Jadyx::Vec3 v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

void SaveBmp(const std::string& path, const D3D12::MenuImage& image) {
    std::ofstream out(path, std::ios::binary);
    const auto row = (image.width * 3 + 3) & ~3u;
    const auto put32 = [&](unsigned v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto put16 = [&](unsigned short v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("BM", 2);
    put32(54 + row * image.height);
    put32(0);
    put32(54);
    put32(40);
    put32(image.width);
    put32(image.height);
    put16(1);
    put16(24);
    put32(0);
    put32(row * image.height);
    put32(2835);
    put32(2835);
    put32(0);
    put32(0);
    std::string line(row, '\0');
    for (unsigned y = image.height; y-- > 0;) {
        for (unsigned x = 0; x < image.width; ++x) {
            const unsigned char* px = image.pixels.data() + (static_cast<std::size_t>(y) * image.width + x) * 4;
            // Over a stand-in for a game: bright sky on the left half, dark ground on the right.
            const unsigned back = x < image.width / 2 ? 0xC8 : 0x30;
            const auto over = [&](unsigned c) { return static_cast<char>(c + back * (255 - px[3]) / 255); };
            line[x * 3 + 0] = over(px[2]);
            line[x * 3 + 1] = over(px[1]);
            line[x * 3 + 2] = over(px[0]);
        }
        out.write(line.data(), row);
    }
}

/// Pixels the controller covers whole (its soft shadow stays well under opaque).
std::size_t DrawnPixels(const D3D12::MenuImage& image) {
    std::size_t drawn = 0;
    for (std::size_t i = 3; i < image.pixels.size(); i += 4) {
        if (image.pixels[i] == 255) ++drawn;
    }
    return drawn;
}

} // namespace

int main(int argc, char** argv) {
    using namespace Jadyx;
    constexpr float Pi = std::numbers::pi_v<float>;
    constexpr float Dt = 0.004f;

    // Off: a pad lying flat and still.
    {
        MotionEmu emu;
        const MotionSample s = emu.Step(Dt, 1.0f, 1.0f, true);
        CHECK(Near(s.accel.x, 0, 1e-5f) && Near(s.accel.y, 0, 1e-5f) && Near(s.accel.z, -1, 1e-5f));
        CHECK(Length(s.gyro) < 1e-5f);
    }

    // Tilt forward: the front edge dips, gravity leans toward the pad's +Y, and the gyro (rev/s
    // about X) adds up to the angle.
    {
        MotionEmu emu;
        emu.SetMode(MotionMode::On);
        float turned_x = 0.0f;
        MotionSample s;
        for (int i = 0; i < 500; ++i) {
            s = emu.Step(Dt, 0.0f, 1.0f, false);
            turned_x += s.gyro.x * 2.0f * Pi * Dt;
            CHECK(Near(Length(s.accel), 1.0f, 1e-3f));
        }
        CHECK(s.orientation.Rotate({0, 1, 0}).z < -0.7f);
        CHECK(Near(s.accel.y, std::sin(MotionEmu::MaxTiltRadians), 0.01f));
        CHECK(Near(turned_x, -MotionEmu::MaxTiltRadians, 0.02f));
        CHECK(Length(s.gyro) < 0.01f); // Settled.
        // Let go: it levels out.
        for (int i = 0; i < 500; ++i) s = emu.Step(Dt, 0.0f, 0.0f, false);
        CHECK(Near(s.accel.z, -1.0f, 1e-3f));
    }

    // Tilt right: the right side dips.
    {
        MotionEmu emu;
        emu.SetMode(MotionMode::On);
        MotionSample s;
        for (int i = 0; i < 500; ++i) s = emu.Step(Dt, 1.0f, 0.0f, false);
        CHECK(s.orientation.Rotate({1, 0, 0}).z < -0.7f);
        CHECK(s.accel.x > 0.7f);
    }

    // Shake: the shake button swings the accelerometer past 1 g; it dies out after the release.
    {
        MotionEmu emu;
        emu.SetMode(MotionMode::On);
        float peak = 0.0f;
        for (int i = 0; i < 250; ++i) peak = std::max(peak, Length(emu.Step(Dt, 0.0f, 0.0f, true).accel));
        CHECK(peak > 2.0f);
        MotionSample s;
        for (int i = 0; i < 100; ++i) s = emu.Step(Dt, 0.0f, 0.0f, false);
        CHECK(Near(Length(s.accel), 1.0f, 1e-4f) && Length(s.gyro) < 1e-4f);
        CHECK(s.shake.x == 0.0f && s.shake.z == 0.0f);
        // Turned off and on again, it starts from rest.
        for (int i = 0; i < 500; ++i) emu.Step(Dt, 1.0f, 0.0f, false);
        emu.SetMode(MotionMode::Off);
        emu.SetMode(MotionMode::On);
        CHECK(Near(emu.Step(Dt, 0.0f, 0.0f, false).accel.z, -1.0f, 1e-5f));
        // Older saved modes (tilt, aim, shake) all read as on.
        CHECK(MotionModeFromStored(0) == MotionMode::Off && MotionModeFromStored(1) == MotionMode::On &&
              MotionModeFromStored(3) == MotionMode::On);
    }

    // Rumble: off or no amplitude is still; strength scales it, capped at full.
    {
        CHECK(RumbleMotor({true, 100}, 0.5f) == 0.5f);
        CHECK(RumbleMotor({false, 100}, 0.5f) == 0.0f);
        CHECK(RumbleMotor({true, 50}, 0.5f) == 0.25f);
        CHECK(RumbleMotor({true, 200}, 0.8f) == 1.0f);
        CHECK(RumbleMotor({true, 100}, 0.0f) == 0.0f);
        const RumbleSettings global{true, 75};
        CHECK(ResolveRumble(global, {}) == global);
        CHECK(ResolveRumble(global, GameRumble{false, std::nullopt}) == (RumbleSettings{false, 75}));
    }

    // The picture: in the bottom-left corner, a controller floating on its shadow (premultiplied
    // RGBA, clear around it), which changes as it tilts and as the game rumbles.
    {
        const std::pair<unsigned, unsigned> screen{1920, 1080};
        MotionViewState state;
        state.mode = MotionMode::On;
        const D3D12::MenuImage flat = DrawMotionView(state, screen, 0.0f);
        CHECK(flat.width > 0 && flat.height > 0);
        CHECK(flat.pixels.size() == static_cast<std::size_t>(flat.width) * flat.height * 4);
        CHECK(flat.placed && flat.x < 100 && flat.y + flat.height <= 1080 && flat.y > 1080 / 2);
        bool premultiplied = true;
        for (std::size_t i = 0; i + 3 < flat.pixels.size(); i += 4) {
            premultiplied &= std::max({flat.pixels[i], flat.pixels[i + 1], flat.pixels[i + 2]}) <= flat.pixels[i + 3];
        }
        CHECK(premultiplied);
        CHECK(flat.pixels[3] == 0); // No card: the top-left corner shows the game.
        const std::size_t middle = (static_cast<std::size_t>(flat.height / 2) * flat.width + flat.width / 2) * 4;
        CHECK(flat.pixels[middle + 3] == 255);
        const std::size_t drawn = DrawnPixels(flat);
        CHECK(drawn > flat.pixels.size() / 4 / 8); // The controller covers a good part of it.

        MotionEmu emu;
        emu.SetMode(MotionMode::On);
        MotionSample s;
        for (int i = 0; i < 500; ++i) s = emu.Step(Dt, 0.7f, 0.7f, false);
        state.orientation = s.orientation;
        state.left_x = -1.0f;
        state.buttons = MotionViewButton::A | MotionViewButton::ZR;
        const D3D12::MenuImage tilted = DrawMotionView(state, screen, 0.0f);
        CHECK(tilted.pixels != flat.pixels);

        state.rumble = 1.0f;
        const D3D12::MenuImage rumbling = DrawMotionView(state, screen, 0.37f);
        CHECK(rumbling.pixels != tilted.pixels);

        // Too small a screen draws nothing rather than a broken panel.
        CHECK(DrawMotionView(state, {32, 32}, 0.0f).pixels.empty());

        if (argc > 1) {
            const std::string folder = argv[1];
            SaveBmp(folder + "/motion-flat.bmp", flat);
            SaveBmp(folder + "/motion-tilted.bmp", tilted);
            SaveBmp(folder + "/motion-rumble.bmp", rumbling);
            std::printf("motion-emu: pictures saved in %s\n", folder.c_str());
        }
    }

    if (failures != 0) {
        std::printf("motion-emu: %d checks failed\n", failures);
        return 1;
    }
    std::printf("motion-emu: all checks passed\n");
    return 0;
}
