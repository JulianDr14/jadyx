// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/app/warmup_screen.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <winrt/base.h>

#include "common/logging.h"
#include "core/core.h"
#include "jadyx/game/game_menu_surface.h"
#include "jadyx/input/controller_catalog.h"
#include "jadyx/input/uwp_controllers.h"
#include "jadyx/input/uwp_input.h"
#include "video_core/gpu.h"

namespace Jadyx {

struct WarmupScreen::Impl {
    Impl(Core::System& system_, WarmupControl& control_) : system{system_}, control{control_} {
        worker = std::jthread{[this](std::stop_token stop) { Run(stop); }};
    }
    ~Impl() {
        worker.request_stop();
        worker.join();
        // The boot coordinator destroys us only after shader/JIT callbacks are drained.
        try {
            system.GPU().RunOnGpuThread([this] { D3D12::HideGameMenu(system.Renderer()); });
        } catch (const std::exception& e) {
            LOG_WARNING(Frontend, "Warmup screen removal failed: {}", e.what());
        } catch (const winrt::hresult_error& e) {
            LOG_WARNING(Frontend, "Warmup screen removal failed: {}",
                        winrt::to_string(e.message()));
        }
    }

    void Run(std::stop_token stop) {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Uninitialize {
                ~Uninitialize() {
                    winrt::uninit_apartment();
                }
            } uninitialize;
            GameMenuSurface canvas;
            ControllerCatalog<ControllerDevice> controllers;
            const auto options = LoadControllerOptions();
            std::wstring selected;
            bool connected = false;
            const auto started = std::chrono::steady_clock::now();
            auto screen = D3D12::GameMenuScreenSize(system.Renderer());
            // A bounded CPU bitmap, centered on larger outputs like the Pulse screen.
            screen.first = std::min(screen.first, u32{1920});
            screen.second = std::min(screen.second, u32{1080});
            while (!stop.stop_requested()) {
                const auto now = std::chrono::steady_clock::now();
                try {
                    if (controllers.Update(now, EnumerateControllers, SameControllerList)) {
                        connected = false;
                        control.Disconnect();
                    }
                    if (const auto* pad = controllers.Select(options.controller_id)) {
                        if (!connected || selected != pad->id)
                            control.Disconnect();
                        selected = pad->id;
                        connected = true;
                        using B = winrt::Windows::Gaming::Input::GamepadButtons;
                        const bool swap =
                            SwapControllerFaceButtons(*pad, options.swap_face_buttons);
                        const auto buttons =
                            MapFaceButtons(ReadController(*pad).gamepad.Buttons, swap);
                        control.Poll((buttons & B::X) != B::None, (buttons & B::B) != B::None);
                        canvas.SetPromptMapping(
                            static_cast<u8>((swap ? 1 : 0) | (pad->nintendo ? 2 : 0)));
                    } else {
                        connected = false;
                        control.Disconnect();
                    }
                } catch (const winrt::hresult_error&) {
                    controllers.ReadFailed();
                    connected = false;
                    control.Disconnect();
                }
                if (QuitRequested())
                    control.Cancel();
                WarmupView frame;
                {
                    std::scoped_lock lock{mutex};
                    frame = view;
                }
                frame.seconds = static_cast<unsigned>(
                    std::chrono::duration_cast<std::chrono::seconds>(now - started).count());
                frame.stopping = control.Token().stop_requested();
                frame.cancelled = control.Cancelled();
                auto image = canvas.DrawWarmup(frame, screen);
                if (!image.pixels.empty()) {
                    system.GPU().RunOnGpuThread(
                        [&] { D3D12::ShowGameMenu(system.Renderer(), std::move(image)); });
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{33});
            }
        } catch (const winrt::hresult_error& e) {
            LOG_WARNING(Frontend, "Warmup screen stopped: {}", winrt::to_string(e.message()));
        } catch (const std::exception& e) {
            LOG_WARNING(Frontend, "Warmup screen stopped: {}", e.what());
        }
    }

    Core::System& system;
    WarmupControl& control;
    std::mutex mutex;
    WarmupView view;
    std::jthread worker; // Last: joined before counters/control/canvas can die.
};

WarmupScreen::WarmupScreen(Core::System& system, WarmupControl& control)
    : impl{std::make_unique<Impl>(system, control)} {}
WarmupScreen::~WarmupScreen() = default;
void WarmupScreen::Stage(WarmupPhase phase) {
    std::scoped_lock lock{impl->mutex};
    impl->view.phase = phase;
    impl->view.done = 0;
    impl->view.total = 0;
}
void WarmupScreen::Progress(std::size_t done, std::size_t total) {
    std::scoped_lock lock{impl->mutex};
    impl->view.done = done;
    impl->view.total = total;
}

} // namespace Jadyx
