// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/network/nextendo/service.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Authentication.Web.h>
#include <winrt/Windows.Security.Credentials.h>
#include <winrt/Windows.Storage.h>

#include "common/logging.h"
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
constexpr wchar_t ServerKey[] = L"nextendo_server_ip";
constexpr wchar_t NatKey[] = L"nextendo_nat_ip";
constexpr wchar_t TlsKey[] = L"nextendo_tls_mode";
constexpr std::uintmax_t MaxCatalogFile = 1u << 20;

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
        const auto address = [&](const wchar_t* key) {
            if (!values.HasKey(key)) {
                return std::string{};
            }
            const auto text = winrt::to_string(winrt::unbox_value<winrt::hstring>(values.Lookup(key)));
            return CleanAddress(text).value_or(std::string{});
        };
        preferences.server_ip = address(ServerKey);
        preferences.nat_ip = address(NatKey);
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
    bool signing_in = false;
    bool started = false;
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
        {
            std::scoped_lock lock{state.mutex};
            commands.swap(state.commands);
            presence_wanted = state.preferences.use_nextendo;
        }
        for (auto& command : commands) {
            command(session);
        }
        session.SetPresenceWanted(presence_wanted);
        const std::int64_t wait = session.Step();
        Publish(session, published);
        std::unique_lock lock{state.mutex};
        state.wake.wait_for(lock, stop, std::chrono::seconds(wait),
                            [&state] { return !state.commands.empty(); });
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
    {
        std::scoped_lock lock{state.mutex};
        if (!state.started || state.signing_in || state.view.phase != Phase::SignedOut) {
            return;
        }
        state.signing_in = true;
    }
    const auto pkce = MakePkcePair();
    const std::string expected_state = MakeState();
    const auto finish = [](Command command) {
        {
            auto& shared = Shared();
            std::scoped_lock lock{shared.mutex};
            shared.signing_in = false;
        }
        Post(std::move(command));
    };
    if (!pkce || expected_state.empty()) {
        finish([](Session& session) { session.CancelSignIn(false); });
        return;
    }
    Post([](Session& session) { session.BeginSignIn(); });
    using namespace winrt::Windows::Security::Authentication::Web;
    using winrt::Windows::Foundation::AsyncStatus;
    using winrt::Windows::Foundation::Uri;
    try {
        const std::string url = BuildAuthorizeUrl(kRedirectUri, expected_state, pkce->challenge);
        auto operation = WebAuthenticationBroker::AuthenticateAsync(
            WebAuthenticationOptions::None, Uri{winrt::to_hstring(url)},
            Uri{winrt::to_hstring(kRedirectUri)});
        operation.Completed([verifier = pkce->verifier, expected_state, finish](
                                const auto& async, AsyncStatus status) {
            CallbackResult callback;
            bool by_player = false;
            bool answered = false;
            try {
                if (status == AsyncStatus::Completed) {
                    const auto result = async.GetResults();
                    switch (result.ResponseStatus()) {
                    case WebAuthenticationStatus::Success:
                        callback = ParseCallback(winrt::to_string(result.ResponseData()),
                                                 expected_state);
                        answered = true;
                        break;
                    case WebAuthenticationStatus::UserCancel:
                        by_player = true;
                        break;
                    default:
                        break;
                    }
                }
            } catch (...) {
            }
            if (answered) {
                finish([callback, verifier](Session& session) {
                    session.FinishSignIn(callback, verifier, std::string(kRedirectUri));
                });
            } else {
                finish([by_player](Session& session) { session.CancelSignIn(by_player); });
            }
        });
    } catch (...) {
        LOG_WARNING(Network, "[Nextendo] the sign-in page did not open");
        finish([](Session& session) { session.CancelSignIn(false); });
    }
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
