# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable integration guards for the UWP-only error applet wiring."""

import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


class ErrorAppletIntegrationTest(unittest.TestCase):
    def test_uwp_supplies_error_frontend(self):
        boot = (ROOT / "src/jadyx/app/uwp_boot.cpp").read_text()
        self.assertIn("applets.error =", boot)
        self.assertIn("bool nxbox_error_overlay{};", boot)
        self.assertIn('line == "nxbox_error_overlay=1"', boot)
        self.assertIn('line == "nxbox_error_overlay=0"', boot)
        self.assertIn(
            "Settings::values.error_applet_mode.SetValue(Settings::AppletMode::HLE)",
            boot,
        )
        self.assertIn("error_display->Stop()", boot)

    def test_guest_callback_does_not_capture_raw_owner(self):
        core = (ROOT / "src/core/hle/service/am/frontend/applet_error.cpp").read_text()
        self.assertNotIn("const auto callback = [this]", core)
        self.assertIn("weak_from_this()", core)

    def test_adapter_and_guest_lifetime(self):
        core = (ROOT / "src/core/hle/service/am/frontend/applet_error.cpp").read_text()
        callback = core[
            core.index("    const auto callback =") : core.index(
                "    const auto title_id ="
            )
        ]
        completion = core[
            core.index("void Error::DisplayCompleted()") : core.index(
                "Result Error::RequestExit()"
            )
        ]
        harness = (
            r"""
#include <cassert>
#include <memory>
#include <vector>
#include "jadyx/game/uwp_error_applet.h"
namespace Core::Frontend { ErrorApplet::~ErrorApplet() = default; }
using u8 = unsigned char;
struct IStorage { IStorage(int, std::vector<u8>) {} };
int output_count = 0;
int exit_count = 0;
class Error : public std::enable_shared_from_this<Error> {
public:
    std::function<void()> Callback() {
"""
            + callback
            + r"""
        return callback;
    }
    void DisplayCompleted();
    void PushOutData(std::shared_ptr<IStorage>) { ++output_count; }
    void Exit() { ++exit_count; }
    int system{};
    bool complete{};
};
"""
            + completion
            + r"""
int main() {
    auto display = std::make_shared<Jadyx::ErrorDisplay>();
    Jadyx::UwpErrorApplet applet{display};
    int completed = 0;
    const Result code{.module = 7, .description = 123};
    applet.ShowError(code, [&] { ++completed; });
    assert(display->Current()->code == "2007-0123");
    assert(display->Current()->message.empty());
    display->Dismiss(display->Current()->id);
    display->CompleteDismissed();
    applet.ShowErrorWithTimestamp(code, std::chrono::seconds{12345}, [&] { ++completed; });
    assert(display->Current()->code == "2007-0123");
    display->Dismiss(display->Current()->id);
    display->CompleteDismissed();
    applet.ShowCustomErrorText(code, "main", "detail", [&] { ++completed; });
    assert(display->Current()->message == "main\n\ndetail");
    applet.Close();
    display->CompleteDismissed();
    assert(completed == 3);
    applet.ShowCustomErrorText(code, "", "detail", {});
    assert(display->Current()->message == "detail");
    applet.Close();
    display->CompleteDismissed();
    auto guest = std::make_shared<Error>();
    auto callback = guest->Callback();
    callback(); callback();
    assert(output_count == 1 && exit_count == 1);
    guest.reset();
    callback(); // Actual production lambda must tolerate a destroyed guest.
    assert(output_count == 1 && exit_count == 1);
    guest = std::make_shared<Error>();
    callback = guest->Callback();
    applet.ShowError(code, callback);
    guest.reset();
    applet.Close();
    display->CompleteDismissed();
    assert(output_count == 1 && exit_count == 1);
}
"""
        )
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            stub = path / "core/hle/result.h"
            stub.parent.mkdir(parents=True)
            # Only Result's two accessors are stubbed; production applet headers/adapter are compiled.
            stub.write_text(
                "#pragma once\nstruct Result { unsigned module, description; "
                "unsigned GetModule() const { return module; } "
                "unsigned GetDescription() const { return description; } };\n"
            )
            (path / "test.cpp").write_text(harness)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wshadow",
                    "-Wconversion",
                    "-pthread",
                    "-I" + str(path),
                    "-I" + str(ROOT / "src"),
                    str(path / "test.cpp"),
                    str(ROOT / "src/jadyx/game/uwp_error_applet.cpp"),
                    "-o",
                    str(path / "test"),
                ],
                check=True,
            )
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
