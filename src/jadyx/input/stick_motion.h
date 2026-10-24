// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace Jadyx {

struct StickMotionSample {
    std::array<float, 3> accel{0.0f, 0.0f, -1.0f};
    std::array<float, 3> gyro{};
    std::uint64_t delta_timestamp{16667};
    bool suppress_sticks{};
    bool guest_l3{};
};

// Portable synthesis in g and turns/second, matching HID's SDL sensor axes.
class StickMotion {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr float kPi = 3.14159265358979323846f;
    static constexpr float kMaxTilt = kPi / 3.0f;
    static constexpr float kAimTurnsPerSecond = 0.5f;
    static constexpr float kTiltTurnsPerSecond = 1.0f;
    static constexpr float kDeadzone = 0.15f;

    StickMotionSample Update(bool sensor_enabled, bool modifier, float left_x, float left_y,
                             float right_x, float right_y, Clock::time_point now,
                             bool blocked = false) {
        const float elapsed =
            initialized ? std::chrono::duration<float>(now - last).count() : 1.0f / 60.0f;
        const float dt = std::clamp(elapsed, 0.000001f, 0.1f);
        initialized = true;
        last = now;
        StickMotionSample sample;
        sample.delta_timestamp = static_cast<std::uint64_t>(dt * 1000000.0f);
        const bool moved = std::abs(left_x) > kDeadzone || std::abs(left_y) > kDeadzone ||
                           std::abs(right_x) > kDeadzone || std::abs(right_y) > kDeadzone;
        if (!sensor_enabled) {
            pending = motion_hold = cancelled = false;
            pulse_until = now;
            sample.guest_l3 = modifier;
        } else if (blocked) {
            cancelled = modifier;
            pending = motion_hold = false;
            pulse_until = now;
        } else {
            if (modifier && !was_l3) {
                pressed_at = now;
                pulse_until = now;
                pending = true;
                motion_hold = false;
            }
            // A held button crossing into sensor mode cannot become a tap on release.
            if (modifier && !pending && !motion_hold)
                cancelled = true;
            if (pending && modifier &&
                (moved || now - pressed_at >= std::chrono::milliseconds(250))) {
                pending = false;
                motion_hold = true;
            }
            if (!modifier && was_l3) {
                if (pending && !cancelled && !moved &&
                    now - pressed_at < std::chrono::milliseconds(250)) {
                    pulse_until = now + std::chrono::milliseconds(40);
                    ++tap_count;
                }
                pending = motion_hold = cancelled = false;
            }
            sample.guest_l3 = now < pulse_until && !modifier;
        }
        was_l3 = modifier;
        modifier_active = sensor_enabled && modifier && motion_hold && !blocked && !cancelled;
        sample.suppress_sticks = modifier_active;
        if (!sample.suppress_sticks) {
            tilt_roll = tilt_pitch = aim_pitch = 0.0f;
            return sample;
        }
        const float old_roll = tilt_roll;
        const float old_pitch = tilt_pitch;
        const float step = kTiltTurnsPerSecond * 2.0f * kPi * dt;
        tilt_roll += std::clamp(Axis(left_x) * kMaxTilt - tilt_roll, -step, step);
        tilt_pitch += std::clamp(Axis(left_y) * kMaxTilt - tilt_pitch, -step, step);
        const float pitch_rate = Axis(right_y) * kAimTurnsPerSecond;
        const float yaw_rate = -Axis(right_x) * kAimTurnsPerSecond;
        aim_pitch = std::remainder(aim_pitch + pitch_rate * 2.0f * kPi * dt, 2.0f * kPi);
        const float pitch = tilt_pitch + aim_pitch;
        // Gravity in controller space: R_forward(roll) * R_right(pitch).
        sample.accel = {std::sin(tilt_roll), -std::cos(tilt_roll) * std::sin(pitch),
                        -std::cos(tilt_roll) * std::cos(pitch)};
        const float roll_rate = (tilt_roll - old_roll) / (dt * 2.0f * kPi);
        // Yaw is about world up, expressed in controller space using gravity.
        sample.gyro = {pitch_rate + (tilt_pitch - old_pitch) / (dt * 2.0f * kPi),
                       roll_rate * std::cos(pitch), -roll_rate * std::sin(pitch)};
        for (std::size_t axis = 0; axis < sample.gyro.size(); ++axis) {
            sample.gyro[axis] -= sample.accel[axis] * yaw_rate;
        }
        return sample;
    }

    bool IsModifierActive() const {
        return modifier_active;
    }
    std::uint64_t TapCount() const {
        return tap_count;
    }

private:
    Clock::time_point pressed_at{}, pulse_until{};
    std::uint64_t tap_count{};
    bool was_l3{}, pending{}, motion_hold{}, cancelled{}, modifier_active{};

    static float Axis(float value) {
        const float magnitude =
            std::clamp((std::abs(value) - kDeadzone) / (1.0f - kDeadzone), 0.0f, 1.0f);
        return std::copysign(magnitude, value);
    }

    Clock::time_point last{};
    float tilt_roll{}, tilt_pitch{}, aim_pitch{};
    bool initialized{};
};

} // namespace Jadyx
