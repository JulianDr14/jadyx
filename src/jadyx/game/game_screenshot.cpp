// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/game/game_screenshot.h"

#include <cstdio>
#include <system_error>
#include <vector>

#include <wincodec.h>
#include <wrl/client.h>

namespace Jadyx {

std::optional<std::wstring> SaveScreenshot(const D3D12::MenuImage& frame, const std::filesystem::path& folder) {
    using Microsoft::WRL::ComPtr;
    const std::size_t pixels = static_cast<std::size_t>(frame.width) * frame.height;
    if (pixels == 0 || frame.pixels.size() < pixels * 4) return std::nullopt;
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) return std::nullopt;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[32]{};
    std::swprintf(stamp, std::size(stamp), L"jadyx_%04u-%02u-%02u_%02u-%02u-%02u", now.wYear, now.wMonth, now.wDay,
                  now.wHour, now.wMinute, now.wSecond);
    std::wstring name = std::wstring{stamp} + L".png";
    for (int n = 2; std::filesystem::exists(folder / name, error); ++n)
        name = std::wstring{stamp} + L"_" + std::to_wstring(n) + L".png";
    const auto path = folder / name;

    // 24-bit BGR, as PNG keeps it without an alpha channel the frame never had.
    std::vector<BYTE> bgr(pixels * 3);
    for (std::size_t i = 0; i < pixels; ++i) {
        bgr[i * 3 + 0] = frame.pixels[i * 4 + 2];
        bgr[i * 3 + 1] = frame.pixels[i * 4 + 1];
        bgr[i * 3 + 2] = frame.pixels[i * 4 + 0];
    }
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> image;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    const bool written =
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&image, nullptr)) && SUCCEEDED(image->Initialize(nullptr)) &&
        SUCCEEDED(image->SetSize(frame.width, frame.height)) && SUCCEEDED(image->SetPixelFormat(&format)) &&
        format == GUID_WICPixelFormat24bppBGR &&
        SUCCEEDED(image->WritePixels(frame.height, frame.width * 3, static_cast<UINT>(bgr.size()), bgr.data())) &&
        SUCCEEDED(image->Commit()) && SUCCEEDED(encoder->Commit());
    if (!written) {
        image.Reset();
        encoder.Reset();
        stream.Reset();
        std::filesystem::remove(path, error);
        return std::nullopt;
    }
    return name;
}

} // namespace Jadyx
