// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// An emulated gyro and accelerometer for a pad that has neither. A stick tilts a virtual
// controller and a button shakes it; this turns its orientation into the samples a Switch pad's
// IMU reports:
// - gyro: angular velocity in the pad's frame, in revolutions per second;
// - accel: gravity in the pad's frame, in g (a pad lying flat reads (0, 0, -1)).
// The pad's frame is X right, Y forward (away from the player), Z up out of its face, as Eden's
// SDL driver converts real controllers.
// Header-only and free of the frontend's headers, so the test gate builds it on the PC.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace Jadyx {

/// The emulated gyro, per game.
enum class MotionMode : std::uint8_t {
    Off, ///< The pad lies still on a table: gravity only, no turn.
    On,  ///< The gyro stick tilts the controller (where it points is how far it leans) and the
         ///< shake button shakes it while held.
};
inline constexpr std::uint8_t MotionModeCount = 2;
/// Saved modes before Off/On (Tilt, Aim, Shake) all read as On.
[[nodiscard]] inline MotionMode MotionModeFromStored(int value) {
    return value > 0 ? MotionMode::On : MotionMode::Off;
}

/// Which stick tilts the emulated controller (Settings > Controller); the game no longer sees it
/// while the gyro is on.
enum class MotionStick : std::uint8_t { Right, Left };
inline constexpr std::uint8_t MotionStickCount = 2;

/// The button that shakes it, by the Xbox pad's names; the game no longer sees it either.
enum class MotionShakeButton : std::uint8_t {
    StickClick, ///< Pressing the gyro stick itself.
    LB,
    RB,
    LT,
    RT,
};
inline constexpr std::uint8_t MotionShakeButtonCount = 5;

/// When the picture of the emulated controller shows, while a MotionMode is on.
enum class MotionViewMode : std::uint8_t {
    Always, ///< The whole time.
    Moving, ///< While it moves, and a moment after.
    Hidden,
};
inline constexpr std::uint8_t MotionViewModeCount = 3;

struct Vec3 {
    float x{}, y{}, z{};
};

/// A rotation (unit quaternion) from the pad's frame to the world's (Z up).
struct Quat {
    float w{1.0f}, x{}, y{}, z{};

    [[nodiscard]] static Quat AxisAngle(Vec3 axis, float radians) {
        const float s = std::sin(radians * 0.5f);
        return {std::cos(radians * 0.5f), axis.x * s, axis.y * s, axis.z * s};
    }
    [[nodiscard]] Quat operator*(const Quat& o) const {
        return {w * o.w - x * o.x - y * o.y - z * o.z, w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x, w * o.z + x * o.y - y * o.x + z * o.w};
    }
    [[nodiscard]] Quat Conjugate() const {
        return {w, -x, -y, -z};
    }
    [[nodiscard]] Quat Normalized() const {
        const float n = std::sqrt(w * w + x * x + y * y + z * z);
        return n > 0.0f ? Quat{w / n, x / n, y / n, z / n} : Quat{};
    }
    /// The vector `v` of the pad's frame in the world's.
    [[nodiscard]] Vec3 Rotate(Vec3 v) const {
        const Quat p = *this * Quat{0.0f, v.x, v.y, v.z} * Conjugate();
        return {p.x, p.y, p.z};
    }
};

/// Shortest-path interpolation, `t` in [0, 1].
[[nodiscard]] inline Quat Slerp(Quat a, Quat b, float t) {
    float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (dot < 0.0f) {
        b = {-b.w, -b.x, -b.y, -b.z};
        dot = -dot;
    }
    if (dot > 0.9995f) {
        return Quat{a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                    a.z + (b.z - a.z) * t}
            .Normalized();
    }
    const float theta = std::acos(dot);
    const float sa = std::sin((1.0f - t) * theta) / std::sin(theta);
    const float sb = std::sin(t * theta) / std::sin(theta);
    return {a.w * sa + b.w * sb, a.x * sa + b.x * sb, a.y * sa + b.y * sb, a.z * sa + b.z * sb};
}

/// One IMU sample, plus what the on-screen controller shows.
struct MotionSample {
    Vec3 gyro;  ///< rev/s, pad's frame.
    Vec3 accel; ///< g, pad's frame.
    Quat orientation;
    /// The shake's displacement of the controller for the picture, about [-1, 1] per axis.
    Vec3 shake;
};

/// The virtual controller. Step it at the input rate with the stick in [-1, 1] (x right, y up).
class MotionEmu {
public:
    /// How far the stick leans the controller at full tilt.
    static constexpr float MaxTiltRadians = 50.0f * std::numbers::pi_v<float> / 180.0f;
    /// How hard and how fast the shake moves it.
    static constexpr float ShakeG = 2.5f;
    static constexpr float ShakeHz = 6.0f;

    void SetMode(MotionMode next) {
        if (next != mode) {
            mode = next;
            Reset();
        }
    }
    [[nodiscard]] MotionMode Mode() const {
        return mode;
    }
    /// Back to lying flat and still.
    void Reset() {
        orientation = {};
        shake_phase = 0.0f;
        shake_amount = 0.0f;
    }

    /// Advances `dt` seconds. `shaking` is the shake button; Off ignores the stick and it.
    MotionSample Step(float dt, float stick_x, float stick_y, bool shaking) {
        dt = std::clamp(dt, 0.0005f, 0.1f);
        const Quat before = orientation;
        if (mode == MotionMode::On) {
            // Stick forward dips the front edge (a turn about X), right dips the right side (about
            // Y), with the stick's springy return eased so the gyro never spikes.
            const Quat pitch = Quat::AxisAngle({1, 0, 0}, -stick_y * MaxTiltRadians);
            const Quat roll = Quat::AxisAngle({0, 1, 0}, stick_x * MaxTiltRadians);
            const Quat target = (pitch * roll).Normalized();
            orientation = Slerp(orientation, target, 1.0f - std::exp(-dt * 18.0f));
        } else {
            orientation = {};
        }

        MotionSample sample;
        sample.orientation = orientation;
        // The turn since the last step, in the pad's own frame.
        Quat delta = (before.Conjugate() * orientation).Normalized();
        if (delta.w < 0.0f) {
            delta = {-delta.w, -delta.x, -delta.y, -delta.z};
        }
        const float half = std::acos(std::clamp(delta.w, -1.0f, 1.0f));
        const float sin_half = std::sin(half);
        if (sin_half > 1e-6f) {
            const float rev_per_s = 2.0f * half / dt / (2.0f * std::numbers::pi_v<float>);
            sample.gyro = {delta.x / sin_half * rev_per_s, delta.y / sin_half * rev_per_s,
                           delta.z / sin_half * rev_per_s};
        }
        // Gravity, down in the world, as the pad sees it.
        sample.accel = orientation.Conjugate().Rotate({0.0f, 0.0f, -1.0f});

        // The shake swells in and dies out, so a short press is still a clear jolt.
        const bool shake_on = mode == MotionMode::On && shaking;
        shake_amount = std::clamp(shake_amount + (shake_on ? dt * 12.0f : -dt * 8.0f), 0.0f, 1.0f);
        if (shake_amount > 0.0f) {
            shake_phase = std::fmod(shake_phase + dt * ShakeHz, 1.0f);
            const float wave = std::sin(shake_phase * 2.0f * std::numbers::pi_v<float>);
            const float side = std::sin(shake_phase * 4.0f * std::numbers::pi_v<float> + 1.0f);
            // Mostly side to side, as an arm shakes a controller, with some up and down.
            sample.shake = {wave * shake_amount, 0.0f, side * 0.4f * shake_amount};
            sample.accel.x += ShakeG * sample.shake.x;
            sample.accel.z += ShakeG * sample.shake.z;
            // A wrist shake also turns it a little.
            sample.gyro.y += 1.5f * shake_amount *
                             std::cos(shake_phase * 2.0f * std::numbers::pi_v<float>);
        } else {
            shake_phase = 0.0f;
        }
        return sample;
    }

private:
    MotionMode mode{MotionMode::Off};
    Quat orientation;
    float shake_phase{};
    float shake_amount{};
};

/// What the on-screen controller shows, from the input thread.
struct MotionViewState {
    MotionMode mode{MotionMode::Off};
    MotionViewMode view{MotionViewMode::Always};
    Quat orientation;
    Vec3 shake;
    /// The player's real sticks, x right and y up in [-1, 1].
    float left_x{}, left_y{}, right_x{}, right_y{};
    /// Held buttons as VirtualButton bits (Switch names); `by_position` when the face buttons
    /// go by place, so the Switch A is the Xbox B.
    std::uint32_t buttons{};
    bool by_position{};
    /// The game's rumble, 0 to 1.
    float rumble{};
    /// Since the controller last moved, in milliseconds.
    std::uint32_t still_ms{};
    /// The in-game menu is open: the game, and so the controller, is paused.
    bool in_menu{};
};

/// VirtualButton's bits the picture uses (input_common/drivers/virtual_gamepad.h).
namespace MotionViewButton {
inline constexpr std::uint32_t A = 1u << 0, B = 1u << 1, X = 1u << 2, Y = 1u << 3;
inline constexpr std::uint32_t StickL = 1u << 4, StickR = 1u << 5;
inline constexpr std::uint32_t L = 1u << 6, R = 1u << 7, ZL = 1u << 8, ZR = 1u << 9;
inline constexpr std::uint32_t Plus = 1u << 10, Minus = 1u << 11;
inline constexpr std::uint32_t Left = 1u << 12, Up = 1u << 13, Right = 1u << 14, Down = 1u << 15;
} // namespace MotionViewButton

} // namespace Jadyx
