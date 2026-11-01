// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Headless GATE-2 boot frontend for the Xbox/UWP AppContainer target.
//
// Brings up Core::System with the D3D12 renderer + native XAudio2 output, loads a payload staged in the
// app's sandboxed local storage, runs it through the dynarmic JIT, and emits a deterministic
// JIT-liveness marker. Input is the Xbox gamepad and/or a boot.cfg script
// (uwp_input.h).
//
// The Core::System bring-up is modeled on the proven desktop boot in src/yuzu_cmd/yuzu.cpp; the
// WinRT IFrameworkView wrapper is the UWP entry point that drives it.
//
// JIT-LIVENESS CONTRACT (agreed with AGENT QA for the GATE-2 checklist): GATE 2 means "Eden executed
// guest code via the JIT", not "the process didn't crash". The homebrew NRO (NO keys/firmware/ROM â€”
// house rule) issues svcOutputDebugString with the exact sentinel below; Eden's SVC handler logs
// OutputDebugString, so observing this line is positive proof the JIT decoded + executed guest code.


#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>

#include "common/bug_tracker.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/windows/cpu_pin.h"
#include "common/windows/sampling_profiler.h"
#include "core/arm/jit_prewarm.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/svc/svc_debug_string.h" // Kernel::Svc::SetDebugStringObserver
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "core/perf_stats.h"
#include "hid_core/hid_core.h"
#include "video_core/frame_trace.h"
#include "video_core/gpu.h"
#include "video_core/gpu_thread.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/performance_overlay.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

#include "jadyx/app/boot_config.h"
#include "jadyx/app/boot_diag.h"
#include "jadyx/app/boot_renderer_decls.h"
#include "jadyx/app/boot_session.h"
#include "jadyx/app/boot_settings.h"
#include "jadyx/app/boot_state.h"
#include "jadyx/app/headless_emu_window.h"
#include "jadyx/app/prewarm_budget.h"
#include "jadyx/app/session_memory.h"
#include "jadyx/app/session_menu.h"
#include "jadyx/content/content_service.h"
#include "jadyx/game/uwp_software_keyboard.h"
#include "jadyx/i18n/localization.h"
#include "jadyx/input/uwp_input.h"
#include "jadyx/library/game_library.h"
#include "jadyx/library/play_history.h"
#include "jadyx/mods/mod_settings.h"
#include "jadyx/pulse/pulse_gate.h"
#include "jadyx/pulse/pulse_screen.h"
#include "jadyx/pulse/pulse_sessions.h"
#include "jadyx/pulse/pulse_store_gate.h"
#include "jadyx/settings/uwp_system_settings.h"
#include "jadyx/storage/network_import_gate.h"
#include "jadyx/storage/utf8.h"
#include "jadyx/storage/uwp_file_manager.h"
#include "jadyx/storage/uwp_rom_storage.h"
#include "jadyx/ui/launch_splash.h"

namespace Jadyx {

constexpr const char* JIT_LIVENESS_SENTINEL = "JADYX_JIT_ALIVE";
// Emitted by the payload after its framebuffer loop, so the boot keeps presenting until then.
constexpr const char* GFX_DONE_SENTINEL = "JADYX_GFX_DONE";


namespace {

// As Eden's firmware applet launcher: the applet's program from the system NAND, run as a
// library applet so AM hands it the input it would get from a game.
constexpr u64 mii_edit_program_id = static_cast<u64>(Service::AM::AppletProgramId::MiiEdit);

/// One game on a built System: finds its content, loads it, runs it and shuts it down. Declared in
/// the order the session needs things torn down: last first.
class GameSession {
public:
    GameSession(Core::System& system_, HeadlessEmuWindow& emu_window_, GamepadInput& input_,
                std::string& nro_path_, const BootSurface& surface_,
                const BootConfig& config_, bool mii_editor_)
        : system{system_}, emu_window{emu_window_}, input{input_}, nro_path{nro_path_},
          surface{surface_}, config{config_}, mii_editor{mii_editor_},
          boot_screen{config_.play && surface_.core_window != nullptr && !config_.null_renderer} {}

    /// A process exit-style status.
    int Run();

private:
    void Shutdown();
    /// Each returns a status when the session ended there.
    std::optional<int> PrepareContent();
    std::optional<int> LoadGuest();
    void EndBootScreen();
    void WatchSentinels();
    void StartGpu();
    void PrepareGame();
    void StartGuest();
    int RunGpuFailureProbe();
    void FinishPrewarmBudget(u32 tick);
    int PlayLoop();
    int TimedRun();
    int WaitForSentinels();

    Core::System& system;
    HeadlessEmuWindow& emu_window;
    GamepadInput& input;
    std::string& nro_path;
    const BootSurface& surface;
    const BootConfig& config;
    const bool mii_editor;
    // Played by hand: the launch splash stays up from the library's exit to the game's first
    // frame. The renderer takes the window from the frontend's splash, then draws it itself until
    // the guest is about to run.
    const bool boot_screen;

    // Set only after a failed Load has returned from Core's own synchronous cleanup.
    // Other early Load failures still require the frontend to tear down the kernel.
    bool load_already_shutdown = false;
    // Shown while Pulse saves or resumes a session (pulse_screen.h); it draws through the GPU
    // thread, so it goes before anything is torn down.
    std::optional<PulseScreen> pulse_screen;
    // The splash over the guest's first frames (launch_splash.h); it draws as the Pulse screen.
    std::optional<LaunchScreen> launch_screen;
    // Only while played. First in Shutdown, as its workers read the System.
    std::optional<SessionMemory> memory;
    FileSys::ManualContentProvider manual_provider;
    Jadyx::UwpSoftwareKeyboard* keyboard_applet = nullptr;
    Content::LaunchResolution content;
    Service::AM::FrontendAppletParameters load_parameters;
    std::atomic<bool> guest_exited{false};
    std::optional<Common::Pulse::Identity> pulse_identity;
    bool gpu_started = false, resumed = false;
    std::mutex live_mutex;
    std::condition_variable live_cv;
    std::atomic<bool> jit_alive{false};
    std::atomic<bool> gfx_done{false};
    u64 program_id{};
    PrewarmBudget prewarm_budget;
};

int GameSession::Run() {
    if (const auto status = PrepareContent()) return *status;
    if (const auto status = LoadGuest()) return *status;
    WatchSentinels();
    StartGpu();
    PrepareGame();
    StartGuest();
    if (config.gpu_failure_probe) return RunGpuFailureProbe();
    if (config.play) return PlayLoop();
    if (config.run_seconds > 0) return TimedRun();
    return WaitForSentinels();
}

// Stops what runs over the System, then the guest. Every way out of a session that got this far.
void GameSession::Shutdown() {
    memory.reset();
    pulse_screen.reset();
    launch_screen.reset();
    Common::SamplingProfiler::Stop(); // writes the window in progress, if it runs
    if (config.gpu_failure_probe) WriteDiag("GPU recovery shutdown: input.Stop");
    input.Stop();
    Kernel::Svc::SetDebugStringObserver(nullptr); // detach before teardown
    if (load_already_shutdown) {
        WriteDiag("shutdown: failed Load already cleaned up Core; skipping Pause and second shutdown");
    } else {
        if (g_gpu_command_failed.load(std::memory_order_acquire)) {
            // A discarded GPU batch may never signal a guest syncpoint. Release CPU owners
            // before Pause waits for them; Core shutdown repeats this idempotent cancellation.
            WriteDiag("GPU recovery shutdown: cancel waits before Pause");
            system.Host1x().GetSyncpointManager().CancelWaits();
            system.GPU().NotifyShutdown();
        }
        if (config.gpu_failure_probe) WriteDiag("GPU recovery shutdown: system.Pause");
        void(system.Pause());
        if (config.gpu_failure_probe) WriteDiag("GPU recovery shutdown: ShutdownMainProcess");
        system.ShutdownMainProcess();
    }
    if (config.gpu_failure_probe) WriteDiag("GPU recovery shutdown: HID unload");
    system.HIDCore().UnloadInputDevices();
    Common::BugTracker::RequestFlush(); // this game's summary, before the next one starts
    WriteDiag("step: shutdown complete");
}

std::optional<int> GameSession::PrepareContent() {
    // Filesystem + content plumbing, as yuzu_cmd does it, plus the manual content provider the Qt
    // and Android frontends fill from their game lists: a game dump loaded straight from its file
    // is only found through it, and without it the game has no control data (NACP), so no save
    // data sizes, and aborts.
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                   &manual_provider);
    system.SetFilesystem(MakeUwpFilesystem());
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    system.GetUserChannel().clear();
    // Before Load: games read the profile name when they start.
    if (ApplyUserName(system.GetProfileManager(), LoadSystemSettings().user_name)) {
        WriteDiag("step: profile name from Settings > System");
    }
    // Played by hand, a game asking for text gets the on-screen keyboard (uwp_software_keyboard.h);
    // unattended runs keep the core's stand-in, which answers on its own.
    if (config.play) {
        auto keyboard = std::make_unique<Jadyx::UwpSoftwareKeyboard>();
        keyboard_applet = keyboard.get();
        Service::AM::Frontend::FrontendAppletSet applets; // The others stay the core's.
        applets.software_keyboard = std::move(keyboard);
        system.SetFrontendAppletSet(std::move(applets));
    }
    if (mii_editor) {
        const auto applet = system.GetFileSystemController().GetSystemNANDContents()->GetEntry(
            mii_edit_program_id, FileSys::ContentRecordType::Program);
        if (!applet) {
            WriteDiag("mii editor: not in the system NAND");
            Shutdown(); return MII_EDITOR_MISSING;
        }
        nro_path = applet->GetFullPath();
        system.GetFrontendAppletHolder().SetCurrentAppletId(Service::AM::AppletId::MiiEdit);
        WriteDiag("step: mii editor from the system NAND");
    } else if (const auto file = system.GetFilesystem()->OpenFile(nro_path, FileSys::OpenMode::Read);
        file != nullptr && manual_provider.AddEntriesFromContainer(file)) {
        WriteDiag("step: game contents registered");
    }
    content = mii_editor ? Content::LaunchResolution{} :
        Content::ResolveLaunchContent(*system.GetFilesystem(), nro_path, manual_provider,
                                      Content::ConsumeAllowMissing(nro_path));
    if (content.error != Content::Error::None) {
        WriteDiag("content: launch blocked error=" + std::to_string(static_cast<int>(content.error)));
        Shutdown(); return 16;
    }
    if (content.managed)
        system.GetContentProviderUnion().SetApplicationContentOverride(content.application, &manual_provider);
    // The player's mod choices (which are off, and their order) before the loader reads any mod,
    // and nothing left of an install the app closed during.
    Mods::ClearScratch(Mods::DefaultModPaths());
    Mods::ApplyModChoices(Mods::DefaultModPaths());
    ConsumeStorageReadFailure();
    WriteDiag("step: filesystem factories created");
    return std::nullopt;
}

// Stops the renderer's splash before anything else presents.
void GameSession::EndBootScreen() {
    if (!boot_screen) return;
    D3D12::EndBootScreen(system.Renderer());
    SetLaunchStep(LaunchStep::Starting);
}

std::optional<int> GameSession::LoadGuest() {
    // As yuzu_cmd. Without the Application applet id, AM never sends the focus messages and a game
    // waits for them forever after its first ReceiveMessage (homebrew does not wait).
    load_parameters = mii_editor ?
        Service::AM::FrontendAppletParameters{
            .program_id = mii_edit_program_id,
            .applet_id = Service::AM::AppletId::MiiEdit,
            .applet_type = Service::AM::AppletType::LibraryApplet,
        } :
        Service::AM::FrontendAppletParameters{
            .applet_id = Service::AM::AppletId::Application,
        };
    // A guest that ends on its own (the Mii editor when the player is done) goes back to the
    // library; the play loop sees this between frames.
    system.RegisterExitCallback([this] { guest_exited.store(true); });
    // Run with no caller, the editor ends by asking to return to the main program
    // (ILibraryAppletSelfAccessor::ExitProcessAndReturn); here that is the library.
    if (mii_editor)
        system.RegisterExecuteProgramCallback([this](std::size_t) { guest_exited.store(true); });
    // Pulse: the identity this boot would save under, and the session it resumes if one matches.
    if (config.pulse_sessions && PulseSessions::Enabled() && !mii_editor) {
        try {
            const auto file = system.GetFilesystem()->OpenFile(nro_path, FileSys::OpenMode::Read);
            const auto user = system.GetProfileManager().GetUser(0);
            if (file && user) {
                pulse_identity = PulseSessions::IdentityFor(nro_path, user->uuid, file->GetSize(),
                                                            content.application, content.version,
                                                            content.build_id);
            }
        } catch (const std::exception& e) {
            WritePulseLog(std::string("no identity: ") + e.what());
        }
    }
    if (boot_screen) {
        D3D12::SetWindowHandoff(ReleaseLaunchSplash);
        D3D12::SetBootScreen(DrawLaunchSplash);
    }
    Core::SystemResultStatus load_result{Core::SystemResultStatus::ErrorNotInitialized};
    if (pulse_identity && config.pulse_resume && PulseSessions::CanResume(*pulse_identity)) {
        WritePulseLog("resuming the kept session | " + MemoryReport());
        const auto resume_start = std::chrono::steady_clock::now();
        bool loaded = false;
        try {
            PulseSessions::Resume(system, *pulse_identity, [&](const Kernel::ProcessLayoutPin& pin) {
                system.Kernel().PinNextProcessLayout(pin);
                loaded = true;
                load_result = system.Load(emu_window, nro_path, load_parameters);
                load_already_shutdown = load_result != Core::SystemResultStatus::Success &&
                                        system.IsShuttingDown();
                if (load_result != Core::SystemResultStatus::Success) {
                    throw std::runtime_error("system.Load failed");
                }
                // The services the restore rebuilds talk to the GPU, as in the test bench.
                EndBootScreen();
                system.GPU().Start();
                system.GetCpuManager().OnGpuReady();
                gpu_started = true;
                std::string title;
                void(system.GetAppLoader().ReadTitle(title));
                pulse_screen.emplace(system, PulseScreen::Kind::Resume, Utf8ToUtf16(title));
            });
            resumed = true;
            WritePulseLog("session resumed in " +
                      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - resume_start)
                                         .count()) +
                      " ms | " + MemoryReport());
        } catch (const std::exception& e) {
            WritePulseLog(std::string("resume failed: ") + e.what());
            if (loaded) {
                // This System holds a half-rebuilt process and is never run: a fresh one boots.
                Shutdown();
                return PULSE_RESUME_FAILED;
            }
            PulseSessions::SetNotice(I18n::ToUtf8(I18n::Tr(I18n::Text::PulseNotResumed)));
        }
    }
    if (!resumed) {
        load_result = system.Load(emu_window, nro_path, load_parameters);
        load_already_shutdown = load_result != Core::SystemResultStatus::Success &&
                                system.IsShuttingDown();
    }
    // Unused when Load failed before the renderer came up.
    D3D12::SetWindowHandoff(nullptr);
    D3D12::SetBootScreen(nullptr);
    WriteDiag("step: system.Load() returned status " +
              std::to_string(static_cast<int>(load_result)) + " | " + MemoryReport());
    WriteDiag("memory map: " + LargestAllocations());
    if (load_result != Core::SystemResultStatus::Success) {
        LOG_CRITICAL(Frontend, "Headless boot: failed to load {} (status {})", nro_path,
                     static_cast<int>(load_result));
        // Some early Load failures leave kernel objects; others already shut Core down.
        // Stop frontend owners in both cases, but never run the kernel's teardown twice.
        Shutdown();
        // The loader already looked for the ExeFS in an installed update before giving up.
        if (load_result == Core::SystemResultStatus(u32(Core::SystemResultStatus::ErrorLoader) +
                                                    u32(Loader::ResultStatus::ErrorNoExeFS))) {
            return UPDATE_REQUIRED;
        }
        return 2;
    }
    ApplyGameLanguage(system);
    if (config.bug_tracker) {
        std::string title;
        void(system.GetAppLoader().ReadTitle(title));
        Common::BugTracker::SetTitle(system.GetApplicationProcessProgramID(), std::move(title));
    }
    return std::nullopt;
}

void GameSession::WatchSentinels() {
    // Install the JIT-liveness observer BEFORE running any guest code: it watches every guest
    // svcOutputDebugString chunk for the sentinel and signals the wait below. Cheap no-op for any
    // write that isn't the sentinel; zero cost in builds that never install an observer.
    Kernel::Svc::SetDebugStringObserver([&](std::string_view chunk) {
        if (chunk.find(JIT_LIVENESS_SENTINEL) != std::string_view::npos) {
            jit_alive.store(true, std::memory_order_release);
            live_cv.notify_all();
        }
        if (chunk.find(GFX_DONE_SENTINEL) != std::string_view::npos) {
            gfx_done.store(true, std::memory_order_release);
            live_cv.notify_all();
        }
    });
}

void GameSession::StartGpu() {
    // Start the GPU host thread (null renderer â€” no device) and release the CPU manager.
    if (!gpu_started) {
        system.GPU().Start();
        WriteDiag("step: GPU host thread started");
        system.GetCpuManager().OnGpuReady();
    }

    if (Settings::values.use_disk_shader_cache.GetValue()) {
        // As yuzu_cmd: build the pipelines earlier sessions saved (LocalState/jadyx/shader/<title>/
        // d3d12.bin) before the guest runs, so they do not stutter in; the ones the game meets
        // later are appended to the file. The renderer shows the progress on screen.
        WriteDiag("step: building the disk shader cache | " + MemoryReport());
        VideoCore::RendererBase& renderer = system.Renderer();
        if (pulse_screen) pulse_screen->ShadersStarting();
        renderer.ReadRasterizer()->LoadDiskResources(
            system.GetApplicationProcessProgramID(), std::stop_token{},
            [this, &renderer](VideoCore::LoadCallbackStage, size_t done, size_t total) {
                // A resume's screen and the splash show the count themselves; two presenters
                // would race.
                if (pulse_screen) pulse_screen->Shaders(done, total);
                else if (boot_screen) SetLaunchStep(LaunchStep::Shaders, done, total);
                else D3D12::ShowLoadProgress(renderer, done, total);
            });
        WriteDiag("step: disk shader cache built | " + MemoryReport());
    }
}

void GameSession::PrepareGame() {
    program_id = system.GetApplicationProcessProgramID();
    ApplyGameGraphics(system, config, LoadGameGraphics(program_id));
    // The game's own offline mode over Settings > System's, before any guest code runs; boot.cfg's
    // airplane=1 wins over both.
    if (const auto offline = LoadGameOfflineMode(program_id); offline && !config.airplane_mode)
        Settings::values.airplane_mode.SetValue(*offline);
    WriteDiag(Settings::values.airplane_mode.GetValue() ? "network: offline mode, the game sees no internet"
                                                        : "network: the game sees the internet");
    prewarm_budget = PrewarmBudget{LoadPrewarmStep(program_id)};
    if (config.jit_prewarm != BootConfig::JitPrewarm::Off) {
        // All guest cores are still stopped. Each owns a separate JIT; never
        // compile into an instance concurrently with Run or another compiler.
        // A resume warms too: its JIT starts empty like a boot's, and without the pass the
        // whole game compiled in its first minute (bursts of 25k blocks in one frame). The
        // blocks are checked against the restored code, so a session's own loads are safe.
        if (auto* process = system.ApplicationProcess()) {
            const u64 prewarm_mib =
                config.jit_prewarm_mib != 0 ? config.jit_prewarm_mib : prewarm_budget.MiBPerCore();
            WriteDiag(std::string{"step: CPU JIT profile/prewarm starting ("} +
                      (process->Is64Bit() ? "A64" : "A32") + "), " +
                      std::to_string(prewarm_mib) + " MiB per core" +
                      (config.jit_prewarm_mib != 0 ? " (boot.cfg)" : "") + " | " +
                      MemoryReport());
            if (pulse_screen) pulse_screen->CpuStarting();
            Core::ConfigureApplicationPrewarm(system,
                config.jit_prewarm == BootConfig::JitPrewarm::Warm,
                [this](size_t done, size_t total) {
                    // A resume's screen and the splash show the count themselves; two presenters
                    // would race.
                    if (pulse_screen) pulse_screen->Cpu(done, total);
                    else if (boot_screen) SetLaunchStep(LaunchStep::Cpu, done, total);
                    else D3D12::ShowCpuLoadProgress(system.Renderer(), done, total);
                },
                prewarm_mib * PrewarmBudget::MiB);
            WriteDiag("step: CPU JIT profile/prewarm ready | " + MemoryReport());
        }
    }
}

void GameSession::StartGuest() {
    if (config.profile) {
        const auto output =
            Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "jadyx_profile.txt";
        Common::SamplingProfiler::Start({.hz = config.profile_hz,
                                         .window_seconds = config.profile_window,
                                         .output = output});
        WriteDiag("step: sampling profiler on, " + std::to_string(config.profile_hz) +
                  " Hz, " + std::to_string(config.profile_window) + " s windows");
    }
    // Run the guest. CpuManager spins up guest threads; dynarmic compiles + executes their code.
    // A resume's screen stays over it until the game presents.
    if (pulse_screen) pulse_screen->GameStarting();
    // The splash goes on over the guest until the game presents (a resume's screen ended it).
    if (boot_screen && !pulse_screen) {
        EndBootScreen();
        launch_screen.emplace(system);
    }
    void(system.Run());
    WriteDiag("step: CPU pin " + Common::Windows::CpuPinReport());
    input.Start(system.HIDCore());
}

int GameSession::RunGpuFailureProbe() {
    WriteDiag("GPU recovery probe: injecting callback failure");
    system.GPU().RunOnGpuThread([&] {
        if (config.gpu_removal_probe) {
            D3D12::RemoveDeviceForProbe(system.Renderer());
        }
        throw std::runtime_error("injected GPU allocation failure for recovery gate");
    });
    WriteDiag("GPU recovery probe: failed callback waiter released");
    // Completion of a second request also proves that the failed GPU thread can drain host
    // sync work after its observer has run, without racing that observer's notification.
    system.GPU().RunOnGpuThread([] {});
    WriteDiag("GPU recovery probe: second callback completed");
    if (!g_gpu_command_failed.load(std::memory_order_acquire)) {
        throw std::runtime_error("GPU exception recovery probe did not notify frontend");
    }
    WriteDiag("GPU recovery probe: starting shutdown");
    Shutdown();
    WriteDiag("GPU failure recovery probe PASS: host waiter released and shutdown completed");
    return 0;
}

// The next session's prewarm budget follows this one's headroom (PrewarmBudget).
void GameSession::FinishPrewarmBudget(u32 tick) {
    if (prewarm_budget.Finish(tick / TICKS_PER_SECOND)) {
        SavePrewarmStep(program_id, static_cast<std::int32_t>(prewarm_budget.Step()));
        WriteDiag("JIT prewarm budget raised to " +
                  std::to_string(prewarm_budget.MiBPerCore()) +
                  " MiB per core for this game's next session");
    }
}

int GameSession::PlayLoop() {
    // Played by hand: the guest runs until the app is closed from the console, which ends the
    // process. The diag keeps a heartbeat of how long it ran and how much memory it used.
    // Q on a keyboard (the PC) ends the session cleanly instead, so the run can be told apart
    // from a crash.
    WriteDiag("step: system.Run() issued, playing until the app is closed | " + MemoryReport());
    // Play history (play_history.h): the launch is recorded now, play time while the guest runs
    // (never with the menu open), saved each minute since the dashboard can end the process.
    std::optional<PlaySession> play_session;
    if (!mii_editor) play_session.emplace(nro_path, program_id);
    SessionMenu menu{{system, input, emu_window, config, surface, pulse_screen, launch_screen,
                      pulse_identity, keyboard_applet, program_id, mii_editor}};
    constexpr auto TICK = std::chrono::milliseconds(20);
    memory.emplace(system, config, program_id, prewarm_budget);
    // Pulse: after a resume the presented frames are logged each second for 30 s, so a guest
    // that freezes shows when it stopped.
    u64 resume_frames = 0;
    for (u32 tick = 1;; ++tick) {
        std::this_thread::sleep_for(TICK);
        if (play_session) play_session->Tick(!menu.IsOpen());
        memory->Tick(tick, menu.IsOpen());
        if (ConsumeStorageReadFailure()) {
            WriteDiag("content: storage read failed; closing guest for library recovery");
            Shutdown(); return 16;
        }
        if (g_gpu_command_failed.load(std::memory_order_acquire)) {
            WriteDiag("GPU command failed; shutting down game for library recovery | " + MemoryReport());
            Shutdown();
            return 15;
        }
        if (resumed && tick % TICKS_PER_SECOND == 0 && tick <= 30 * TICKS_PER_SECOND) {
            const u32 second = tick / TICKS_PER_SECOND;
            const u64 frames = system.GetPerfStats().TotalGameFrames();
            WritePulseLog("resumed +" + std::to_string(second) + " s: " +
                          std::to_string(frames - resume_frames) + " frames" +
                          (menu.IsOpen() ? " (menu)" : ""));
            resume_frames = frames;
        }
        if (config.hunt_at > 0 && tick == config.hunt_at * TICKS_PER_SECOND) {
            D3D12::HuntNextFrames();
            WriteDiag("hunt_at: bottleneck capture requested for next guest frames");
        }
        if (config.frame_trace_on_drop > 0 && tick == 1) {
            VideoCore::FrameTrace::ArmOnSwapIntervalRise(300, 180, config.frame_trace_on_drop);
            WriteDiag("frame_trace_on_drop: frame chain timeline armed");
        }
        if (config.frame_trace_at > 0 && tick == config.frame_trace_at * TICKS_PER_SECOND) {
            VideoCore::FrameTrace::Start(480);
            WriteDiag("frame_trace_at: frame chain timeline requested");
        }
        if (config.quit_at > 0 && tick == config.quit_at * TICKS_PER_SECOND) {
            WriteDiag("quit_at: shutting down | " + MemoryReport());
            FinishPrewarmBudget(tick);
            Shutdown();
            return 0;
        }
        if (Jadyx::QuitRequested()) {
            WriteDiag("step: Q pressed after " + std::to_string(tick / 50) +
                      " s, shutting down | " + MemoryReport());
            FinishPrewarmBudget(tick);
            Shutdown();
            LOG_INFO(Frontend, "Headless boot: session closed with Q.");
            return 0;
        }
        if (guest_exited.load()) {
            WriteDiag("step: the guest exited after " + std::to_string(tick / 50) +
                      " s; back to the library | " + MemoryReport());
            FinishPrewarmBudget(tick);
            Shutdown();
            return RETURN_TO_LIBRARY;
        }
        if (menu.Update() == SessionMenu::Outcome::BackToLibrary) {
            FinishPrewarmBudget(tick);
            Shutdown();
            return RETURN_TO_LIBRARY;
        }
        memory->Report(tick);
    }
}

int GameSession::TimedRun() {
    // Timed mode: the clock, or the payload's own end sentinel. The payload counts as having run if
    // the process is still alive when either comes; the log says what it drew.
    WriteDiag("step: system.Run() issued, running for " + std::to_string(config.run_seconds) +
              " s | " + MemoryReport());
    for (u32 second = 1; second <= config.run_seconds; ++second) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (ConsumeStorageReadFailure()) {
            WriteDiag("content: storage read failed; closing guest for library recovery");
            Shutdown(); return 16;
        }
        if (g_gpu_command_failed.load(std::memory_order_acquire)) {
            WriteDiag("GPU command failed during timed gate | " + MemoryReport());
            Shutdown();
            return 15;
        }
        // A payload that says it has finished ends the run early; run_seconds is then a backstop.
        if (gfx_done.load(std::memory_order_acquire)) {
            WriteDiag("step: payload finished after " + std::to_string(second) + " s");
            break;
        }
        if (second % 10 == 0) {
            WriteDiag("step: running, " + std::to_string(second) + " s | " + MemoryReport());
            if (second % 30 == 0) {
                WriteDiag("memory map: " + LargestAllocations());
            }
        }
    }
    Shutdown();
    LOG_INFO(Frontend, "Headless boot: timed run of {} s finished.", config.run_seconds);
    return 0;
}

int GameSession::WaitForSentinels() {
    WriteDiag("step: system.Run() issued, waiting for the JIT sentinel | " + MemoryReport());

    // Headless: no window event loop. Wait for the guest to execute through the JIT and emit the
    // sentinel; the timeout is only a backstop (a hung/failed boot), not the success path.
    constexpr auto kLivenessTimeout = std::chrono::seconds(30);
    {
        std::unique_lock lock(live_mutex);
        live_cv.wait_for(lock, kLivenessTimeout,
                         [&] { return jit_alive.load(std::memory_order_acquire); });
    }
    const bool alive = jit_alive.load(std::memory_order_acquire);
    WriteDiag(std::string(alive ? "step: sentinel observed"
                                : "step: sentinel NOT observed within timeout") +
              " | " + MemoryReport());

    // With a real renderer the payload goes on to draw frames; keep the guest running so they reach
    // the screen, until it reports the loop finished (or the backstop expires).
    if (alive && surface.core_window != nullptr) {
        constexpr auto kGfxTimeout = std::chrono::seconds(30);
        {
            std::unique_lock lock(live_mutex);
            live_cv.wait_for(lock, kGfxTimeout,
                             [&] { return gfx_done.load(std::memory_order_acquire); });
        }
        WriteDiag(std::string(gfx_done.load() ? "step: framebuffer loop finished"
                                              : "step: framebuffer loop NOT finished in time") +
                  " | " + MemoryReport());
    }

    Shutdown();

    if (alive) {
        LOG_INFO(Frontend, "Headless boot: JIT liveness CONFIRMED ('{}' observed).",
                 JIT_LIVENESS_SENTINEL);
        return 0;
    }
    LOG_CRITICAL(Frontend, "Headless boot: JIT-liveness sentinel '{}' not observed within timeout.",
                 JIT_LIVENESS_SENTINEL);
    return 3;
}

} // namespace

// Returns a process exit-style status. 0 == boot reached the run phase cleanly.
int RunHeadlessBoot(const std::string& launch_path, const BootSurface& surface,
                    const BootConfig& config) {
    // The library's Mii editor entry boots the firmware's applet instead of a game.
    const bool mii_editor = launch_path == MiiEditorLaunch;
    std::string nro_path = launch_path;
    // The editor is not a game: no performance overlay over it; games get the player's choice back.
    struct OverlayRestore {
        bool active;
        ~OverlayRestore() {
            if (active)
                VideoCore::performance_overlay_enabled.store(LoadSystemSettings().show_performance_stats,
                                                             std::memory_order_relaxed);
        }
    } overlay_restore{mii_editor};
    if (!config.log_filter.empty()) {
        Settings::values.log_filter = config.log_filter;
    }
    Common::Log::Initialize();
    if (config.bug_tracker) {
        // Idempotent: the library launches several games in one process.
        Common::BugTracker::Install(Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir),
                                  std::string(Common::g_build_fullname) + " (" +
                                      Common::g_scm_desc + ")");
    }
    if (config.rom_storage_checks)
        return RunRomStorageGate(config.rom_storage_checks == 2, WriteDiag) ? 0 : 15;
    if (config.file_manager_gate) return RunFileManagerGate(WriteDiag) ? 0 : 15;
    if (config.log_export_gate) return RunLogExportGate(
        std::filesystem::path{std::wstring{winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()}},
        WriteDiag) ? 0 : 15;
    if (config.network_import_gate) return RunNetworkImportGate(
        std::filesystem::path{std::wstring{winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()}},
        WriteDiag) ? 0 : 15;
    ApplyBootConfig(config, surface, mii_editor);

    if (config.pulse_store) {
        const auto result = RunPulseStoreGate(
            {.diag = [](const std::string& line) { WriteDiag(line); }, .restart = [] { RestartApp(); }});
        if (result != PulseStoreGateResult::Pass) return 17;
        if (!config.pulse_cycles) return 0;
    }
    if (config.pulse_cycles) {
        return RunPulseGate(launch_path, {.cycles = config.pulse_cycles,
                                       .seed = config.pulse_seed,
                                       .core_window = config.null_renderer ? nullptr
                                                                           : surface.core_window,
                                       .width = surface.width,
                                       .height = surface.height,
                                       .diag = [](const std::string& line) { WriteDiag(line); }});
    }

    Core::System system{};
    g_gpu_command_failed.store(false, std::memory_order_release);
    VideoCommon::GPUThread::SetExceptionObserver([](const char* message) {
        // Logged here, on the GPU thread: tells a stuck recovery apart from a GPU thread that
        // never got to report its failure.
        if (!g_gpu_command_failed.exchange(true, std::memory_order_acq_rel)) {
            WriteDiag(std::string("GPU thread reported a failure: ") + message);
        }
    });
    WriteDiag("step: Core::System constructed");
    system.Initialize();
    WriteDiag("step: system.Initialize() done");
    system.ApplySettings();
    WriteDiag("step: system.ApplySettings() done");

    HeadlessEmuWindow emu_window{surface.core_window, surface.width, surface.height};

    // Before Load, so HID starts with player 1 connected and bound to the gamepad engine.
    GamepadInput input{config.input_script};
    system.HIDCore().ReloadInputDevices();

    GameSession session{system, emu_window, input, nro_path, surface, config, mii_editor};
    return session.Run();
}

} // namespace Jadyx
