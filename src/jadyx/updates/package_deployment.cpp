// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <thread>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>
#include "jadyx/updates/package_deployment.h"

namespace Jadyx::Updates {
DeploymentResult DeployPackage(const std::filesystem::path& path, std::stop_token stop) {
    try {
        using namespace winrt::Windows::Foundation;
        using namespace winrt::Windows::Management::Deployment;
        // Escape each component: '#' and '%' in a user profile must remain filename bytes.
        const auto native = path.generic_wstring();
        if (!path.is_absolute() || native.size() < 3 || native[1] != L':' || native[2] != L'/')
            return {false, 0x80070057u}; // E_INVALIDARG; no UNC/network deployment.
        std::wstring uri = L"file:///" + native.substr(0, 3);
        for (std::size_t start = 3; start < native.size();) {
            const auto end = native.find(L'/', start);
            uri += Uri::EscapeComponent(native.substr(start, end - start));
            if (end == native.npos)
                break;
            uri += L'/';
            start = end + 1;
        }
        if (stop.stop_requested())
            return {false, 0x800704c7u};
        auto operation = PackageManager{}.AddPackageAsync(
            Uri{uri}, nullptr, DeploymentOptions::ForceApplicationShutdown);
        while (operation.Status() == AsyncStatus::Started) {
            if (stop.stop_requested()) {
                // App replacement may itself trigger suspension/teardown. Do not cancel
                // the OS deployment just because the old process is shutting down.
                return {false, 0, true};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        const auto result = operation.GetResults();
        const auto error = static_cast<std::uint32_t>(result.ExtendedErrorCode().value);
        return {error == 0 && result.IsRegistered(), error                   ? error
                                                     : result.IsRegistered() ? 0u
                                                                             : 0x80004005u};
    } catch (const winrt::hresult_error& error) {
        return {false, static_cast<std::uint32_t>(error.code().value)};
    } catch (...) {
        return {false, 0x80004005u};
    }
}
} // namespace Jadyx::Updates
