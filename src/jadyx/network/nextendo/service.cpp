// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/network/nextendo/service.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.h>
#include <winrt/Windows.Storage.h>

#include "common/logging.h"
#include "jadyx/app/boot_diag.h"
#include "jadyx/network/http.h"
#include "jadyx/network/nextendo/network_mode.h"
#include "jadyx/network/nextendo/pkce.h"
#include "jadyx/network/online_rooms.h"

namespace Jadyx::Network::Nextendo {
namespace {

namespace Nx = Core::Online::Nextendo;
namespace fs = std::filesystem;

constexpr wchar_t VaultResource[] = L"Jadyx.Nextendo";
constexpr wchar_t VaultUser[] = L"refresh_token";
constexpr wchar_t UseKey[] = L"nextendo_use";
// Renamed when the defaults arrived: the earlier keys held empty values, not a player's choice.
constexpr wchar_t ServerKey[] = L"nextendo_server";
constexpr wchar_t NatKey[] = L"nextendo_nat";
constexpr wchar_t TlsKey[] = L"nextendo_tls_mode";
constexpr std::uintmax_t MaxCatalogFile = 1u << 20;
/// How long a sign-in waits for the phone's answer: as long as the server keeps a code.
constexpr auto SignInWait = std::chrono::minutes{15};

/// The refresh token in the system's credential vault (encrypted per user by the system); the
/// public catalog as a file in LocalCache.
class VaultStorage final : public Storage {
public:
    std::optional<std::string> LoadRefreshToken() override {
        try {
            winrt::Windows::Security::Credentials::PasswordVault vault;
            auto credential = vault.Retrieve(VaultResource, VaultUser);
            credential.RetrievePassword();
            return winrt::to_string(credential.Password());
        } catch (...) {
            return std::nullopt; // none stored yet
        }
    }

    bool SaveRefreshToken(std::string_view token) override {
        try {
            winrt::Windows::Security::Credentials::PasswordVault vault;
            RemoveAll(vault);
            vault.Add({VaultResource, VaultUser, winrt::to_hstring(token)});
            return true;
        } catch (...) {
            LOG_WARNING(Network, "[Nextendo] the credential vault refused the sign-in");
            return false;
        }
    }

    void ClearRefreshToken() override {
        try {
            winrt::Windows::Security::Credentials::PasswordVault vault;
            RemoveAll(vault);
        } catch (...) {
        }
    }

    std::optional<std::string> LoadCatalog() override {
        std::error_code error;
        const auto path = CatalogPath();
        const auto size = fs::file_size(path, error);
        if (error || size == 0 || size > MaxCatalogFile) {
            return std::nullopt;
        }
        std::ifstream file(path, std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (!file && !file.eof()) {
            return std::nullopt;
        }
        return text;
    }

    void SaveCatalog(std::string_view envelope) override {
        std::error_code error;
        const auto path = CatalogPath();
        if (path.empty()) {
            return;
        }
        fs::create_directories(path.parent_path(), error);
        const auto part = fs::path(path).concat(L".part");
        {
            std::ofstream file(part, std::ios::binary | std::ios::trunc);
            file.write(envelope.data(), static_cast<std::streamsize>(envelope.size()));
            if (!file) {
                fs::remove(part, error);
                return;
            }
        }
        fs::rename(part, path, error);
    }

private:
    static void RemoveAll(const winrt::Windows::Security::Credentials::PasswordVault& vault) {
        try {
            for (const auto& credential : vault.FindAllByResource(VaultResource)) {
                vault.Remove(credential);
            }
        } catch (...) {
            // FindAllByResource throws when there is none.
        }
    }

    static fs::path CatalogPath() {
        try {
            return fs::path{winrt::Windows::Storage::ApplicationData::Current()
                                .LocalCacheFolder()
                                .Path()
                                .c_str()} /
                   L"nextendo" / L"catalog.json";
        } catch (...) {
            return {};
        }
    }
};

Preferences LoadPreferences() {
    Preferences preferences;
    try {
        const auto values =
            winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        if (values.HasKey(UseKey)) {
            preferences.use_nextendo = winrt::unbox_value<bool>(values.Lookup(UseKey));
        }
        // Never saved: the default stays.
        const auto address = [&](const wchar_t* key, std::string fallback) {
            if (!values.HasKey(key)) {
                return fallback;
            }
            const auto text = winrt::to_string(winrt::unbox_value<winrt::hstring>(values.Lookup(key)));
            return CleanAddress(text).value_or(std::move(fallback));
        };
        preferences.server_ip = address(ServerKey, preferences.server_ip);
        preferences.nat_ip = address(NatKey, preferences.nat_ip);
        if (values.HasKey(TlsKey)) {
            const auto mode = winrt::unbox_value<int32_t>(values.Lookup(TlsKey));
            if (mode >= 0 && mode <= static_cast<int32_t>(Nx::TlsVerifyMode::Enforce)) {
                preferences.tls_mode = static_cast<Nx::TlsVerifyMode>(mode);
            }
        }
    } catch (...) {
        // Missing or invalid values keep the defaults.
    }
    return preferences;
}

void SavePreferences(const Preferences& preferences) {
    try {
        const auto values =
            winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        values.Insert(UseKey, winrt::box_value(preferences.use_nextendo));
        values.Insert(ServerKey, winrt::box_value(winrt::to_hstring(preferences.server_ip)));
        values.Insert(NatKey, winrt::box_value(winrt::to_hstring(preferences.nat_ip)));
        values.Insert(TlsKey, winrt::box_value(static_cast<int32_t>(preferences.tls_mode)));
    } catch (...) {
        LOG_WARNING(Network, "[Nextendo] could not save the settings");
    }
}

std::int64_t UnixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

const char* PhaseName(Phase phase) {
    switch (phase) {
    case Phase::SignedOut:
        return "signed out";
    case Phase::SigningIn:
        return "signing in";
    case Phase::Connecting:
        return "connecting";
    case Phase::Ready:
        return "ready";
    }
    return "?";
}

const char* ProblemName(Problem problem) {
    switch (problem) {
    case Problem::None:
        return "none";
    case Problem::Offline:
        return "offline";
    case Problem::RateLimited:
        return "rate limited";
    case Problem::Unavailable:
        return "unavailable";
    case Problem::SignInCancelled:
        return "sign-in cancelled";
    case Problem::SignInRejected:
        return "sign-in rejected";
    case Problem::SessionExpired:
        return "session expired";
    case Problem::MissingScope:
        return "missing scope";
    case Problem::Revoked:
        return "revoked";
    case Problem::StorageFailed:
        return "vault failed";
    }
    return "?";
}

using Command = std::function<void(Session&)>;

struct State {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::vector<Command> commands;
    Preferences preferences;
    SessionView view;
    Nx::GameCatalog catalog;
    std::uint64_t version = 1;
    bool game_running = false;
    bool started = false;
    bool page_shown = false;
    /// A sign-in waiting for the phone (SignIn): what its answer must carry, and until when.
    struct WaitingSignIn {
        std::string verifier;
        std::string state;
        std::string url;
        std::chrono::steady_clock::time_point deadline;
    };
    std::optional<WaitingSignIn> sign_in;
};

State& Shared() {
    // Never destroyed: the worker may still be ending while the process exits.
    static State* const state = new State;
    return *state;
}

std::jthread g_worker;

void Post(Command command) {
    auto& state = Shared();
    {
        std::scoped_lock lock{state.mutex};
        state.commands.push_back(std::move(command));
    }
    state.wake.notify_all();
}

/// Copies what changed to the shared state and hands the emulated services their config.
/// Worker thread only.
void Publish(const Session& session, Nx::Config& published) {
    auto& state = Shared();
    Preferences preferences;
    {
        std::scoped_lock lock{state.mutex};
        preferences = state.preferences;
        const auto& view = session.View();
        if (view.version != state.view.version) {
            if (view.phase != state.view.phase || view.problem != state.view.problem) {
                LOG_INFO(Network, "[Nextendo] account {} (problem: {})", PhaseName(view.phase),
                         ProblemName(view.problem));
            }
            if (view.catalog_revision != state.view.catalog_revision) {
                LOG_INFO(Network, "[Nextendo] catalog revision {} with {} games",
                         view.catalog_revision, view.catalog.size());
            }
            state.view = view;
            state.catalog = session.Catalog();
            ++state.version;
        }
    }
    auto config = BuildCoreConfig(preferences, session.Identity());
    if (!SameCoreConfig(config, published)) {
        if (config.enabled != published.enabled) {
            LOG_INFO(Network, "[Nextendo] emulated services {}",
                     config.enabled ? "use Nextendo" : "back to plain Eden");
        }
        Nx::GlobalConfig().Publish(config);
        published = std::move(config);
    }
}

void Run(std::stop_token stop) {
    auto& state = Shared();
    VaultStorage storage;
    Session session{[stop](const Http::ApiRequest& request) { return Http::Send(request, stop); },
                    storage, UnixNow};
    session.Resume();
    Nx::Config published;
    Publish(session, published);
    while (!stop.stop_requested()) {
        std::vector<Command> commands;
        bool presence_wanted = false;
        bool wanted = false;
        bool gave_up = false;
        {
            std::scoped_lock lock{state.mutex};
            if (state.sign_in && std::chrono::steady_clock::now() >= state.sign_in->deadline) {
                state.sign_in.reset();
                gave_up = true;
            }
            commands.swap(state.commands);
            presence_wanted = state.preferences.use_nextendo;
            wanted = presence_wanted || state.page_shown;
        }
        if (gave_up) {
            WriteDiag("nextendo: the sign-in waited too long for the phone");
            session.CancelSignIn(true);
        }
        for (auto& command : commands) {
            command(session);
        }
        session.SetPresenceWanted(presence_wanted);
        // A player who never opened Nextendo nor signed in sends it nothing; a stored sign-in
        // keeps its tokens alive.
        std::int64_t wait = Timing::Idle;
        if (wanted || session.View().phase != Phase::SignedOut) {
            wait = session.Step();
        }
        Publish(session, published);
        std::unique_lock lock{state.mutex};
        auto timeout = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(wait));
        if (state.sign_in) {
            // Awake when a waiting sign-in runs out, so the console stops waiting with it.
            timeout = std::min(timeout, state.sign_in->deadline - std::chrono::steady_clock::now() +
                                            std::chrono::seconds{1});
        }
        state.wake.wait_for(lock, stop, timeout, [&state] { return !state.commands.empty(); });
    }
    // Nothing is left for the games once the worker ends.
    Nx::GlobalConfig().Publish(Nx::Config{});
}

void Changed() {
    // The worker republishes the config from the new preferences.
    Post([](Session&) {});
}

} // namespace

void StartWorker() {
    auto& state = Shared();
    {
        std::scoped_lock lock{state.mutex};
        if (state.started) {
            return;
        }
        state.started = true;
        state.preferences = LoadPreferences();
    }
    g_worker = std::jthread(Run);
}

void StopWorker() {
    if (g_worker.joinable()) {
        g_worker.request_stop();
        Shared().wake.notify_all();
        g_worker.join();
    }
}

SessionView ServiceView() {
    auto& state = Shared();
    std::scoped_lock lock{state.mutex};
    return state.view;
}

std::uint64_t ServiceVersion() {
    auto& state = Shared();
    std::scoped_lock lock{state.mutex};
    return state.version;
}

Nx::GameCatalog ServiceCatalog() {
    auto& state = Shared();
    std::scoped_lock lock{state.mutex};
    return state.catalog;
}

void ShowingNextendo(bool shown) {
    auto& state = Shared();
    {
        std::scoped_lock lock{state.mutex};
        if (state.page_shown == shown) {
            return;
        }
        state.page_shown = shown;
    }
    if (shown) {
        Changed();
    }
}

Preferences ServicePreferences() {
    auto& state = Shared();
    std::scoped_lock lock{state.mutex};
    return state.preferences;
}

void SetEndpoints(const std::string& server_ip, const std::string& nat_ip,
                  Nx::TlsVerifyMode tls_mode) {
    auto& state = Shared();
    Preferences saved;
    {
        std::scoped_lock lock{state.mutex};
        state.preferences.server_ip = CleanAddress(server_ip).value_or(state.preferences.server_ip);
        state.preferences.nat_ip = CleanAddress(nat_ip).value_or(state.preferences.nat_ip);
        state.preferences.tls_mode = tls_mode;
        saved = state.preferences;
        ++state.version;
    }
    SavePreferences(saved);
    Changed();
}

bool ChooseNextendo(bool on) {
    auto& state = Shared();
    Preferences saved;
    {
        std::scoped_lock lock{state.mutex};
        NetworkModeArbiter arbiter;
        arbiter.Commit(state.preferences.use_nextendo ? NetworkMode::Nextendo
                       : Online::InRoom()             ? NetworkMode::Rooms
                                                      : NetworkMode::Offline);
        const auto change =
            arbiter.Request(on ? NetworkMode::Nextendo : NetworkMode::Rooms, state.game_running);
        if (!change.allowed) {
            return false;
        }
        if (change.leave_room) {
            Online::Leave();
        }
        if (state.preferences.use_nextendo == on) {
            return true;
        }
        state.preferences.use_nextendo = on;
        saved = state.preferences;
        ++state.version;
    }
    LOG_INFO(Network, "[Nextendo] online through {}", on ? "Nextendo" : "Eden rooms");
    SavePreferences(saved);
    Changed();
    return true;
}

bool RoomsAllowed() {
    return !ServicePreferences().use_nextendo;
}

void SignIn() {
    auto& state = Shared();
    const auto pkce = MakePkcePair();
    const std::string expected_state = MakeState();
    {
        std::scoped_lock lock{state.mutex};
        if (!state.started || state.sign_in || state.view.phase != Phase::SignedOut) {
            WriteDiag(std::string("nextendo: sign-in ignored (") +
                      (!state.started ? "worker not started" : state.sign_in ? "already waiting" : "not signed out") +
                      ")");
            return;
        }
        if (pkce && !expected_state.empty()) {
            state.sign_in = State::WaitingSignIn{
                pkce->verifier, expected_state, BuildAuthorizeUrl(kRedirectUri, expected_state, pkce->challenge),
                std::chrono::steady_clock::now() + SignInWait};
        }
    }
    Post([](Session& session) { session.BeginSignIn(); });
    if (!pkce || expected_state.empty()) {
        WriteDiag("nextendo: sign-in could not make its PKCE pair");
        Post([](Session& session) { session.CancelSignIn(false); });
        return;
    }
    WriteDiag("nextendo: sign-in waits for the phone");
}

std::string SignInUrl() {
    auto& state = Shared();
    std::scoped_lock lock{state.mutex};
    if (!state.sign_in || std::chrono::steady_clock::now() >= state.sign_in->deadline) {
        return {};
    }
    return state.sign_in->url;
}

std::string CompleteSignIn(std::string_view pasted) {
    auto& state = Shared();
    const std::string address = PastedCallback(pasted);
    CallbackResult callback;
    std::string verifier;
    {
        std::scoped_lock lock{state.mutex};
        if (!state.sign_in || std::chrono::steady_clock::now() >= state.sign_in->deadline) {
            return "none";
        }
        if (address.empty()) {
            return "address";
        }
        // A wrong paste keeps the sign-in waiting: the player may try again.
        callback = ParseCallback(address, state.sign_in->state);
        if (callback.error == "state_mismatch") {
            return "state";
        }
        if (callback.error == "missing_code") {
            return "address";
        }
        verifier = std::move(state.sign_in->verifier);
        state.sign_in.reset();
    }
    // Never the code: only that an answer came, and whether the player accepted.
    const bool accepted = !callback.code.empty();
    WriteDiag(accepted ? "nextendo: the phone's sign-in answer arrived" : "nextendo: sign-in refused on the phone");
    Post([callback, verifier = std::move(verifier)](Session& session) mutable {
        session.FinishSignIn(callback, std::move(verifier), std::string(kRedirectUri));
    });
    return accepted ? "" : callback.error == "access_denied" ? "denied" : "refused";
}

void CancelSignIn() {
    auto& state = Shared();
    {
        std::scoped_lock lock{state.mutex};
        if (!state.sign_in) {
            return;
        }
        state.sign_in.reset();
    }
    WriteDiag("nextendo: sign-in cancelled on the console");
    Post([](Session& session) { session.CancelSignIn(true); });
}

void SignOut() {
    Post([](Session& session) { session.SignOut(); });
}

void RefreshCatalog() {
    Post([](Session& session) { session.RequestCatalog(); });
}

void GameStarted(std::uint64_t title_id, std::string detail) {
    auto& state = Shared();
    bool use = false;
    {
        std::scoped_lock lock{state.mutex};
        state.game_running = true;
        use = state.preferences.use_nextendo;
    }
    if (use) {
        Post([title_id, detail = std::move(detail)](Session& session) mutable {
            session.SetPlaying(title_id, std::move(detail));
        });
    }
}

void GameEnded() {
    auto& state = Shared();
    {
        std::scoped_lock lock{state.mutex};
        state.game_running = false;
    }
    Post([](Session& session) { session.SetPlaying(0, {}); });
}

LaunchCheck CheckGame(std::uint64_t title_id, std::string_view installed_version) {
    auto& state = Shared();
    std::scoped_lock lock{state.mutex};
    return CheckLaunch(state.preferences, state.view, state.catalog, title_id, installed_version);
}

} // namespace Jadyx::Network::Nextendo
