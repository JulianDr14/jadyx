// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/settings/uwp_system_settings.h"

#include <type_traits>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.UserProfile.h>
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/performance_overlay.h"
#include "core/hle/service/acc/profile_manager.h"

namespace Jadyx {
namespace {
constexpr auto UiLanguageKey = L"system_ui_language";
constexpr auto GameLanguageKey = L"system_game_language";
constexpr auto RegionKey = L"system_region";
constexpr auto UserNameKey = L"system_user_name";
constexpr auto PerformanceStatsKey = L"system_show_performance_stats";
constexpr auto AdultModsKey = L"system_show_adult_mods";
constexpr auto PulseSessionsKey = L"system_pulse_sessions";
constexpr auto PlayTimeKey = L"system_track_play_time";
constexpr auto MultipleControllersKey = L"system_multiple_controllers";
constexpr auto OfflineModeKey = L"system_offline_mode";
constexpr auto MemoryEditWarnedKey = L"system_memory_edit_warned";
constexpr auto GraphicsUpscalingKey = L"graphics_upscaling";
constexpr auto GraphicsFrameGenKey = L"graphics_frame_gen";
constexpr auto GraphicsShadersKey = L"graphics_shaders";
constexpr auto GraphicsGpuFenceKey = L"graphics_gpu_fence";
constexpr auto GraphicsAsyncRecordKey = L"graphics_async_record";
constexpr auto GraphicsCpuWriteQueueKey = L"graphics_cpu_write_queue";

/// Saved as the enum's value, or -1 for "follow the system". A value that is not a selectable
/// option (an older or newer build's) also falls back to the system.
template <typename T>
std::optional<T> LoadChoice(const winrt::Windows::Foundation::Collections::IPropertySet& values,
                            const wchar_t* key, std::span<const T> options) {
    if (!values.HasKey(key)) return std::nullopt;
    const auto raw = winrt::unbox_value<int32_t>(values.Lookup(key));
    for (const T option : options)
        if (static_cast<int32_t>(option) == raw) return option;
    return std::nullopt;
}
template <typename T>
int32_t ChoiceValue(std::optional<T> choice) {
    return choice ? static_cast<int32_t>(*choice) : -1;
}
} // namespace

SystemSettings LoadSystemSettings() {
    SystemSettings settings;
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        if (values.HasKey(UiLanguageKey)) {
            settings.ui_language = winrt::unbox_value<winrt::hstring>(values.Lookup(UiLanguageKey)).c_str();
            if (!I18n::FindUiLanguage(settings.ui_language)) settings.ui_language.clear();
        }
        settings.game_language = LoadChoice(values, GameLanguageKey, SelectableGameLanguages());
        settings.region = LoadChoice(values, RegionKey, SelectableRegions());
        if (values.HasKey(UserNameKey))
            settings.user_name = SanitizeUserName(
                winrt::unbox_value<winrt::hstring>(values.Lookup(UserNameKey)).c_str());
        if (values.HasKey(PerformanceStatsKey)) settings.show_performance_stats = winrt::unbox_value<bool>(values.Lookup(PerformanceStatsKey));
        if (values.HasKey(AdultModsKey)) settings.show_adult_mods = winrt::unbox_value<bool>(values.Lookup(AdultModsKey));
        if (values.HasKey(PulseSessionsKey)) settings.pulse_sessions = winrt::unbox_value<bool>(values.Lookup(PulseSessionsKey));
        if (values.HasKey(PlayTimeKey)) settings.track_play_time = winrt::unbox_value<bool>(values.Lookup(PlayTimeKey));
        if (values.HasKey(MultipleControllersKey))
            settings.multiple_controllers = winrt::unbox_value<bool>(values.Lookup(MultipleControllersKey));
        if (values.HasKey(OfflineModeKey)) settings.offline_mode = winrt::unbox_value<bool>(values.Lookup(OfflineModeKey));
        if (values.HasKey(MemoryEditWarnedKey)) settings.memory_edit_warned = winrt::unbox_value<bool>(values.Lookup(MemoryEditWarnedKey));
    } catch (...) { /* Missing or invalid values keep following the system. */ }
    return settings;
}

bool SaveSystemSettings(const SystemSettings& settings) {
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        values.Insert(UiLanguageKey, winrt::box_value(winrt::hstring{settings.ui_language}));
        values.Insert(GameLanguageKey, winrt::box_value(ChoiceValue(settings.game_language)));
        values.Insert(RegionKey, winrt::box_value(ChoiceValue(settings.region)));
        values.Insert(UserNameKey, winrt::box_value(winrt::hstring{SanitizeUserName(settings.user_name)}));
        values.Insert(PerformanceStatsKey, winrt::box_value(settings.show_performance_stats));
        values.Insert(AdultModsKey, winrt::box_value(settings.show_adult_mods));
        values.Insert(PulseSessionsKey, winrt::box_value(settings.pulse_sessions));
        values.Insert(PlayTimeKey, winrt::box_value(settings.track_play_time));
        values.Insert(MultipleControllersKey, winrt::box_value(settings.multiple_controllers));
        values.Insert(OfflineModeKey, winrt::box_value(settings.offline_mode));
        values.Insert(MemoryEditWarnedKey, winrt::box_value(settings.memory_edit_warned));
        return true;
    } catch (...) { return false; }
}

GraphicsSettings LoadGraphicsSettings() {
    GraphicsSettings settings;
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        if (const auto upscaling = LoadChoice(values, GraphicsUpscalingKey, SelectableUpscalings()))
            settings.upscaling = *upscaling;
        if (values.HasKey(GraphicsFrameGenKey))
            settings.frame_gen = winrt::unbox_value<bool>(values.Lookup(GraphicsFrameGenKey));
        if (const auto shaders = LoadChoice(values, GraphicsShadersKey, SelectableShaderBuilds()))
            settings.shaders = *shaders;
        if (const auto gpu_fence = LoadChoice(values, GraphicsGpuFenceKey, SelectableGpuFences()))
            settings.gpu_fence = *gpu_fence;
        if (values.HasKey(GraphicsAsyncRecordKey))
            settings.async_record = winrt::unbox_value<bool>(values.Lookup(GraphicsAsyncRecordKey));
        if (values.HasKey(GraphicsCpuWriteQueueKey))
            settings.cpu_write_queue = winrt::unbox_value<bool>(values.Lookup(GraphicsCpuWriteQueueKey));
    } catch (...) { /* Missing or invalid values keep the defaults. */ }
    // Saved by hand or by an older build with both on: FSR wins, as on a first install.
    if (IsFsr(settings.upscaling)) settings.frame_gen = false;
    return settings;
}

bool SaveGraphicsSettings(const GraphicsSettings& settings) {
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        values.Insert(GraphicsUpscalingKey, winrt::box_value(static_cast<int32_t>(settings.upscaling)));
        values.Insert(GraphicsFrameGenKey, winrt::box_value(settings.frame_gen));
        values.Insert(GraphicsShadersKey, winrt::box_value(static_cast<int32_t>(settings.shaders)));
        values.Insert(GraphicsGpuFenceKey, winrt::box_value(static_cast<int32_t>(settings.gpu_fence)));
        values.Insert(GraphicsAsyncRecordKey, winrt::box_value(settings.async_record));
        values.Insert(GraphicsCpuWriteQueueKey, winrt::box_value(settings.cpu_write_queue));
        return true;
    } catch (...) { return false; }
}

SystemLocale DetectSystemLocale() {
    SystemLocale locale;
    using winrt::Windows::System::UserProfile::GlobalizationPreferences;
    try {
        for (const auto& language : GlobalizationPreferences::Languages())
            locale.languages.emplace_back(language.c_str());
    } catch (const winrt::hresult_error&) {}
    try {
        locale.region = GlobalizationPreferences::HomeGeographicRegion().c_str();
    } catch (const winrt::hresult_error&) {}
    return locale;
}

namespace {
winrt::hstring GameOfflineModeKey(std::uint64_t program_id) {
    wchar_t key[40];
    swprintf(key, std::size(key), L"game_offline_mode_%016llx", static_cast<unsigned long long>(program_id));
    return key;
}
winrt::hstring GameGraphicsKey(const wchar_t* field, std::uint64_t program_id) {
    wchar_t key[48];
    swprintf(key, std::size(key), L"game_graphics_%ls_%016llx", field, static_cast<unsigned long long>(program_id));
    return key;
}

/// Writes a game's override, or removes it so the game follows Settings > Graphics.
template <typename T>
void SaveOverride(const winrt::Windows::Foundation::Collections::IPropertySet& values, const winrt::hstring& key,
                  std::optional<T> value) {
    if (!value) {
        if (values.HasKey(key)) values.Remove(key);
    } else if constexpr (std::is_same_v<T, bool>) {
        values.Insert(key, winrt::box_value(*value));
    } else {
        values.Insert(key, winrt::box_value(static_cast<int32_t>(*value)));
    }
}
} // namespace

GameGraphics LoadGameGraphics(std::uint64_t program_id) {
    GameGraphics graphics;
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        const auto upscaling = GameGraphicsKey(L"upscaling", program_id);
        graphics.upscaling = LoadChoice(values, upscaling.c_str(), SelectableUpscalings());
        const auto frame_gen = GameGraphicsKey(L"frame_gen", program_id);
        if (values.HasKey(frame_gen)) graphics.frame_gen = winrt::unbox_value<bool>(values.Lookup(frame_gen));
        const auto shaders = GameGraphicsKey(L"shaders", program_id);
        graphics.shaders = LoadChoice(values, shaders.c_str(), SelectableShaderBuilds());
        const auto gpu_fence = GameGraphicsKey(L"gpu_fence", program_id);
        graphics.gpu_fence = LoadChoice(values, gpu_fence.c_str(), SelectableGpuFences());
        const auto async_record = GameGraphicsKey(L"async_record", program_id);
        if (values.HasKey(async_record)) graphics.async_record = winrt::unbox_value<bool>(values.Lookup(async_record));
        const auto cpu_write_queue = GameGraphicsKey(L"cpu_write_queue", program_id);
        if (values.HasKey(cpu_write_queue)) graphics.cpu_write_queue = winrt::unbox_value<bool>(values.Lookup(cpu_write_queue));
        const auto slow_clock = GameGraphicsKey(L"slow_clock", program_id);
        if (values.HasKey(slow_clock)) graphics.slow_guest_clock = winrt::unbox_value<bool>(values.Lookup(slow_clock));
        const auto exact_reads = GameGraphicsKey(L"exact_gpu_reads", program_id);
        if (values.HasKey(exact_reads)) graphics.exact_gpu_reads = winrt::unbox_value<bool>(values.Lookup(exact_reads));
        auto& advanced = graphics.advanced;
        const auto load = [&](const wchar_t* field, auto& value, auto options) {
            const auto key = GameGraphicsKey(field, program_id);
            if (const auto saved = LoadChoice(values, key.c_str(), options)) value = *saved;
        };
        load(L"anisotropy", advanced.anisotropy, SelectableAnisotropy());
        load(L"texture_cap", advanced.texture_cap_mib, SelectableTextureCaps());
        load(L"gpu_accuracy", advanced.gpu_accuracy, SelectableGpuAccuracy());
        load(L"workers", advanced.shader_workers, SelectableShaderWorkers());
        load(L"console_mode", advanced.console_mode, SelectableConsoleModes());
        const auto full_textures = GameGraphicsKey(L"full_textures", program_id);
        if (values.HasKey(full_textures)) advanced.full_textures = winrt::unbox_value<bool>(values.Lookup(full_textures));
    } catch (...) { /* A missing or invalid value follows Settings > Graphics. */ }
    return graphics;
}

bool SaveGameGraphics(std::uint64_t program_id, const GameGraphics& graphics) {
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        SaveOverride(values, GameGraphicsKey(L"upscaling", program_id), graphics.upscaling);
        SaveOverride(values, GameGraphicsKey(L"frame_gen", program_id), graphics.frame_gen);
        SaveOverride(values, GameGraphicsKey(L"shaders", program_id), graphics.shaders);
        SaveOverride(values, GameGraphicsKey(L"gpu_fence", program_id), graphics.gpu_fence);
        SaveOverride(values, GameGraphicsKey(L"async_record", program_id), graphics.async_record);
        SaveOverride(values, GameGraphicsKey(L"cpu_write_queue", program_id), graphics.cpu_write_queue);
        SaveOverride(values, GameGraphicsKey(L"slow_clock", program_id),
                     graphics.slow_guest_clock ? std::optional<bool>{true} : std::nullopt);
        SaveOverride(values, GameGraphicsKey(L"exact_gpu_reads", program_id),
                     graphics.exact_gpu_reads ? std::optional<bool>{true} : std::nullopt);
        // A default is not saved: a later build can change what the app does with no choice made.
        const auto& advanced = graphics.advanced;
        const GameAdvancedGraphics defaults;
        const auto save = [&]<typename T>(const wchar_t* field, T value, T standard) {
            SaveOverride(values, GameGraphicsKey(field, program_id), value == standard ? std::nullopt : std::optional<T>{value});
        };
        save(L"anisotropy", advanced.anisotropy, defaults.anisotropy);
        save(L"texture_cap", advanced.texture_cap_mib, defaults.texture_cap_mib);
        save(L"gpu_accuracy", advanced.gpu_accuracy, defaults.gpu_accuracy);
        save(L"workers", advanced.shader_workers, defaults.shader_workers);
        save(L"console_mode", advanced.console_mode, defaults.console_mode);
        save(L"full_textures", advanced.full_textures, defaults.full_textures);
        return true;
    } catch (...) { return false; }
}

std::optional<bool> LoadGameOfflineMode(std::uint64_t program_id) {
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        const auto key = GameOfflineModeKey(program_id);
        if (values.HasKey(key)) return winrt::unbox_value<bool>(values.Lookup(key));
    } catch (...) { /* A missing or invalid value follows the system setting. */ }
    return std::nullopt;
}

bool SaveGameOfflineMode(std::uint64_t program_id, std::optional<bool> offline) {
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        const auto key = GameOfflineModeKey(program_id);
        if (offline) values.Insert(key, winrt::box_value(*offline));
        else if (values.HasKey(key)) values.Remove(key);
        return true;
    } catch (...) { return false; }
}

ResolvedSystem ApplySystemSettings() {
    const auto locale = DetectSystemLocale();
    const auto settings = LoadSystemSettings();
    VideoCore::performance_overlay_enabled.store(settings.show_performance_stats, std::memory_order_relaxed);
    ApplyUpscaling(LoadGraphicsSettings().upscaling);
    const auto resolved = ResolveSystem(settings, locale);
    I18n::SetUiLanguage(*resolved.ui_language);
    Settings::values.language_index = resolved.game_language;
    Settings::values.region_index = resolved.region;
    // The console's nickname, which some games show; Eden's default is its own name.
    Settings::values.device_name.SetValue("Jadyx");
    Settings::values.airplane_mode.SetValue(settings.offline_mode);
    LOG_INFO(Frontend, "System: Windows language {}, region {}; interface {}, game language {}, region {}",
             locale.languages.empty() ? std::string{"(none)"} : I18n::ToUtf8(locale.languages.front()),
             locale.region.empty() ? std::string{"(none)"} : I18n::ToUtf8(locale.region),
             I18n::ToUtf8(resolved.ui_language->tag), Settings::CanonicalizeEnum(resolved.game_language),
             Settings::CanonicalizeEnum(resolved.region));
    return resolved;
}

void ApplyUpscaling(Upscaling upscaling) {
    const UpscalingSettings values = ToSettings(upscaling);
    Settings::values.scaling_filter.SetValue(values.filter);
    Settings::values.fsr_sharpening_slider.SetValue(values.fsr_sharpening_slider);
}

bool ApplyUserName(Service::Account::ProfileManager& profiles, std::wstring_view name) {
    // No saved name is the default one: a profile renamed before goes back to it.
    auto clean = SanitizeUserName(name);
    if (clean.empty()) clean = DefaultUserName;
    const auto index = static_cast<std::size_t>(std::clamp<s32>(
        Settings::values.current_user.GetValue(), 0, static_cast<s32>(Service::Account::MAX_USERS) - 1));
    const auto uuid = profiles.GetUser(index);
    Service::Account::ProfileBase base{};
    if (!uuid || !profiles.GetProfileBase(*uuid, base)) return false;
    // Ten UTF-16 units are at most 30 UTF-8 bytes, inside the 32-byte field with room to spare.
    const auto utf8 = I18n::ToUtf8(clean);
    Service::Account::ProfileUsername username{};
    std::copy_n(utf8.begin(), std::min(utf8.size(), username.size()), username.begin());
    if (base.username == username) return true;
    base.username = username;
    const bool saved = profiles.SetProfileBase(*uuid, base);
    LOG_INFO(Frontend, "System: profile {} renamed to '{}'{}", index, utf8, saved ? "" : " (failed)");
    return saved;
}
} // namespace Jadyx
