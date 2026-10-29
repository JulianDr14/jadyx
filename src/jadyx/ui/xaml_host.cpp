// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/ui/xaml_host.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <dxgi1_3.h>
#include <windows.ui.xaml.media.dxinterop.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Search.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Security.Cryptography.Certificates.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>

#include "common/logging.h"
#include "jadyx/i18n/localization.h"
#include "video_core/renderer_d3d12/d3d12_menu_image.h"

namespace Jadyx::XamlHost {
namespace {

using Microsoft::WRL::ComPtr;
namespace Controls = winrt::Windows::UI::Xaml::Controls;
namespace Http = winrt::Windows::Web::Http;
namespace HttpFilters = winrt::Windows::Web::Http::Filters;
namespace Storage = winrt::Windows::Storage;
namespace Xaml = winrt::Windows::UI::Xaml;
using winrt::Windows::Foundation::Uri;
using winrt::Windows::UI::Core::CoreDispatcher;
using winrt::Windows::UI::Core::CoreDispatcherPriority;
using winrt::Windows::UI::Xaml::Visibility;

constexpr std::uint32_t MaxVideoFiles = 10000;
constexpr std::uint32_t DownloadChunkSize = 64 * 1024;
constexpr std::array<std::wstring_view, 14> VideoExtensions{
    L".mp4", L".m4v", L".mov", L".wmv", L".avi", L".mkv", L".webm",
    L".3gp", L".mpg", L".mpeg", L".ts", L".m2ts", L".mts", L".vob"};
constexpr wchar_t DownloadFolderToken[] = L"jadyx-browser-downloads";
constexpr wchar_t VideoFolderToken[] = L"jadyx-video-folder";
constexpr wchar_t DownloadFolderSetting[] = L"browser_download_folder";
constexpr wchar_t VideoFolderSetting[] = L"video_folder";
constexpr wchar_t UnsafeDownloadsSetting[] = L"browser_ignore_certificate_errors";

std::wstring Text(I18n::Text id) {
    return std::wstring{I18n::Tr(id)};
}

struct Host {
    Host() : http_filter{}, http{http_filter} {}

    Controls::Grid root{nullptr};
    Controls::SwapChainPanel render_panel{nullptr};
    Controls::Grid browser_page{nullptr};
    Controls::Grid video_page{nullptr};
    Controls::WebView browser{nullptr};
    Controls::TextBox address{nullptr};
    Controls::TextBlock browser_status{nullptr};
    Controls::Button unsafe_downloads_button{nullptr};
    Controls::Canvas cursor_canvas{nullptr};
    Controls::Ellipse cursor{nullptr};
    Controls::ListView video_list{nullptr};
    Controls::TextBlock video_status{nullptr};
    Controls::MediaPlayerElement media_element{nullptr};
    std::array<Controls::Control, 8> browser_controls{};
    winrt::Windows::Media::Playback::MediaPlayer media_player{nullptr};
    CoreDispatcher dispatcher{nullptr};
    ComPtr<ISwapChainPanelNative> native_panel;
    std::vector<Storage::StorageFile> video_files;
    std::optional<std::size_t> current_video;
    HttpFilters::HttpBaseProtocolFilter http_filter{nullptr};
    Http::HttpClient http{nullptr};
    float pointer_x{320}, pointer_y{180};
    std::chrono::steady_clock::time_point pointer_update{};
    bool browser_open{};
    bool video_open{};
    bool downloading{};
    bool cancel_download{};
    bool ignore_download_certificate_errors{};
    bool security_setting_failed{};
    bool security_filter_safe{true};
    int browser_focus{8};

    Controls::Button Button(std::wstring_view label) {
        Controls::Button button;
        button.Content(winrt::box_value(winrt::hstring{label}));
        button.FontSize(18);
        button.Padding({16, 8, 16, 8});
        button.Margin({4, 4, 4, 4});
        return button;
    }

    Controls::TextBlock Label(std::wstring_view label, double size = 20) {
        Controls::TextBlock text;
        text.Text(label);
        text.FontSize(size);
        text.VerticalAlignment(Xaml::VerticalAlignment::Center);
        text.Margin({12, 4, 12, 4});
        return text;
    }

    void ConfigureCertificateErrors(bool ignore) {
        using winrt::Windows::Security::Cryptography::Certificates::ChainValidationResult;
        auto errors = http_filter.IgnorableServerCertificateErrors();
        errors.Clear();
        if (!ignore) return;
        constexpr ChainValidationResult IgnoredErrors[] = {
            ChainValidationResult::Untrusted,
            ChainValidationResult::Expired,
            ChainValidationResult::IncompleteChain,
            ChainValidationResult::WrongUsage,
            ChainValidationResult::InvalidName,
            ChainValidationResult::InvalidCertificateAuthorityPolicy,
            ChainValidationResult::BasicConstraintsError,
            ChainValidationResult::RevocationInformationMissing,
            ChainValidationResult::RevocationFailure,
            ChainValidationResult::OtherErrors,
        };
        for (const auto error : IgnoredErrors) errors.Append(error);
    }

    void LoadSecuritySetting() {
        try {
            const auto values = Storage::ApplicationData::Current().LocalSettings().Values();
            if (!values.HasKey(UnsafeDownloadsSetting)) return;
            const bool enabled = winrt::unbox_value<bool>(values.Lookup(UnsafeDownloadsSetting));
            ConfigureCertificateErrors(enabled);
            ignore_download_certificate_errors = enabled;
        } catch (const winrt::hresult_error& error) {
            LOG_WARNING(Frontend, "Could not load browser security setting: 0x{:08X}",
                        static_cast<std::uint32_t>(error.code().value));
            security_setting_failed = true;
            try {
                ConfigureCertificateErrors(false);
            } catch (const winrt::hresult_error& reset_error) {
                security_filter_safe = false;
                LOG_ERROR(Frontend, "Could not clear browser certificate exceptions: 0x{:08X}",
                          static_cast<std::uint32_t>(reset_error.code().value));
            }
        }
    }

    void SetBrowserStatus(std::wstring status = {}) {
        std::wstring message = Text(ignore_download_certificate_errors
            ? I18n::Text::BrowserUnsafeDownloadsEnabled
            : I18n::Text::BrowserUnsafeDownloadsDisabled);
        if (security_setting_failed) {
            message += L"\n" + Text(I18n::Text::BrowserSecuritySettingFailed);
        }
        if (!status.empty()) message += L"\n" + status;
        browser_status.Text(message);
    }

    void UpdateUnsafeDownloadsButton() {
        if (!unsafe_downloads_button) return;
        unsafe_downloads_button.Content(winrt::box_value(Text(ignore_download_certificate_errors
            ? I18n::Text::BrowserUnsafeDownloadsOn
            : I18n::Text::BrowserUnsafeDownloadsOff)));
    }

    void ToggleUnsafeDownloads() {
        if (downloading) {
            SetBrowserStatus(Text(I18n::Text::BrowserUnsafeDownloadsBusy));
            return;
        }
        const bool enabled = !ignore_download_certificate_errors;
        try {
            ConfigureCertificateErrors(enabled);
            auto values = Storage::ApplicationData::Current().LocalSettings().Values();
            values.Insert(UnsafeDownloadsSetting, winrt::box_value(enabled));
            ignore_download_certificate_errors = enabled;
            security_filter_safe = true;
            security_setting_failed = false;
            UpdateUnsafeDownloadsButton();
            SetBrowserStatus();
        } catch (const winrt::hresult_error& error) {
            LOG_WARNING(Frontend, "Could not update browser security setting: 0x{:08X}",
                        static_cast<std::uint32_t>(error.code().value));
            try {
                ConfigureCertificateErrors(ignore_download_certificate_errors);
                security_filter_safe = true;
            } catch (const winrt::hresult_error& restore_error) {
                security_filter_safe = false;
                LOG_ERROR(Frontend, "Could not restore secure browser downloads: 0x{:08X}",
                          static_cast<std::uint32_t>(restore_error.code().value));
            }
            security_setting_failed = true;
            SetBrowserStatus(Text(I18n::Text::BrowserUnsafeDownloadsSaveFailed));
        }
    }

    void SetPage(bool browser_visible, bool video_visible) {
        browser_open = browser_visible;
        video_open = video_visible;
        browser_page.Visibility(browser_visible ? Visibility::Visible : Visibility::Collapsed);
        video_page.Visibility(video_visible ? Visibility::Visible : Visibility::Collapsed);
        render_panel.Visibility(browser_visible || video_visible ? Visibility::Collapsed : Visibility::Visible);
        if (!browser_visible && !video_visible) {
            media_player.Pause();
        }
    }

    void Navigate(std::wstring value) {
        value.erase(value.begin(), std::find_if(value.begin(), value.end(),
            [](wchar_t c) { return !std::iswspace(c); }));
        value.erase(std::find_if(value.rbegin(), value.rend(),
            [](wchar_t c) { return !std::iswspace(c); }).base(), value.end());
        if (value.empty()) return;
        try {
            if (value.find(L"://") == std::wstring::npos) {
                if (value.find(L' ') != std::wstring::npos || value.find(L'.') == std::wstring::npos) {
                    value = L"https://www.bing.com/search?q=" + std::wstring{Uri::EscapeComponent(value)};
                } else {
                    value = L"https://" + value;
                }
            }
            const Uri uri{value};
            if (uri.SchemeName() != L"http" && uri.SchemeName() != L"https") {
                SetBrowserStatus(Text(I18n::Text::BrowserHttpOnly));
                return;
            }
            address.Text(uri.AbsoluteUri());
            browser.Navigate(uri);
            SetBrowserStatus();
        } catch (const winrt::hresult_error&) {
            SetBrowserStatus(Text(I18n::Text::BrowserInvalidAddress));
        }
    }

    void InjectPageActions() {
        static constexpr wchar_t script[] =
            L"(function(){if(window.__jadyxHooks)return;window.__jadyxHooks=1;"
            L"document.addEventListener('click',function(e){var a=e.target.closest&&e.target.closest('a');"
            L"if(!a||!a.href)return;var u=a.href;if(a.hasAttribute('download')||"
            L"/\\.(pdf|zip|7z|rar|tar|gz|nsp|xci|nro|mp4|m4v|mov|wmv|avi|mkv|webm|"
            L"mp3|wav|flac|jpg|jpeg|png|gif|webp|exe|msi|appx|msix|iso|epub|docx|xlsx|pptx)"
            L"(?:[?#]|$)/i.test(u)){"
            L"e.preventDefault();window.external.notify('jadyx-download:'+u);}},true);})();";
        browser.InvokeScriptAsync(L"eval", {script});
    }

    winrt::fire_and_forget RestoreDownloadFolder() {
        try {
            const auto values = Storage::ApplicationData::Current().LocalSettings().Values();
            if (values.HasKey(DownloadFolderSetting)) {
                const auto token = winrt::unbox_value<winrt::hstring>(values.Lookup(DownloadFolderSetting));
                auto folder = co_await Storage::AccessCache::StorageApplicationPermissions::FutureAccessList()
                                  .GetFolderAsync(token);
                SetBrowserStatus(Text(I18n::Text::BrowserDownloadFolder) + L": " +
                                 std::wstring{folder.Name()});
                co_return;
            }
            SetBrowserStatus(Text(I18n::Text::BrowserDownloadDefault));
        } catch (const winrt::hresult_error&) {
            SetBrowserStatus(Text(I18n::Text::BrowserFolderRestoreFailed));
        }
    }

    winrt::fire_and_forget PickDownloadFolder() {
        try {
            Storage::Pickers::FolderPicker picker;
            picker.SuggestedStartLocation(Storage::Pickers::PickerLocationId::ComputerFolder);
            picker.FileTypeFilter().Append(L"*");
            auto folder = co_await picker.PickSingleFolderAsync();
            if (!folder) co_return;
            Storage::AccessCache::StorageApplicationPermissions::FutureAccessList()
                .AddOrReplace(DownloadFolderToken, folder);
            Storage::ApplicationData::Current().LocalSettings().Values().Insert(
                DownloadFolderSetting, winrt::box_value(DownloadFolderToken));
            SetBrowserStatus(Text(I18n::Text::BrowserDownloadFolder) + L": " +
                             std::wstring{folder.Name()});
        } catch (const winrt::hresult_error&) {
            SetBrowserStatus(Text(I18n::Text::BrowserFolderPickFailed));
        }
    }

    winrt::Windows::Foundation::IAsyncOperation<Storage::StorageFolder> GetDownloadFolder() {
        const auto values = Storage::ApplicationData::Current().LocalSettings().Values();
        if (values.HasKey(DownloadFolderSetting)) {
            const auto token = winrt::unbox_value<winrt::hstring>(values.Lookup(DownloadFolderSetting));
            co_return co_await Storage::AccessCache::StorageApplicationPermissions::FutureAccessList()
                             .GetFolderAsync(token);
        }
        co_return co_await Storage::ApplicationData::Current().LocalFolder()
                         .CreateFolderAsync(L"Downloads", Storage::CreationCollisionOption::OpenIfExists);
    }

    static std::wstring FileNameFromUri(const Uri& uri) {
        auto path = std::wstring{Uri::UnescapeComponent(uri.Path())};
        const auto slash = path.find_last_of(L'/');
        auto name = slash == std::wstring::npos ? path : path.substr(slash + 1);
        if (name.empty()) name = L"download";
        for (auto& c : name) {
            if (c < 32 || std::wstring_view{L"\\/:*?\"<>|"}.find(c) != std::wstring_view::npos) c = L'_';
        }
        while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
        if (name.empty()) name = L"download";
        if (name.size() > 128) name.resize(128);
        return name;
    }

    winrt::fire_and_forget Download(Uri uri) {
        if (!security_filter_safe) {
            SetBrowserStatus(Text(I18n::Text::BrowserSecuritySettingFailed));
            co_return;
        }
        if (downloading) return;
        downloading = true;
        cancel_download = false;
        Storage::StorageFile file{nullptr};
        bool failed = false;
        try {
            SetBrowserStatus(Text(I18n::Text::BrowserDownloading));
            auto folder = co_await GetDownloadFolder();
            file = co_await folder.CreateFileAsync(FileNameFromUri(uri),
                Storage::CreationCollisionOption::GenerateUniqueName);
            auto response = co_await http.GetAsync(uri, winrt::Windows::Web::Http::HttpCompletionOption::ResponseHeadersRead);
            response.EnsureSuccessStatusCode();
            auto input = co_await response.Content().ReadAsInputStreamAsync();
            auto output = co_await file.OpenAsync(Storage::FileAccessMode::ReadWrite);
            winrt::Windows::Storage::Streams::DataReader reader{input};
            winrt::Windows::Storage::Streams::DataWriter writer{output};
            std::array<std::uint8_t, DownloadChunkSize> bytes{};
            while (!cancel_download) {
                const auto count = co_await reader.LoadAsync(DownloadChunkSize);
                if (!count) break;
                reader.ReadBytes({bytes.data(), bytes.data() + count});
                writer.WriteBytes({bytes.data(), bytes.data() + count});
                co_await writer.StoreAsync();
            }
            co_await writer.FlushAsync();
            reader.DetachStream();
            writer.DetachStream();
        } catch (const winrt::hresult_error& error) {
            LOG_WARNING(Frontend, "Browser download failed: 0x{:08X}",
                        static_cast<std::uint32_t>(error.code().value));
            failed = true;
        }
        if ((failed || cancel_download) && file) {
            try {
                co_await file.DeleteAsync();
            } catch (const winrt::hresult_error& error) {
                LOG_WARNING(Frontend, "Could not remove incomplete browser download: 0x{:08X}",
                            static_cast<std::uint32_t>(error.code().value));
            }
        }
        if (cancel_download) {
            SetBrowserStatus(Text(I18n::Text::BrowserDownloadCancelled));
        } else if (failed) {
            SetBrowserStatus(Text(I18n::Text::BrowserDownloadFailed));
        } else if (file) {
            SetBrowserStatus(Text(I18n::Text::BrowserDownloadComplete) + L": " +
                             std::wstring{file.Name()});
        }
        downloading = false;
    }

    winrt::fire_and_forget PickVideoFolder() {
        try {
            Storage::Pickers::FolderPicker picker;
            picker.SuggestedStartLocation(Storage::Pickers::PickerLocationId::ComputerFolder);
            picker.FileTypeFilter().Append(L"*");
            auto folder = co_await picker.PickSingleFolderAsync();
            if (!folder) co_return;
            Storage::AccessCache::StorageApplicationPermissions::FutureAccessList()
                .AddOrReplace(VideoFolderToken, folder);
            Storage::ApplicationData::Current().LocalSettings().Values().Insert(
                VideoFolderSetting, winrt::box_value(VideoFolderToken));
            co_await ScanVideos(folder);
        } catch (const winrt::hresult_error&) {
            video_status.Text(Text(I18n::Text::VideoFolderPickFailed));
        }
    }

    winrt::fire_and_forget RestoreVideoFolder() {
        const auto values = Storage::ApplicationData::Current().LocalSettings().Values();
        if (!values.HasKey(VideoFolderSetting)) {
            video_status.Text(Text(I18n::Text::VideoChooseFolder));
            co_return;
        }
        try {
            const auto token = winrt::unbox_value<winrt::hstring>(values.Lookup(VideoFolderSetting));
            auto folder = co_await Storage::AccessCache::StorageApplicationPermissions::FutureAccessList()
                              .GetFolderAsync(token);
            co_await ScanVideos(folder);
        } catch (const winrt::hresult_error&) {
            video_status.Text(Text(I18n::Text::VideoFolderRestoreFailed));
        }
    }

    winrt::Windows::Foundation::IAsyncAction ScanVideos(Storage::StorageFolder folder) {
        video_status.Text(Text(I18n::Text::VideoScanning));
        media_player.Pause();
        current_video.reset();
        video_files.clear();
        video_list.Items().Clear();
        std::vector<winrt::hstring> extensions;
        for (const auto extension : VideoExtensions)
            extensions.emplace_back(extension);
        winrt::Windows::Storage::Search::QueryOptions options{
            winrt::Windows::Storage::Search::CommonFileQuery::OrderByName, extensions};
        options.FolderDepth(winrt::Windows::Storage::Search::FolderDepth::Deep);
        const auto query = folder.CreateFileQueryWithOptions(options);
        const auto count = co_await query.GetItemCountAsync();
        const auto files = co_await query.GetFilesAsync(0, std::min(count, MaxVideoFiles));
        for (const auto& file : files) {
            video_files.push_back(file);
            Controls::ListViewItem item;
            item.Content(winrt::box_value(file.Name()));
            video_list.Items().Append(item);
        }
        if (!video_files.empty()) video_list.SelectedIndex(0);
        if (count > MaxVideoFiles) {
            video_status.Text(Text(I18n::Text::VideoScanLimit));
        } else if (video_files.empty()) {
            video_status.Text(Text(I18n::Text::VideoNoFiles));
        } else {
            video_status.Text(Text(I18n::Text::VideoFolder) + L": " +
                              std::wstring{folder.Name()} + L" · " +
                              std::to_wstring(video_files.size()));
        }
    }

    void PlayVideo(std::size_t index) {
        if (index >= video_files.size()) return;
        try {
            media_player.Source(winrt::Windows::Media::Core::MediaSource::CreateFromStorageFile(video_files[index]));
            current_video = index;
            media_player.Play();
            video_status.Text(std::wstring{video_files[index].Name()});
        } catch (const winrt::hresult_error&) {
            video_status.Text(Text(I18n::Text::VideoPlaybackFailed));
        }
    }

    void ToggleVideoPlayback() {
        const auto selected = video_list.SelectedIndex();
        if (selected < 0) return;
        const auto index = static_cast<std::size_t>(selected);
        if (current_video != index) {
            PlayVideo(index);
        } else if (media_player.PlaybackSession().PlaybackState() ==
                   winrt::Windows::Media::Playback::MediaPlaybackState::Playing) {
            media_player.Pause();
        } else {
            media_player.Play();
        }
    }

    void MoveVideoSelection(int amount) {
        if (video_files.empty()) return;
        const auto current = std::max(0, video_list.SelectedIndex());
        const auto next = std::clamp(current + amount, 0, static_cast<int>(video_files.size() - 1));
        video_list.SelectedIndex(next);
    }

    void SetBrowserFocus(int index) {
        browser_focus = std::clamp(index, 0, static_cast<int>(browser_controls.size()));
        if (browser_focus == static_cast<int>(browser_controls.size())) {
            browser.Focus(Xaml::FocusState::Programmatic);
        } else {
            browser_controls[static_cast<std::size_t>(browser_focus)].Focus(Xaml::FocusState::Programmatic);
        }
    }

    void ActivateBrowserControl() {
        switch (browser_focus) {
        case 0:
            if (browser.CanGoBack()) browser.GoBack();
            else SetPage(false, false);
            break;
        case 1:
            address.Focus(Xaml::FocusState::Programmatic);
            break;
        case 2:
            Navigate(std::wstring{address.Text()});
            break;
        case 3:
            DownloadCurrentAddress();
            break;
        case 4:
            PickDownloadFolder();
            break;
        case 5:
            ToggleUnsafeDownloads();
            break;
        case 6:
            if (downloading) {
                cancel_download = true;
                http.CancelPendingRequests();
            }
            break;
        case 7:
            SetPage(false, false);
            break;
        default:
            ClickBrowser();
            break;
        }
    }

    void DownloadCurrentAddress() {
        try {
            std::wstring value{address.Text()};
            if (value.find(L"://") == std::wstring::npos) value = L"https://" + value;
            const Uri uri{value};
            if (uri.SchemeName() != L"http" && uri.SchemeName() != L"https") {
                SetBrowserStatus(Text(I18n::Text::BrowserHttpOnly));
                return;
            }
            Download(uri);
        } catch (const winrt::hresult_error&) {
            SetBrowserStatus(Text(I18n::Text::BrowserInvalidAddress));
        }
    }

    void ClickBrowser() {
        if (!browser_open) return;
        const double width = std::max(1.0, browser.ActualWidth());
        const double height = std::max(1.0, browser.ActualHeight());
        const auto x = pointer_x * 100.0 / width;
        const auto y = pointer_y * 100.0 / height;
        const auto script = L"(function(){var x=document.documentElement.clientWidth*" +
            std::to_wstring(x / 100.0) + L",y=document.documentElement.clientHeight*" +
            std::to_wstring(y / 100.0) + L";var e=document.elementFromPoint(x,y);if(!e)return;"
            L"var d={bubbles:true,cancelable:true,clientX:x,clientY:y,button:0,buttons:1};"
            L"e.dispatchEvent(new MouseEvent('mousemove',d));e.dispatchEvent(new MouseEvent('mousedown',d));"
            L"d.buttons=0;e.dispatchEvent(new MouseEvent('mouseup',d));e.click();})();";
        browser.InvokeScriptAsync(L"eval", {script});
    }

    void MoveBrowserPointer(float x, float y) {
        if (!browser_open) return;
        constexpr float deadzone = 0.18f;
        const auto scale_axis = [=](float value) {
            const float magnitude = std::abs(value);
            return magnitude <= deadzone ? 0.0f :
                std::copysign((magnitude - deadzone) / (1.0f - deadzone), value);
        };
        x = scale_axis(x);
        y = scale_axis(y);
        if (x == 0.0f && y == 0.0f) return;
        const auto now = std::chrono::steady_clock::now();
        if (pointer_update.time_since_epoch().count() != 0 &&
            now - pointer_update < std::chrono::milliseconds{33}) return;
        const float elapsed = pointer_update.time_since_epoch().count() == 0 ? 1.0f / 30.0f :
            std::clamp(std::chrono::duration<float>(now - pointer_update).count(), 0.0f, 0.05f);
        pointer_update = now;
        const float width = static_cast<float>(std::max(1.0, browser.ActualWidth()));
        const float height = static_cast<float>(std::max(1.0, browser.ActualHeight()));
        pointer_x = std::clamp(pointer_x + x * 900.0f * elapsed, 0.0f, width);
        pointer_y = std::clamp(pointer_y - y * 900.0f * elapsed, 0.0f, height);
        Controls::Canvas::SetLeft(cursor, pointer_x);
        Controls::Canvas::SetTop(cursor, pointer_y);
        const auto client_x = pointer_x / width;
        const auto client_y = pointer_y / height;
        const auto script = L"(function(){var x=document.documentElement.clientWidth*" +
            std::to_wstring(client_x) + L",y=document.documentElement.clientHeight*" +
            std::to_wstring(client_y) + L",e=document.elementFromPoint(x,y);if(e)e.dispatchEvent("
            L"new MouseEvent('mousemove',{bubbles:true,clientX:x,clientY:y}));})();";
        browser.InvokeScriptAsync(L"eval", {script});
    }

    void Build() {
        LoadSecuritySetting();
        root = Controls::Grid{};
        render_panel = Controls::SwapChainPanel{};
        root.Children().Append(render_panel);

        browser_page = Controls::Grid{};
        browser_page.Background(winrt::Windows::UI::Xaml::Media::SolidColorBrush{
            winrt::Windows::UI::ColorHelper::FromArgb(255, 24, 24, 24)});
        browser_page.RowDefinitions().Append(Controls::RowDefinition{});
        browser_page.RowDefinitions().Append(Controls::RowDefinition{});
        browser_page.RowDefinitions().GetAt(0).Height({156, Xaml::GridUnitType::Pixel});
        browser_page.RowDefinitions().GetAt(1).Height({1, Xaml::GridUnitType::Star});
        Controls::Grid toolbar;
        toolbar.RowDefinitions().Append(Controls::RowDefinition{});
        toolbar.RowDefinitions().Append(Controls::RowDefinition{});
        toolbar.RowDefinitions().Append(Controls::RowDefinition{});
        toolbar.ColumnDefinitions().Append(Controls::ColumnDefinition{});
        toolbar.ColumnDefinitions().Append(Controls::ColumnDefinition{});
        toolbar.ColumnDefinitions().Append(Controls::ColumnDefinition{});
        toolbar.ColumnDefinitions().Append(Controls::ColumnDefinition{});
        toolbar.ColumnDefinitions().GetAt(0).Width({160, Xaml::GridUnitType::Pixel});
        toolbar.ColumnDefinitions().GetAt(1).Width({1, Xaml::GridUnitType::Star});
        toolbar.ColumnDefinitions().GetAt(2).Width({100, Xaml::GridUnitType::Pixel});
        toolbar.ColumnDefinitions().GetAt(3).Width({140, Xaml::GridUnitType::Pixel});
        Controls::Button back = Button(Text(I18n::Text::Back));
        back.Click([this](auto&&, auto&&) {
            if (browser.CanGoBack()) browser.GoBack();
            else SetPage(false, false);
        });
        Controls::Grid::SetColumn(back, 0);
        Controls::Grid::SetRow(back, 0);
        toolbar.Children().Append(back);
        address = Controls::TextBox{};
        address.MinWidth(240);
        address.FontSize(20);
        address.PlaceholderText(Text(I18n::Text::BrowserAddress));
        address.KeyDown([this](auto&&, winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
            if (args.Key() == winrt::Windows::System::VirtualKey::Enter) Navigate(std::wstring{address.Text()});
        });
        Controls::Grid::SetColumn(address, 1);
        Controls::Grid::SetRow(address, 0);
        toolbar.Children().Append(address);
        auto go = Button(Text(I18n::Text::BrowserGo));
        go.Click([this](auto&&, auto&&) { Navigate(std::wstring{address.Text()}); });
        Controls::Grid::SetColumn(go, 2);
        Controls::Grid::SetRow(go, 0);
        toolbar.Children().Append(go);
        auto download = Button(Text(I18n::Text::BrowserDownload));
        download.Click([this](auto&&, auto&&) { DownloadCurrentAddress(); });
        Controls::Grid::SetColumn(download, 3);
        Controls::Grid::SetRow(download, 0);
        toolbar.Children().Append(download);
        auto folder = Button(Text(I18n::Text::BrowserDownloadFolder));
        folder.Click([this](auto&&, auto&&) { PickDownloadFolder(); });
        Controls::Grid::SetColumn(folder, 0);
        Controls::Grid::SetRow(folder, 1);
        toolbar.Children().Append(folder);
        unsafe_downloads_button = Button(Text(I18n::Text::BrowserUnsafeDownloads));
        unsafe_downloads_button.Click([this](auto&&, auto&&) { ToggleUnsafeDownloads(); });
        Controls::Grid::SetColumn(unsafe_downloads_button, 1);
        Controls::Grid::SetRow(unsafe_downloads_button, 1);
        toolbar.Children().Append(unsafe_downloads_button);
        auto cancel = Button(Text(I18n::Text::Cancel));
        cancel.Click([this](auto&&, auto&&) {
            if (downloading) {
                cancel_download = true;
                http.CancelPendingRequests();
            }
        });
        Controls::Grid::SetColumn(cancel, 2);
        Controls::Grid::SetRow(cancel, 1);
        toolbar.Children().Append(cancel);
        auto close = Button(Text(I18n::Text::Back));
        close.Click([this](auto&&, auto&&) { SetPage(false, false); });
        Controls::Grid::SetColumn(close, 3);
        Controls::Grid::SetRow(close, 1);
        toolbar.Children().Append(close);
        browser_controls = {back, address, go, download, folder, unsafe_downloads_button, cancel, close};
        browser_status = Label({}, 16);
        browser_status.FontSize(14);
        browser_status.TextWrapping(Xaml::TextWrapping::Wrap);
        browser_status.HorizontalAlignment(Xaml::HorizontalAlignment::Left);
        Controls::Grid::SetColumnSpan(browser_status, 4);
        Controls::Grid::SetRow(browser_status, 2);
        toolbar.Children().Append(browser_status);
        UpdateUnsafeDownloadsButton();
        SetBrowserStatus();
        Controls::Grid::SetRow(toolbar, 0);
        browser_page.Children().Append(toolbar);
        browser = Controls::WebView{};
        browser.NavigationCompleted([this](auto&&, auto&&) { InjectPageActions(); });
        browser.NavigationStarting([this](auto&&, auto&& args) {
            const auto uri = args.Uri();
            if (uri.SchemeName() != L"http" && uri.SchemeName() != L"https") {
                args.Cancel(true);
                SetBrowserStatus(Text(I18n::Text::BrowserHttpOnly));
                return;
            }
            address.Text(uri.AbsoluteUri());
        });
        browser.NewWindowRequested([this](auto&&, auto&& args) {
            args.Handled(true);
            browser.Navigate(args.Uri());
        });
        browser.ScriptNotify([this](auto&&, auto&& args) {
            const std::wstring value{args.Value()};
            constexpr std::wstring_view prefix{L"jadyx-download:"};
            if (!value.starts_with(prefix)) return;
            try {
                Uri uri{value.substr(prefix.size())};
                if (uri.SchemeName() != L"http" && uri.SchemeName() != L"https") return;
                Download(std::move(uri));
            } catch (const winrt::hresult_error&) {
                SetBrowserStatus(Text(I18n::Text::BrowserInvalidAddress));
            }
        });
        browser.AllowedScriptNotifyUris().Append(Controls::WebView::AnyScriptNotifyUri());
        Controls::Grid::SetRow(browser, 1);
        browser_page.Children().Append(browser);
        cursor_canvas = Controls::Canvas{};
        cursor_canvas.IsHitTestVisible(false);
        cursor = Controls::Ellipse{};
        cursor.Width(18);
        cursor.Height(18);
        cursor.Fill(winrt::Windows::UI::Xaml::Media::SolidColorBrush{
            winrt::Windows::UI::ColorHelper::FromArgb(255, 255, 255, 255)});
        cursor.Stroke(winrt::Windows::UI::Xaml::Media::SolidColorBrush{
            winrt::Windows::UI::ColorHelper::FromArgb(255, 0, 0, 0)});
        cursor.StrokeThickness(2);
        cursor_canvas.Children().Append(cursor);
        Controls::Grid::SetRow(cursor_canvas, 1);
        browser_page.Children().Append(cursor_canvas);
        browser_page.Visibility(Visibility::Collapsed);
        root.Children().Append(browser_page);

        video_page = Controls::Grid{};
        video_page.Background(winrt::Windows::UI::Xaml::Media::SolidColorBrush{
            winrt::Windows::UI::ColorHelper::FromArgb(255, 24, 24, 24)});
        video_page.RowDefinitions().Append(Controls::RowDefinition{});
        video_page.RowDefinitions().Append(Controls::RowDefinition{});
        video_page.RowDefinitions().GetAt(0).Height({64, Xaml::GridUnitType::Pixel});
        video_page.RowDefinitions().GetAt(1).Height({1, Xaml::GridUnitType::Star});
        Controls::StackPanel video_toolbar;
        video_toolbar.Orientation(Controls::Orientation::Horizontal);
        auto video_back = Button(Text(I18n::Text::Back));
        video_back.Click([this](auto&&, auto&&) { SetPage(false, false); });
        video_toolbar.Children().Append(video_back);
        auto choose_video = Button(Text(I18n::Text::VideoChooseFolder));
        choose_video.Click([this](auto&&, auto&&) { PickVideoFolder(); });
        video_toolbar.Children().Append(choose_video);
        video_status = Label(Text(I18n::Text::VideoChooseFolder), 16);
        video_toolbar.Children().Append(video_status);
        Controls::Grid::SetRow(video_toolbar, 0);
        video_page.Children().Append(video_toolbar);
        Controls::Grid body;
        body.ColumnDefinitions().Append(Controls::ColumnDefinition{});
        body.ColumnDefinitions().Append(Controls::ColumnDefinition{});
        body.ColumnDefinitions().GetAt(0).Width({420, Xaml::GridUnitType::Pixel});
        body.ColumnDefinitions().GetAt(1).Width({1, Xaml::GridUnitType::Star});
        video_list = Controls::ListView{};
        video_list.IsItemClickEnabled(true);
        video_list.ItemClick([this](auto&&, Controls::ItemClickEventArgs const&) {
            const auto selected = video_list.SelectedIndex();
            if (selected >= 0) PlayVideo(static_cast<std::size_t>(selected));
        });
        body.Children().Append(video_list);
        media_element = Controls::MediaPlayerElement{};
        media_element.AreTransportControlsEnabled(true);
        media_player = winrt::Windows::Media::Playback::MediaPlayer{};
        media_element.SetMediaPlayer(media_player);
        media_player.MediaFailed([this](auto&&, winrt::Windows::Media::Playback::MediaPlayerFailedEventArgs const&) {
            video_status.Text(Text(I18n::Text::VideoPlaybackFailed));
        });
        Controls::Grid::SetColumn(media_element, 1);
        body.Children().Append(media_element);
        Controls::Grid::SetRow(body, 1);
        video_page.Children().Append(body);
        video_page.Visibility(Visibility::Collapsed);
        root.Children().Append(video_page);

        winrt::Windows::UI::Xaml::Window::Current().Content(root);
        winrt::Windows::UI::Xaml::Window::Current().Activate();
        winrt::check_hresult(winrt::get_unknown(render_panel)->QueryInterface(IID_PPV_ARGS(&native_panel)));
    }
};

std::unique_ptr<Host> host;

} // namespace

void Initialize() {
    if (host) return;
    host = std::make_unique<Host>();
    host->dispatcher = CoreDispatcher::GetForCurrentThread();
    host->Build();
    D3D12::SetSwapChainAttach([](void* swap_chain) { AttachSwapChain(swap_chain); });
}

void* RenderSurface() noexcept {
    return host ? host->native_panel.Get() : nullptr;
}

bool IsMediaPageOpen() noexcept {
    return host && (host->browser_open || host->video_open);
}

void OpenBrowser() {
    if (!host) return;
    host->SetPage(true, false);
    host->SetBrowserFocus(7);
    if (host->browser.Source()) return;
    host->Navigate(L"https://www.bing.com");
    host->RestoreDownloadFolder();
}

void OpenVideoPlayer() {
    if (!host) return;
    host->SetPage(false, true);
    host->video_list.Focus(Xaml::FocusState::Programmatic);
    host->RestoreVideoFolder();
}

bool HandleMediaController(float right_x, float right_y, bool confirm, bool back,
                           bool alternate, bool play_pause, bool left, bool right,
                           bool up, bool down) {
    if (!IsMediaPageOpen()) return false;
    if (host->browser_open) {
        host->MoveBrowserPointer(right_x, right_y);
        if (alternate) host->PickDownloadFolder();
        if (back) {
            if (host->browser.CanGoBack()) host->browser.GoBack();
            else host->SetPage(false, false);
        }
        if (host->browser_focus == static_cast<int>(host->browser_controls.size())) {
            if (up) host->SetBrowserFocus(1);
            if (down) host->browser.InvokeScriptAsync(L"eval", {L"window.scrollBy(0,window.innerHeight*0.7)"});
            if (confirm) host->ClickBrowser();
        } else {
            if (left) host->SetBrowserFocus((host->browser_focus +
                static_cast<int>(host->browser_controls.size()) - 1) %
                static_cast<int>(host->browser_controls.size()));
            if (right) host->SetBrowserFocus((host->browser_focus + 1) %
                static_cast<int>(host->browser_controls.size()));
            if (down) host->SetBrowserFocus(static_cast<int>(host->browser_controls.size()));
            if (confirm) host->ActivateBrowserControl();
        }
    } else {
        if (alternate) host->PickVideoFolder();
        if (up) host->MoveVideoSelection(-1);
        if (down) host->MoveVideoSelection(1);
        if (confirm) host->ToggleVideoPlayback();
        if (play_pause) host->ToggleVideoPlayback();
        if (back) host->SetPage(false, false);
    }
    return true;
}

void CloseMediaPage() {
    if (host) host->SetPage(false, false);
}

void AttachSwapChain(void* swap_chain) {
    if (!host) throw std::runtime_error("XAML host is not initialized");
    ComPtr<IDXGISwapChain> retained;
    if (swap_chain) {
        winrt::check_hresult(static_cast<IUnknown*>(swap_chain)->QueryInterface(IID_PPV_ARGS(&retained)));
    }
    const auto set = [panel = host->native_panel, retained]() {
        winrt::check_hresult(panel->SetSwapChain(retained.Get()));
    };
    if (host->dispatcher.HasThreadAccess()) {
        set();
    } else {
        host->dispatcher.RunAsync(CoreDispatcherPriority::High, set).get();
    }
}

} // namespace Jadyx::XamlHost
