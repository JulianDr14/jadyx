// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// ============================================================================================
// UWP entry point: a CoreApplication IFrameworkView whose Run() drives RunHeadlessBoot() against the
// homebrew NRO bundled in the package install location (Package.InstalledLocation\boot.nro). Reading
// the fixed GATE-2 payload from the read-only install dir keeps the MSIX self-contained â€” no
// Device-Portal file-push or LocalState chicken-and-egg. (Eden's log still writes to the writable
// LocalFolder; see common/fs/path_util.cpp under YUZU_UWP_APPCONTAINER.)
// ============================================================================================
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

#include <windows.h> // OutputDebugStringA/W + ::Sleep (sets the target-arch macros winnt.h needs)

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include "common/fs/path_util.h"
#include "common/host_memory.h"
#include "common/permafrost.h"
#include "jadyx/app/alloc_watch.h"
#include "jadyx/app/boot_config.h"
#include "jadyx/app/boot_crash.h"
#include "jadyx/app/boot_diag.h"
#include "jadyx/app/boot_session.h"
#include "jadyx/app/boot_state.h"
#include "jadyx/app/user_data_seed.h"
#include "jadyx/i18n/localization.h"
#include "jadyx/input/keyboard_bindings.h"
#include "jadyx/input/uwp_controllers.h"
#include "jadyx/input/uwp_input.h"
#include "jadyx/library/game_library.h"
#include "jadyx/library/uwp_library.h"
#include "jadyx/pulse/pulse_sessions.h"
#include "jadyx/settings/uwp_system_settings.h"
#include "jadyx/storage/storage_path.h"
#include "jadyx/storage/utf8.h"
#include "jadyx/ui/launch_splash.h"
#include "video_core/frame_trace.h"

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;

namespace Jadyx {

namespace {

struct LaunchConfig {
    bool show_library = false;
    std::string library_pick;
    bool output_4k = false;
    bool output_1080p = false;
    std::optional<Jadyx::Upscaling> upscaling;
    std::optional<bool> frame_gen;
};

/// What one session runs and where it presents.
struct LaunchChoice {
    std::optional<std::string> library_game;
    bool show_library = false;
    u64 library_program_id = 0;
    Jadyx::BootSurface game_surface;
};

/// Runs the self-tests boot.cfg asks for at launch. True when it turns the developer hotkeys on.
bool ScanBootConfigEarly() {
    bool developer_hotkeys = false;
    try {
        const auto install = std::filesystem::path{
            Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str()};
        std::ifstream cfg{install / L"boot.cfg"};
        for (std::string line; std::getline(cfg, line);)
            if (line == "developer_hotkeys=1") developer_hotkeys = true;
            else if (line == "pro_hid_probe=1") {
                WriteDiag(Jadyx::RunProControllerDecoderSelfTest() ? "Pro HID decoder self-test PASS" : "Pro HID decoder self-test FAIL");
                Jadyx::ProbeProControllerHid([](std::string message) { WriteDiag(message); });
            }
            else if (line == "keyboard_self_test=1") {
                WriteDiag(Jadyx::RunKeyboardInputSelfTest() ? "keyboard self-test PASS" : "keyboard self-test FAIL");
            }
    } catch (...) {}
    return developer_hotkeys;
}

/// A key while the library is not up.
void HandleGameKey(Windows::System::VirtualKey key, bool pressed, bool developer_hotkeys) {
    using Windows::System::VirtualKey;
    if (Jadyx::TextEntryActive()) {
        // A game's keyboard is up: keys edit its text (characters arrive through
        // CharacterReceived) and nothing reaches the game.
        using Command = Jadyx::TextEntry::Command;
        std::optional<Command> command;
        switch (key) {
        case VirtualKey::Escape: command = Command::Cancel; break;
        case VirtualKey::Enter: command = Command::Done; break;
        case VirtualKey::Back: command = Command::Delete; break;
        case VirtualKey::Up: command = Command::Up; break;
        case VirtualKey::Down: command = Command::Down; break;
        case VirtualKey::Left: command = Command::Left; break;
        case VirtualKey::Right: command = Command::Right; break;
        default: break;
        }
        if (pressed && command) Jadyx::PushTextEntryKey({.command = command});
        return;
    }
    if (developer_hotkeys && key == VirtualKey::Q) {
        if (pressed) Jadyx::RequestQuit();
        return;
    }
    if (developer_hotkeys && key == VirtualKey::T) {
        if (pressed) VideoCore::FrameTrace::Start(480);
        return;
    }
    if (developer_hotkeys && key == VirtualKey::F6) {
        if (pressed) D3D12::TraceNextFrames(1);
        return;
    }
    if (developer_hotkeys && key == VirtualKey::F7) {
        if (pressed) D3D12::TraceNextMaterialFrame();
        return;
    }
    if (developer_hotkeys && key == VirtualKey::F8) {
        if (pressed) D3D12::HuntNextFrames();
        return;
    }
    if (key == VirtualKey::Escape) {
        if (pressed) Jadyx::RequestGameMenu();
        return;
    }
    Jadyx::SetKeyboardKey(static_cast<unsigned>(key), pressed);
}

Jadyx::BootSurface ReadWindowSurface(const CoreWindow& window) {
    // The swapchain is sized in physical pixels: CoreWindow bounds are in view pixels (DIPs).
    Jadyx::BootSurface surface{};
    if (window) {
        surface.core_window = winrt::get_abi(window); // an IInspectable, so a valid IUnknown*
        try {
            const auto bounds = window.Bounds();
            const double scale = Windows::Graphics::Display::DisplayInformation::
                                     GetForCurrentView()
                                         .RawPixelsPerViewPixel();
            surface.width = static_cast<u32>(bounds.Width * scale + 0.5);
            surface.height = static_cast<u32>(bounds.Height * scale + 0.5);
        } catch (...) {
            WriteDiag("could not read the window size, assuming 1920x1080");
        }
    }
    return surface;
}

/// What boot.cfg says about the library and the output size, read again for each session.
LaunchConfig ReadLaunchConfig(const std::filesystem::path& install) {
    LaunchConfig cfg;
    std::ifstream file{install / L"boot.cfg"};
    for (std::string line; std::getline(file, line);) {
        if (line == "library=1") cfg.show_library = true;
        else if (line.starts_with("library_pick=")) cfg.library_pick = line.substr(13);
        else if (line == "output=4k") cfg.output_4k = true;
        else if (line == "output=1080p") cfg.output_1080p = true;
        // The same keys the boot reads (BootConfig), here only for the output size.
        else if (line.starts_with("upscaling=")) cfg.upscaling = Jadyx::ParseUpscaling(line.substr(10));
        else if (line == "frame_gen=off") cfg.frame_gen = false;
        else if (line == "frame_gen=auto" || line == "frame_gen=30" || line == "frame_gen=60")
            cfg.frame_gen = true;
    }
    return cfg;
}

// The swapchain the size of the TV's current mode (3840x2160 on a 4K TV); the
// window's own size (1920x1080 on the console) when the mode is not larger.
Jadyx::BootSurface ScreenSurface(const Jadyx::BootSurface& surface, u32 session) {
    Jadyx::BootSurface screen = surface;
    if (surface.core_window == nullptr) return screen;
    try {
        const auto hdmi = Windows::Graphics::Display::Core::HdmiDisplayInformation::
            GetForCurrentView();
        const auto mode = hdmi ? hdmi.GetCurrentDisplayMode() : nullptr;
        if (mode && mode.ResolutionWidthInRawPixels() > surface.width) {
            screen.width = mode.ResolutionWidthInRawPixels();
            screen.height = mode.ResolutionHeightInRawPixels();
        } else if (session == 1) {
            WriteDiag("output: the display mode is not larger than " +
                      std::to_string(surface.width) + "x" + std::to_string(surface.height));
        }
    } catch (const winrt::hresult_error& e) {
        WriteDiag("output: no display mode: " + winrt::to_string(e.message()));
    }
    return screen;
}

// The game's graphics (its own over Settings > Graphics, as ApplyGameGraphics
// resolves them once it is loaded) pick the swapchain, which exists before the
// game does: FSR presents at the TV's resolution, frame generation and the
// bilinear blit at 1080p. A game the library did not name follows Settings.
Jadyx::BootSurface ChooseGameSurface(const Jadyx::BootSurface& surface, const Jadyx::BootSurface& library_surface,
                                     const LaunchConfig& cfg, u64 library_program_id, u32 session) {
    Jadyx::BootSurface game_surface = surface;
    if (cfg.output_4k || cfg.output_1080p) {
        game_surface = library_surface;
        game_surface.screen_width = game_surface.plain_width = library_surface.width;
        game_surface.screen_height = game_surface.plain_height = library_surface.height;
    } else {
        Jadyx::GraphicsSettings graphics = Jadyx::ResolveGraphics(
            Jadyx::LoadGraphicsSettings(),
            library_program_id != 0 ? Jadyx::LoadGameGraphics(library_program_id) : Jadyx::GameGraphics{});
        if (cfg.upscaling) graphics.upscaling = *cfg.upscaling;
        if (cfg.frame_gen) graphics.frame_gen = *cfg.frame_gen;
        const Jadyx::BootSurface screen = ScreenSurface(surface, session);
        game_surface.plain_width = surface.width;
        game_surface.plain_height = surface.height;
        game_surface.screen_width = screen.width;
        game_surface.screen_height = screen.height;
        if (Jadyx::UsesScreenResolution(graphics)) {
            game_surface.width = screen.width;
            game_surface.height = screen.height;
        } else {
            game_surface.width = surface.width;
            game_surface.height = surface.height;
        }
    }
    return game_surface;
}

/// Reads boot.cfg, shows the library when it asks for one and sizes the game's swapchain. False
/// when the app ends here.
bool ChooseLaunch(const Jadyx::BootSurface& surface, u32 session, std::string& library_error,
                  bool& library_active, LaunchChoice& choice) {
    try {
        const auto install = std::filesystem::path{
            Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str()};
        const LaunchConfig cfg = ReadLaunchConfig(install);
        choice.show_library = cfg.show_library;

        // boot.cfg output=4k / output=1080p force a size on the library and the game, for
        // test runs. 1080p is a fixed swapchain on high-DPI PC screens; the present
        // stretches it.
        Jadyx::BootSurface library_surface = surface;
        if (cfg.output_4k) library_surface = ScreenSurface(surface, session);
        if (cfg.output_1080p) library_surface = {surface.core_window, 1920, 1080};

        if (cfg.show_library) {
            if (session > 1) {
                WriteDiag("library: back from the game | " + MemoryReport());
                WriteDiag("heaps: " + HeapReport() + " | " + MemoryReport());
                WriteDiag("memory map: " + LargestAllocations());
            }
            library_active = true;
            const auto local = std::filesystem::path{
                Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()};
            WriteDiag("library: scanning LocalState/games; waiting for user selection");
            choice.library_game = Jadyx::ShowGameLibrary(surface.core_window, library_surface.width,
                library_surface.height, local / L"games", [install, local] { SeedUserData(install, local); },
                session <= 2 && library_error.empty() ? std::string_view{cfg.library_pick} : std::string_view{},
                library_error, &choice.library_program_id);
            library_active = false;
            if (!choice.library_game) {
                WriteDiag("library: closed by user");
                return false;
            }
            WriteDiag("library: selected " + *choice.library_game);
            library_error.clear();
            // The window is never left blank while the game loads.
            Jadyx::ShowLaunchSplash(surface.core_window, surface.width, surface.height);
        }

        choice.game_surface = ChooseGameSurface(surface, library_surface, cfg, choice.library_program_id, session);
        char program[17];
        std::snprintf(program, sizeof(program), "%016llX",
                      static_cast<unsigned long long>(choice.library_program_id));
        WriteDiag("output: " + std::to_string(choice.game_surface.width) + "x" +
                  std::to_string(choice.game_surface.height) +
                  (cfg.output_4k || cfg.output_1080p ? std::string{" (boot.cfg)"}
                                                     : std::string{" (graphics of "} + program + ")"));
    } catch (const std::exception& e) {
        WriteDiag(std::string("library: failed: ") + e.what());
        return false;
    } catch (const winrt::hresult_error& e) {
        // The code first: the message can be a stale error the platform recorded earlier.
        char code[16];
        std::snprintf(code, sizeof(code), "%08X", static_cast<unsigned>(e.code().value));
        WriteDiag(std::string("library: failed: ") + code + " " + winrt::to_string(e.message()));
        return false;
    }
    return true;
}

/// The boot worker: runs one session on its own thread, since RunHeadlessBoot blocks.
void RunBootWorker(const Jadyx::BootSurface& surface, const std::optional<std::string>& library_game,
                   u64 library_program_id, std::atomic<bool>& done, std::atomic<int>& boot_status) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::set_terminate(OnTerminate); // per-thread in the MSVC runtime
    std::string nro_path;
    Jadyx::BootConfig config{};
    try {
        Jadyx::UseGameAdvancedGraphics(config, library_program_id);
        // Bundled NRO from the read-only package install location.
        const std::string install_path = winrt::to_string(
            Windows::ApplicationModel::Package::Current().InstalledLocation().Path());
        nro_path = install_path + "\\boot.nro";
        WriteDiag("resolved NRO path: " + nro_path);
        ParseBootConfigFile(install_path, config);
        const std::string local_path = winrt::to_string(
            Windows::Storage::ApplicationData::Current().LocalFolder().Path());
        SeedUserData(std::filesystem::path{winrt::to_hstring(install_path).c_str()},
                     std::filesystem::path{winrt::to_hstring(local_path).c_str()});
        if (library_game) {
            config.play = true;
            config.run_seconds = 0;
            config.pulse_sessions = *library_game != Jadyx::MiiEditorLaunch;
        }
        if (library_game || !config.game.empty()) {
            // A game= path on the player's storage (jadyx-usb:/...) opens as in the library.
            nro_path = library_game                       ? *library_game
                       : Jadyx::IsStoragePath(config.game) ? config.game
                                                           : local_path + "\\games\\" + config.game;
            if (config.run_seconds == 0 && !config.play) {
                config.run_seconds = Jadyx::DEFAULT_GAME_RUN_SECONDS;
            }
            WriteDiag("resolved game path: " + nro_path + ", running " +
                      (config.play ? std::string("until closed")
                                   : std::to_string(config.run_seconds) + " s"));
        }
    } catch (...) {
        WriteDiag("FAILED resolving Package.InstalledLocation");
    }
    ApplyProcessMemoryLimit(config.memory_limit_mib);
    // Capture any early throw to the diag file instead of a silent exit.
    try {
        WriteDiag("calling RunHeadlessBoot");
        int rc = Jadyx::RunHeadlessBoot(nro_path, surface, config);
        if (rc == Jadyx::PULSE_RESUME_FAILED) {
            Jadyx::PulseSessions::SetNotice(Jadyx::I18n::ToUtf8(
                Jadyx::I18n::Tr(Jadyx::I18n::Text::PulseNotResumed)));
            WritePulseLog("booting the game fresh after a failed resume");
            config.pulse_resume = false;
            rc = Jadyx::RunHeadlessBoot(nro_path, surface, config);
        }
        boot_status.store(rc);
        WriteDiag("RunHeadlessBoot returned " + std::to_string(rc) + " | " + MemoryReport());
    } catch (winrt::hresult_error const& e) {
        WriteDiag("winrt::hresult_error: " + winrt::to_string(e.message()));
    } catch (std::exception const& e) {
        WriteDiag(std::string("std::exception: ") + e.what());
    } catch (...) {
        WriteDiag("unknown exception in RunHeadlessBoot");
    }
    done.store(true);
}

/// PC: publishes the window mode for the in-game menu and applies the one it asked for. The
/// choice is also kept as the launch mode. The swapchain keeps its size and is stretched.
void ApplyWindowMode() {
    using Windows::UI::ViewManagement::ApplicationView;
    using Windows::UI::ViewManagement::ApplicationViewWindowingMode;
    static const bool xbox = Windows::System::Profile::AnalyticsInfo::VersionInfo()
                                 .DeviceFamily() == L"Windows.Xbox";
    if (xbox) return;
    try {
        const auto view = ApplicationView::GetForCurrentView();
        if (const int request = g_fullscreen_request.exchange(-1); request >= 0) {
            if (request == 1) {
                const bool entered = view.TryEnterFullScreenMode();
                WriteDiag(entered ? "window: full screen" : "window: full screen refused");
            } else {
                view.ExitFullScreenMode();
                WriteDiag("window: windowed");
            }
            ApplicationView::PreferredLaunchWindowingMode(
                request == 1 ? ApplicationViewWindowingMode::FullScreen
                             : ApplicationViewWindowingMode::Auto);
        }
        g_fullscreen.store(view.IsFullScreenMode());
        g_can_fullscreen.store(true);
    } catch (...) {
        g_can_fullscreen.store(false);
    }
}

/// Keeps the UI thread responsive while the worker runs.
void PumpUntilDone(const CoreWindow& window, const std::atomic<bool>& done) {
    // Heartbeat every 10 s: if steps stop but heartbeats continue, the boot hung rather than
    // crashed, and the last step names where. The first 30 s also log the memory every
    // second, since a boot can run out of commit before the first heartbeat.
    CoreDispatcher dispatcher = window.Dispatcher();
    const ULONGLONG boot_start = GetTickCount64();
    ULONGLONG next_heartbeat = boot_start + 10'000;
    ULONGLONG next_boot_memory = boot_start + 1'000;
    while (!done.load()) {
        Jadyx::RefreshProControllers();
        ApplyWindowMode();
        dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        ::Sleep(50);
        if (const ULONGLONG now = GetTickCount64();
            now >= next_boot_memory && now < boot_start + 30'000 && now < next_heartbeat) {
            WriteDiag("boot memory: " + MemoryReport());
            next_boot_memory = now + 1'000;
        }
        if (GetTickCount64() >= next_heartbeat) {
            WriteDiag("heartbeat: boot worker still running | " + MemoryReport());
            if (const u64 visited = g_host_trim_visited.exchange(0); visited != 0) {
                WriteDiag("host trim: released " +
                          std::to_string(g_host_trim_released.exchange(0) >> 20) +
                          " MiB of " + std::to_string(visited >> 20) + " MiB visited");
            }
            if (Common::GuestSwapActive()) {
                WriteDiag("permafrost: released " +
                          std::to_string(g_guest_swap_released.exchange(0) >> 20) +
                          " MiB since the last heartbeat | " + Common::DescribeGuestSwap() +
                          " | coordinator: " + g_memory_coordinator_state.Describe());
            }
            WriteDiag("memory by owner: " + MemoryOwners());
            for (const auto& capture : Jadyx::AllocWatch::TakeNew()) {
                WriteDiag(FormatFrames(
                    ("alloc watch: " + std::to_string(capture.size) + " bytes from").c_str(),
                    capture.frames, capture.frame_count));
            }
            next_heartbeat += 10'000;
        }
    }
}

/// What the library shows when a session ended with this status; nullopt when the app ends instead.
std::optional<std::string> LibraryNotice(int status) {
    using I18n::Text;
    const auto text = [](Text id) { return Jadyx::I18n::ToUtf8(Jadyx::I18n::Tr(id)); };
    switch (status) {
    case 2:
        WriteDiag("boot load failed; returning to library for recovery");
        return text(Text::GameLoadFailed);
    case Jadyx::UPDATE_REQUIRED:
        WriteDiag("game has no ExeFS without its update; returning to library");
        return text(Text::GameNeedsUpdate);
    case 16:
        WriteDiag("content/storage failure handled; returning to library");
        return text(Text::ContentUnavailable);
    case Jadyx::MII_EDITOR_MISSING:
        WriteDiag("mii editor missing; returning to library");
        return text(Text::MiiEditorMissing);
    case 15:
        WriteDiag("GPU failure handled; returning to library for recovery");
        return text(Text::GameStoppedGpu);
    case Jadyx::RETURN_TO_LIBRARY:
        WriteDiag("boot worker joined; back to the library");
        return Jadyx::PulseSessions::TakeNotice();
    default:
        return std::nullopt;
    }
}

struct BootView : implements<BootView, IFrameworkViewSource, IFrameworkView> {
    IFrameworkView CreateView() {
        return *this;
    }


    void Initialize(CoreApplicationView const&) {}
    void SetWindow(CoreWindow const& window) {
        m_window = window;
    }
    void Load(hstring const&) {}
    void Uninitialize() {}

    void Run() {
        WriteDiag("BootView::Run entered"); // also resolves + caches the diag path
        WriteDiag(MemoryReport());
        WriteDiag(StorageReport());
        InstallCrashHandlers();

        // A UWP app MUST activate its CoreWindow and pump the dispatcher, or the OS terminates it a
        // couple seconds after launch (no crash, no dump - exactly the "flashes then closes" symptom).
        // The hello-world/triangle apps survive because they render (activate + pump); this headless
        // boot did neither. Activate the (blank, Null-renderer) window, run the blocking boot on a
        // worker thread, and keep the UI thread pumping so the OS sees an activated, responsive app.
        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();
        // On Xbox the gamepad's B also raises "back"; left unhandled at the root it can send the
        // app to the background. B is a game button here (uwp_input.h).
        SystemNavigationManager::GetForCurrentView().BackRequested(
            [](auto&&, BackRequestedEventArgs const& args) { args.Handled(true); });
        bool library_active = false;
        const bool developer_hotkeys = ScanBootConfigEarly();
        const auto on_key = [&library_active, developer_hotkeys](Windows::System::VirtualKey key, bool pressed) {
            if (library_active) return;
            HandleGameKey(key, pressed, developer_hotkeys);
        };
        auto focus = window.Activated(winrt::auto_revoke, [](auto&&, WindowActivatedEventArgs const& args) {
            if (args.WindowActivationState() != CoreWindowActivationState::Deactivated) return;
            Jadyx::ResetKeyboardKeys();
            // On the console the Xbox button's guide takes the focus: the game pauses in its menu.
            Jadyx::RequestGameMenuOpen();
        });
        window.KeyDown([on_key](auto&&, KeyEventArgs const& args) {
            if (!args.KeyStatus().WasKeyDown) on_key(args.VirtualKey(), true);
            args.Handled(true);
        });
        window.KeyUp([on_key](auto&&, KeyEventArgs const& args) {
            on_key(args.VirtualKey(), false);
            args.Handled(true);
        });
        window.CharacterReceived([](auto&&, CharacterReceivedEventArgs const& args) {
            if (!Jadyx::TextEntryActive()) return;
            const auto c = static_cast<wchar_t>(args.KeyCode());
            if (c >= 0x20 && c != 0x7F) Jadyx::PushTextEntryKey({.character = c});
            args.Handled(true);
        });
        // Raw relative motion (not the cursor, which stops at the screen's edge): the input thread
        // turns it into the right stick when the player chose the mouse for it.
        Windows::Devices::Input::MouseDevice::MouseMoved_revoker mouse_moved;
        try {
            mouse_moved = Windows::Devices::Input::MouseDevice::GetForCurrentView().MouseMoved(
                winrt::auto_revoke, [&library_active](auto&&, Windows::Devices::Input::MouseEventArgs const& args) {
                    if (library_active) return;
                    const auto delta = args.MouseDelta();
                    Jadyx::AddMouseMotion(delta.X, delta.Y);
                });
            WriteDiag("input: mouse motion events on");
        } catch (const winrt::hresult_error& e) {
            WriteDiag("input: no mouse motion events: " + winrt::to_string(e.message()));
        }

        const Jadyx::BootSurface surface = ReadWindowSurface(m_window);

        // "Back to the library" in the in-game menu shuts the game down inside this process and
        // comes back here: restarting the app is slow and awkward on the console.
        std::string library_error;
        for (u32 session = 1;; ++session) {
            LaunchChoice choice;
            if (!ChooseLaunch(surface, session, library_error, library_active, choice)) return;

            std::atomic<bool> done{false};
            std::atomic<int> boot_status{0};
            std::thread worker([&done, &boot_status, &choice] {
                RunBootWorker(choice.game_surface, choice.library_game, choice.library_program_id, done, boot_status);
            });
            PumpUntilDone(window, done);
            worker.join();
            // Still up when the boot ended before its renderer took the window; the library needs it.
            Jadyx::ReleaseLaunchSplash();
            if (choice.show_library) {
                if (auto notice = LibraryNotice(boot_status.load())) {
                    library_error = std::move(*notice);
                    continue;
                }
            }
            WriteDiag("boot worker joined; exiting");
            break;
        }
    }

private:
    CoreWindow m_window{nullptr};
};

} // namespace

} // namespace Jadyx

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    winrt::init_apartment();
    CoreApplication::Run(winrt::make<Jadyx::BootView>());
    return 0;
}
