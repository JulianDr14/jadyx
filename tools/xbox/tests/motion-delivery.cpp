// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "common/input.h"
#include "hid_core/frontend/input_converter.h"
#include "hid_core/frontend/motion_input.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/input_poller.h"
#include "jadyx/input/synthetic_motion.h"

namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned int, const char*, fmt::string_view,
                       const fmt::format_args&) {}
} // namespace Common::Log
void AssertFailSoftImpl() {
    std::abort();
}
void AssertFatalImpl() {
    std::abort();
}

int main() {
    using namespace std::chrono_literals;
    auto engine = std::make_shared<InputCommon::VirtualGamepad>("virtual_gamepad");
    InputCommon::InputFactory factory{engine};
    std::array<std::unique_ptr<Common::Input::InputDevice>, 4> devices;
    std::array<Core::HID::MotionInput, 4> filters;
    std::array<unsigned int, 4> callbacks{};
    std::array<Common::Vec<float, 3>, 4> delivered{};
    for (std::size_t index = 0; index < 4; ++index) {
        // Same engine/port/motion parameters as LoadVirtualGamepadParams. Both
        // hands use sensor 0; the real poller's default deadband is 0.007
        // turns/second.
        Common::ParamPackage mapping{
            {"engine", "virtual_gamepad"}, {"port", index < 2 ? "0" : "8"}, {"motion", "0"}};
        devices[index] = factory.Create(mapping);
        devices[index]->SetCallback({[&, index](const Common::Input::CallbackStatus& status) {
            const auto raw = Core::HID::TransformToMotion(status);
            assert(raw.delta_timestamp > 0 && raw.delta_timestamp <= 100000);
            assert(std::abs(raw.gyro.x.properties.threshold - .007f) < .000001f);
            auto& filter = filters[index];
            filter.SetAcceleration({raw.accel.x.value, raw.accel.y.value, raw.accel.z.value});
            filter.SetGyroscope({raw.gyro.x.value, raw.gyro.y.value, raw.gyro.z.value});
            filter.SetUserGyroThreshold(raw.gyro.x.properties.threshold);
            filter.UpdateRotation(raw.delta_timestamp);
            filter.UpdateOrientation(raw.delta_timestamp);
            delivered[index] = filter.GetGyroscope();
            ++callbacks[index];
        }});
    }
    Jadyx::SyntheticMotion motion;
    const auto start = Jadyx::StickMotion::Clock::time_point{};
    for (int frame = 0; frame < 420; ++frame) {
        Jadyx::StylePad pad{.buttons = 1U << Jadyx::PadBit::LStick,
                            .right_x = frame < 240 ? 1.0f : 0.0f,
                            .right_y = frame < 240 ? 0.0f : 1.0f};
        const auto sample = motion.Filter(pad, true, true, false, start + frame * 16667us);
        for (const std::size_t port : {0U, 8U}) {
            engine->SetMotionState(port, sample.delta_timestamp, sample.gyro[0], sample.gyro[1],
                                   sample.gyro[2], sample.accel[0], sample.accel[1],
                                   sample.accel[2]);
        }
        for (std::size_t index = 0; index < 4; ++index) {
            assert(callbacks[index] == static_cast<unsigned int>(frame + 1));
            const auto gyro = delivered[index];
            if (frame < 240)
                assert(std::abs(gyro[2] + .5f) < .0001f);
            else
                assert(std::abs(gyro[0] - .5f) < .0001f);
        }
    }
    const auto before_extra = callbacks;
    engine->SetMotionState(1, 4000, .5f, 0, 0, 0, 0, -1);
    assert(callbacks == before_extra); // Player two cannot feed player one/handheld.
    for (const std::size_t port : {0U, 8U})
        engine->SetMotionState(port, 4000, 0, 0, 0, 0, 0, -1);
    for (const auto& gyro : delivered)
        assert(gyro.Length() == 0);
    std::puts("motion delivery: 420 repeated yaw/pitch samples, both "
              "hands/ports, isolation PASS");
}
