// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/compatibility_loader.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include "jadyx/network/http.h"
#include "jadyx/storage/atomic_file.h"

namespace Jadyx::Compatibility {
namespace {
std::string ReadBounded(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary | std::ios::ate};
    if (!input)
        return {};
    const auto size = input.tellg();
    if (size < 0 || size > static_cast<std::streamoff>(MaxBytes))
        return {};
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        return {};
    return bytes;
}
} // namespace
struct Loader::Impl {
    std::mutex mutex;
    std::optional<Table> ready;
    // Destroyed first: cancel and join before the mutex and published data are released.
    std::jthread worker;

    void Publish(Table table) {
        const std::scoped_lock lock{mutex};
        ready = std::move(table);
    }
    void Run(std::stop_token stop) {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Apartment {
                ~Apartment() {
                    winrt::uninit_apartment();
                }
            } apartment;
            const auto bundled =
                std::filesystem::path{winrt::Windows::ApplicationModel::Package::Current()
                                          .InstalledLocation()
                                          .Path()
                                          .c_str()} /
                L"compat" / L"compat.json";
            const auto bundled_bytes = ReadBounded(bundled);
            std::filesystem::path cached;
            try {
                cached = std::filesystem::path{winrt::Windows::Storage::ApplicationData::Current()
                                                   .LocalFolder()
                                                   .Path()
                                                   .c_str()} /
                         L"compat" / L"compat.json";
            } catch (...) { /* Read-only data still works if LocalState is unavailable. */
            }
            Publish(
                Select({}, cached.empty() ? std::string{} : ReadBounded(cached), bundled_bytes));
            const char* refresh = std::getenv("JADYX_COMPAT_REFRESH");
            if (!RefreshEnabled(refresh ? refresh : "",
                                ReadBounded(bundled.parent_path().parent_path() / L"boot.cfg")) ||
                stop.stop_requested())
                return;
            std::stop_source request;
            std::stop_callback cancelled{stop, [&request] { request.request_stop(); }};
            // A total deadline, including headers and body. Fetch polls cancellation every 100 ms.
            std::jthread deadline{[&request](std::stop_token timer_stop) {
                std::mutex timer_mutex;
                std::condition_variable_any timer_cv;
                std::unique_lock timer_lock{timer_mutex};
                timer_cv.wait_for(timer_lock, timer_stop, std::chrono::seconds{3},
                                  [] { return false; });
                request.request_stop();
            }};
            const auto response = Http::Fetch(
                "https://raw.githubusercontent.com/JulianDr14/jadyx/main/dist/compat/compat.json",
                {}, request.get_token(), MaxBytes);
            deadline.request_stop();
            if (stop.stop_requested() || response.status != 200)
                return;
            if (auto table = Parse(response.body)) {
                // Cache only complete, validated documents. Failure leaves the bundled fallback.
                if (!cached.empty())
                    WriteFileAtomically(cached, response.body);
                Publish(std::move(*table));
            }
        } catch (...) { /* Compatibility is optional: offline and storage failures stay silent. */
        }
    }
};
Loader::Loader() : impl{std::make_unique<Impl>()} {}
Loader::~Loader() = default;
void Loader::Start() {
    if (!impl->worker.joinable()) {
        try {
            impl->worker =
                std::jthread{[state = impl.get()](std::stop_token stop) { state->Run(stop); }};
        } catch (...) { /* A missing optional badge list must never prevent opening the library. */
        }
    }
}
std::optional<Table> Loader::Take() {
    const std::scoped_lock lock{impl->mutex};
    return std::exchange(impl->ready, std::nullopt);
}
} // namespace Jadyx::Compatibility
