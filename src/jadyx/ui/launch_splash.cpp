// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/ui/launch_splash.h"
#include "jadyx/ui/xaml_host.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <thread>

#include <d2d1.h>
#include <d3d12.h>
#include <dwrite.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>

#include "common/logging.h"
#include "core/core.h"
#include "core/perf_stats.h"
#include "jadyx/i18n/localization.h"
#include "jadyx/ui/ui_fonts.h"
#include "video_core/gpu.h"

namespace Jadyx {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

constexpr auto FramePeriod = std::chrono::milliseconds{33};
/// The splash leaves once the game has presented this many frames, or after the timeout.
constexpr unsigned long long GameFramesToClose = 3;
constexpr float StartingTimeoutSeconds = 20;

/// The launch card, in its own units, laid out by the screen's height: a dark diagonal gradient
/// over the whole screen, the stacked logo (Assets/LaunchLogo.png) in the middle and the
/// indicator in the lower-right corner, with the step and its bar to its left.
constexpr float CardWidth = 841.89f, CardHeight = 595.28f;
/// The gradient's ends (#363635 to #1d1d1b), as fractions of the screen.
constexpr D2D1_POINT_2F GradientFrom{-13.07f / CardWidth, 22.89f / CardHeight};
constexpr D2D1_POINT_2F GradientTo{831.8f / CardWidth, 562.46f / CardHeight};
constexpr float LogoHeight = 348.25f;
/// The indicator: its height and its center's distance from the right and bottom edges.
constexpr float IndicatorHeight = 62.75f, IndicatorInset = 40.25f;
/// The step's line right of the indicator: type size, gap to the indicator, the bar under it.
constexpr float StepSize = 11, StepGap = 14, BarWidth = 130, BarHeight = 2;
/// The corner that changes between frames (indicator, step, bar), from the bottom-right corner;
/// the rest of the image is drawn once.
constexpr float ChangingWidth = 330, ChangingHeight = 90;

/// The indicator's drawing (246x280 units), centered on its hexagons' center.
constexpr float HexX = 123.08f, HexY = 140.05f;
/// Pointy-top hexagons by their circumradius: the dark frame, the light ring around the track
/// (outer and inner edge) and the snake's track (outer and inner edge).
constexpr float FrameRadius = 142.08f, RingOuter = 120.69f, RingInner = 89.94f;
constexpr float TrackOuter = 63.5f, TrackInner = 30.8f;
/// The snake: one lap of the track (6 sides) every LapSeconds, its length breathing between
/// about 1.75 and 3.75 sides. Each side keeps its own shade, so the body seems to turn in light.
constexpr float LapSeconds = 1.8f, BreathSeconds = 2.7f;
constexpr std::array<unsigned, 6> SideShades{0xf4f4f4, 0x9b9b9b, 0x757575, 0x808080, 0xbcbcbc, 0xe2e2e2};

std::atomic<Clock::rep> opened{};
std::atomic<LaunchStep> step{LaunchStep::Loading};
std::atomic<std::size_t> progress_done{}, progress_total{};
std::wstring logo_path;

std::wstring StepText() {
    using T = I18n::Text;
    const auto count = [](T id) {
        return I18n::Format(id, {std::to_wstring(progress_done.load(std::memory_order_relaxed)),
                                 std::to_wstring(progress_total.load(std::memory_order_relaxed))});
    };
    const bool counted = progress_total.load(std::memory_order_relaxed) != 0;
    switch (step.load(std::memory_order_relaxed)) {
    case LaunchStep::Shaders:
        if (counted) return count(T::LaunchStepShaders);
        break;
    case LaunchStep::Cpu:
        if (counted) return count(T::LaunchStepCpu);
        break;
    case LaunchStep::Starting:
        return std::wstring{I18n::Tr(T::LaunchStepStarting)};
    case LaunchStep::Loading:
        break;
    }
    return std::wstring{I18n::Tr(T::LaunchStepLoading)};
}

float SecondsOpen() {
    Clock::rep start = opened.load(std::memory_order_relaxed);
    if (start == 0) {
        // Shown by the renderer alone (a boot retried without the library in between).
        Clock::rep now = Clock::now().time_since_epoch().count();
        opened.compare_exchange_strong(start, now);
        start = opened.load(std::memory_order_relaxed);
    }
    return std::chrono::duration<float>(Clock::now() - Clock::time_point{Clock::duration{start}}).count();
}

/// -1 when there is no bar to show.
float Progress() {
    const std::size_t total = progress_total.load(std::memory_order_relaxed);
    if (total == 0) return -1;
    return static_cast<float>(std::min(progress_done.load(std::memory_order_relaxed), total)) /
           static_cast<float>(total);
}

/// Vertex `k` of a pointy-top hexagon, going lower-left, bottom, lower-right, upper-right, top,
/// upper-left: the way the snake crawls.
D2D1_POINT_2F Vertex(float radius, int k) {
    const float radians = (150.0f - 60.0f * static_cast<float>(((k % 6) + 6) % 6)) *
                          std::numbers::pi_v<float> / 180;
    return {HexX + radius * std::cos(radians), HexY + radius * std::sin(radians)};
}

/// The point `u` sides along a hexagon (whole numbers are its vertices).
D2D1_POINT_2F Along(float radius, float u) {
    const float side = std::floor(u);
    const float f = u - side;
    const auto a = Vertex(radius, static_cast<int>(side)), b = Vertex(radius, static_cast<int>(side) + 1);
    return {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
}

/// A hexagon with its corners rounded `cut` units along each side, as the drawing has them.
void RoundedHex(ID2D1GeometrySink* sink, float radius, float cut) {
    const auto toward = [cut, radius](D2D1_POINT_2F from, D2D1_POINT_2F to) {
        const float f = cut / radius; // a regular hexagon's side is its circumradius
        return D2D1::Point2F(from.x + (to.x - from.x) * f, from.y + (to.y - from.y) * f);
    };
    sink->BeginFigure(toward(Vertex(radius, 5), Vertex(radius, 0)), D2D1_FIGURE_BEGIN_FILLED);
    for (int k = 0; k < 6; ++k) {
        const auto corner = Vertex(radius, k);
        sink->AddLine(toward(corner, Vertex(radius, k - 1)));
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(corner, toward(corner, Vertex(radius, k + 1))));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
}

/// Where the card's parts land on a screen.
struct Layout {
    float width, height;
    float units; ///< Pixels per card unit.
    D2D1_POINT_2F indicator;
    D2D1_RECT_F changing; ///< Whole pixels, so a clip to it stays aliased.
};

Layout LayOut(float width, float height) {
    const float units = height / CardHeight;
    return {width, height, units,
            {width - IndicatorInset * units, height - IndicatorInset * units},
            D2D1::RectF(std::max(0.0f, std::floor(width - ChangingWidth * units)),
                        std::max(0.0f, std::floor(height - ChangingHeight * units)), width, height)};
}

/// Draws the splash on a Direct2D target.
class SplashPainter {
public:
    SplashPainter() {
        winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(&wic)));
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                 reinterpret_cast<IUnknown**>(write.GetAddressOf())));
        try {
            ComPtr<IWICBitmapDecoder> decoder;
            winrt::check_hresult(wic->CreateDecoderFromFilename(logo_path.c_str(), nullptr, GENERIC_READ,
                                                                WICDecodeMetadataCacheOnDemand, &decoder));
            ComPtr<IWICBitmapFrameDecode> decoded;
            winrt::check_hresult(decoder->GetFrame(0, &decoded));
            winrt::check_hresult(wic->CreateBitmapFromSource(decoded.Get(), WICBitmapCacheOnLoad, &logo));
        } catch (const winrt::hresult_error& e) {
            // The indicator alone still says the app is working.
            LOG_WARNING(Frontend, "Launch splash: no logo ({})", winrt::to_string(e.message()));
        }
    }

    /// The card at `at`, `t` seconds in: all of it, or only its changing corner.
    void Draw(ID2D1RenderTarget* target, const Layout& at, float t, bool whole) {
        if (target != bound) Bind(target);
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        if (!whole) target->PushAxisAlignedClip(at.changing, D2D1_ANTIALIAS_MODE_ALIASED);
        background->SetStartPoint({GradientFrom.x * at.width, GradientFrom.y * at.height});
        background->SetEndPoint({GradientTo.x * at.width, GradientTo.y * at.height});
        target->FillRectangle(D2D1::RectF(0, 0, at.width, at.height), background.Get());
        if (whole) DrawLogo(target, at);
        DrawStep(target, at);
        const float units = at.units * IndicatorHeight / (2 * FrameRadius);
        target->SetTransform(D2D1::Matrix3x2F::Translation(-HexX, -HexY) *
                             D2D1::Matrix3x2F::Scale(units, units) *
                             D2D1::Matrix3x2F::Translation(at.indicator.x, at.indicator.y));
        target->FillGeometry(frame.Get(), dark.Get());
        target->FillGeometry(ring.Get(), light.Get());
        brush->SetColor(D2D1::ColorF(0xffffff, 0.05f));
        target->FillGeometry(track.Get(), brush.Get());
        DrawSnake(target, t);
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        if (!whole) target->PopAxisAlignedClip();
    }

private:
    void Bind(ID2D1RenderTarget* target) {
        bound = target;
        logo_bitmap.Reset();
        logo_pixels = 0;
        target->GetFactory(&factory);
        winrt::check_hresult(target->CreateSolidColorBrush(D2D1::ColorF(0xffffff), &brush));
        background = Gradient(target, 0x363635, 0x1d1d1b, {0, 0}, {0, 1});
        dark = Gradient(target, 0x363635, 0x1d1d1b, {HexX, -25.61f}, {HexX, 283.88f});
        light = Gradient(target, 0xffffff, 0xbdbdbd, {HexX, 21.37f}, {HexX, 255.52f});
        frame = Hexagons(FrameRadius, 8, 0, 0);
        ring = Hexagons(RingOuter, 7, RingInner, 7);
        track = Hexagons(TrackOuter, 0, TrackInner, 0);
    }

    /// The logo, centered and scaled on the CPU once per size: a software target only filters
    /// linearly, which shimmers at this reduction.
    void DrawLogo(ID2D1RenderTarget* target, const Layout& at) {
        if (!logo) return;
        UINT logo_width = 0, logo_height = 0;
        winrt::check_hresult(logo->GetSize(&logo_width, &logo_height));
        const UINT high = std::max(1U, static_cast<UINT>(std::lround(LogoHeight * at.units)));
        const UINT wide = std::max(
            1U, static_cast<UINT>(std::lround(static_cast<float>(logo_width) * static_cast<float>(high) /
                                              static_cast<float>(logo_height))));
        if (!logo_bitmap || logo_pixels != high) {
            ComPtr<IWICBitmapScaler> scaler;
            winrt::check_hresult(wic->CreateBitmapScaler(&scaler));
            winrt::check_hresult(scaler->Initialize(logo.Get(), wide, high, WICBitmapInterpolationModeFant));
            ComPtr<IWICFormatConverter> converter;
            winrt::check_hresult(wic->CreateFormatConverter(&converter));
            winrt::check_hresult(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                                       WICBitmapDitherTypeNone, nullptr, 0,
                                                       WICBitmapPaletteTypeMedianCut));
            logo_bitmap.Reset();
            winrt::check_hresult(target->CreateBitmapFromWicBitmap(converter.Get(), &logo_bitmap));
            logo_pixels = high;
        }
        const float left = std::round((at.width - static_cast<float>(wide)) / 2);
        const float top = std::round((at.height - static_cast<float>(high)) / 2);
        target->DrawBitmap(logo_bitmap.Get(),
                           D2D1::RectF(left, top, left + static_cast<float>(wide), top + static_cast<float>(high)),
                           1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }

    /// What the boot is doing, right-aligned to the left of the indicator, with its bar under it.
    void DrawStep(ID2D1RenderTarget* target, const Layout& at) {
        const float size = StepSize * at.units;
        if (!step_format || step_size != size) {
            const InterfaceTypeface typeface = InterfaceFonts(write.Get());
            step_format.Reset();
            winrt::check_hresult(write->CreateTextFormat(typeface.family, typeface.collection,
                                                         DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
                                                         DWRITE_FONT_STRETCH_NORMAL, size, L"", &step_format));
            step_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            step_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            step_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            step_size = size;
        }
        const float progress = Progress();
        const float half_width = IndicatorHeight * std::numbers::sqrt3_v<float> / 4;
        const float right = at.indicator.x - (half_width + StepGap) * at.units;
        const float middle = at.indicator.y - (progress >= 0 ? 4 * at.units : 0);
        const std::wstring text = StepText();
        brush->SetColor(D2D1::ColorF(0xbcbcbc));
        target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), step_format.Get(),
                         D2D1::RectF(at.changing.left, middle - size, right, middle + size), brush.Get());
        if (progress < 0) return;
        const float width = BarWidth * at.units, h = BarHeight * at.units, r = h / 2;
        const float x = right - width, y = middle + size * 0.9f;
        brush->SetColor(D2D1::ColorF(0x3a3a39));
        target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, right, y + h), r, r), brush.Get());
        if (const float filled = width * std::min(progress, 1.0f); filled > 0.5f) {
            brush->SetColor(D2D1::ColorF(0xe2e2e2));
            target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + filled, y + h), r, r),
                                         brush.Get());
        }
    }

    ComPtr<ID2D1LinearGradientBrush> Gradient(ID2D1RenderTarget* target, unsigned top, unsigned bottom,
                                              D2D1_POINT_2F from, D2D1_POINT_2F to) {
        const std::array stops{D2D1::GradientStop(0, D2D1::ColorF(top)),
                               D2D1::GradientStop(1, D2D1::ColorF(bottom))};
        ComPtr<ID2D1GradientStopCollection> collection;
        winrt::check_hresult(target->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()),
                                                                  &collection));
        ComPtr<ID2D1LinearGradientBrush> gradient;
        winrt::check_hresult(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to),
                                                               collection.Get(), &gradient));
        return gradient;
    }

    /// A hexagon, or the band between two (an inner radius of 0 is none).
    ComPtr<ID2D1PathGeometry> Hexagons(float outer, float outer_cut, float inner, float inner_cut) {
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        winrt::check_hresult(factory->CreatePathGeometry(&path));
        winrt::check_hresult(path->Open(&sink));
        sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
        RoundedHex(sink.Get(), outer, outer_cut);
        if (inner > 0) RoundedHex(sink.Get(), inner, inner_cut);
        winrt::check_hresult(sink->Close());
        return path;
    }

    /// The snake: the stretch of the track between its tail and its head, side by side.
    void DrawSnake(ID2D1RenderTarget* target, float t) {
        constexpr float tau = 2 * std::numbers::pi_v<float>;
        const float length = 2.75f + std::sin(t * tau / BreathSeconds);
        const float head = std::fmod(t * 6 / LapSeconds, 6.0f) + 6; // kept positive for the tail
        float from = head - length;
        while (from < head) {
            const float to = std::min(std::floor(from) + 1, head);
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            winrt::check_hresult(factory->CreatePathGeometry(&path));
            winrt::check_hresult(path->Open(&sink));
            sink->BeginFigure(Along(TrackOuter, from), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(Along(TrackOuter, to));
            sink->AddLine(Along(TrackInner, to));
            sink->AddLine(Along(TrackInner, from));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            winrt::check_hresult(sink->Close());
            brush->SetColor(D2D1::ColorF(SideShades[static_cast<std::size_t>(std::floor(from)) % 6]));
            target->FillGeometry(path.Get(), brush.Get());
            from = to;
        }
    }

    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteFactory> write;
    ComPtr<IWICBitmap> logo;
    ComPtr<IDWriteTextFormat> step_format;
    float step_size{};
    // Made for the target they were bound to.
    ID2D1RenderTarget* bound{};
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1Bitmap> logo_bitmap;
    UINT logo_pixels{}; ///< The height logo_bitmap was scaled to.
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1LinearGradientBrush> background, dark, light;
    ComPtr<ID2D1PathGeometry> frame, ring, track;
};

/// The splash on a CPU bitmap, as the renderer's menu image.
class BitmapSplash {
public:
    BitmapSplash() {
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2d)));
        winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(&wic)));
    }

    /// The whole screen's image. The first one at a size is drawn whole; later ones redraw only
    /// the changing corner over a kept copy.
    D3D12::MenuImage Draw(std::pair<u32, u32> screen) {
        const auto [screen_width, screen_height] = screen;
        if (screen_width == 0 || screen_height == 0) return {};
        const bool whole = !bitmap || screen_width != image.width || screen_height != image.height;
        if (whole) {
            target.Reset();
            bitmap.Reset();
            winrt::check_hresult(wic->CreateBitmap(screen_width, screen_height, GUID_WICPixelFormat32bppPBGRA,
                                                   WICBitmapCacheOnDemand, &bitmap));
            const auto properties = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::check_hresult(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target));
            image = {screen_width, screen_height, {}, true, 0};
            image.pixels.resize(static_cast<std::size_t>(screen_width) * screen_height * 4);
        }
        const Layout at = LayOut(static_cast<float>(screen_width), static_cast<float>(screen_height));
        target->BeginDraw();
        painter.Draw(target.Get(), at, SecondsOpen(), whole);
        (void)target->EndDraw();
        if (whole) {
            Copy({0, 0, static_cast<INT>(screen_width), static_cast<INT>(screen_height)});
        } else {
            const auto left = static_cast<INT>(at.changing.left), top = static_cast<INT>(at.changing.top);
            Copy({left, top, static_cast<INT>(screen_width) - left, static_cast<INT>(screen_height) - top});
        }
        return image;
    }

private:
    /// A rectangle of the bitmap into the image, as RGBA (the swapchain's order; D2D draws BGRA).
    void Copy(const WICRect& rect) {
        ComPtr<IWICBitmapLock> lock;
        winrt::check_hresult(bitmap->Lock(&rect, WICBitmapLockRead, &lock));
        UINT stride = 0, size = 0;
        BYTE* data = nullptr;
        winrt::check_hresult(lock->GetStride(&stride));
        winrt::check_hresult(lock->GetDataPointer(&size, &data));
        for (INT y = 0; y < rect.Height; ++y) {
            const BYTE* in = data + static_cast<std::size_t>(y) * stride;
            u8* out = image.pixels.data() +
                      (static_cast<std::size_t>(rect.Y + y) * image.width + static_cast<std::size_t>(rect.X)) * 4;
            for (INT x = 0; x < rect.Width; ++x, in += 4, out += 4) {
                out[0] = in[2];
                out[1] = in[1];
                out[2] = in[0];
                out[3] = 0xFF;
            }
        }
    }

    ComPtr<ID2D1Factory> d2d;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    D3D12::MenuImage image; ///< The last frame, kept for the parts that do not change.
    SplashPainter painter;
};

class CompositionBinding {
public:
    void Attach(IDXGISwapChain* swapchain) {
        XamlHost::AttachSwapChain(swapchain);
        attached = true;
    }

    void Detach() {
        if (!attached) return;
        XamlHost::AttachSwapChain(nullptr);
        attached = false;
    }

    ~CompositionBinding() {
        if (!attached) return;
        try {
            XamlHost::AttachSwapChain(nullptr);
        } catch (const winrt::hresult_error& error) {
            LOG_WARNING(Frontend, "Launch splash could not detach its swapchain: 0x{:08X}",
                        static_cast<u32>(error.code().value));
        } catch (const std::exception& error) {
            LOG_WARNING(Frontend, "Launch splash could not detach its swapchain: {}", error.what());
        }
    }

private:
    bool attached{};
};

/// The frontend's splash before the renderer exists: a small D3D12 swapchain on the window that
/// the CPU-drawn image is copied into, on a thread of its own.
class WindowSplash {
public:
    WindowSplash(unsigned width, unsigned height)
        : thread{[this, width, height](std::stop_token stop) { Run(stop, width, height); }} {}

    ~WindowSplash() {
        thread.request_stop();
        thread.join();
    }

private:
    void Run(std::stop_token stop, unsigned width, unsigned height) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Uninitialize {
            ~Uninitialize() { winrt::uninit_apartment(); }
        } uninitialize;
        try {
            Present(stop, std::max(1U, width), std::max(1U, height));
        } catch (const winrt::hresult_error& e) {
            LOG_WARNING(Frontend, "Launch splash stopped: {}", winrt::to_string(e.message()));
        } catch (const std::exception& e) {
            LOG_WARNING(Frontend, "Launch splash stopped: {}", e.what());
        }
    }

    static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                             D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        return barrier;
    }

    void Present(std::stop_token stop, unsigned width, unsigned height) {
        ComPtr<IDXGIFactory4> factory;
        winrt::check_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
        ComPtr<ID3D12Device> device;
        winrt::check_hresult(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        winrt::check_hresult(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // the image's RGBA order
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        ComPtr<IDXGISwapChain1> swapchain1;
        winrt::check_hresult(factory->CreateSwapChainForComposition(queue.Get(), &desc, nullptr,
                                                                     &swapchain1));
        ComPtr<IDXGISwapChain3> swapchain;
        winrt::check_hresult(swapchain1.As(&swapchain));
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = desc.BufferCount;
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        winrt::check_hresult(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)));
        const UINT rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const auto rtv_of = [&](UINT index) {
            auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += static_cast<SIZE_T>(index) * rtv_step;
            return rtv;
        };
        std::array<ComPtr<ID3D12Resource>, 2> buffers;
        for (UINT i = 0; i < buffers.size(); ++i) {
            winrt::check_hresult(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i])));
            device->CreateRenderTargetView(buffers[i].Get(), nullptr, rtv_of(i));
        }

        // An upload buffer as large as the screen's copy footprint holds any image.
        const D3D12_RESOURCE_DESC screen = buffers[0]->GetDesc();
        UINT64 upload_size = 0;
        device->GetCopyableFootprints(&screen, 0, 1, 0, nullptr, nullptr, nullptr, &upload_size);
        D3D12_HEAP_PROPERTIES upload_heap{};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC upload_desc{};
        upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_desc.Width = upload_size;
        upload_desc.Height = 1;
        upload_desc.DepthOrArraySize = 1;
        upload_desc.MipLevels = 1;
        upload_desc.SampleDesc.Count = 1;
        upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        winrt::check_hresult(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&upload)));
        u8* mapped = nullptr;
        winrt::check_hresult(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));

        ComPtr<ID3D12CommandAllocator> allocator;
        winrt::check_hresult(
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;
        winrt::check_hresult(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                                       nullptr, IID_PPV_ARGS(&list)));
        winrt::check_hresult(list->Close());
        ComPtr<ID3D12Fence> fence;
        winrt::check_hresult(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        const winrt::handle done{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        UINT64 fence_value = 0;
        // One frame in flight: the upload buffer and the list are free again once it has run.
        const auto wait_idle = [&] {
            winrt::check_hresult(queue->Signal(fence.Get(), ++fence_value));
            if (fence->GetCompletedValue() < fence_value) {
                winrt::check_hresult(fence->SetEventOnCompletion(fence_value, done.get()));
                WaitForSingleObject(done.get(), INFINITE);
            }
        };

        BitmapSplash splash;
        std::mutex mutex;
        std::condition_variable_any wake;
        CompositionBinding binding;
        binding.Attach(swapchain.Get());
        auto next = Clock::now();
        while (!stop.stop_requested()) {
            const D3D12::MenuImage image = splash.Draw({width, height});
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            if (!image.pixels.empty()) {
                D3D12_RESOURCE_DESC image_desc = screen;
                image_desc.Width = image.width;
                image_desc.Height = image.height;
                device->GetCopyableFootprints(&image_desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
                for (u32 y = 0; y < image.height; ++y) {
                    std::memcpy(mapped + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch,
                                image.pixels.data() + static_cast<std::size_t>(y) * image.width * 4,
                                static_cast<std::size_t>(image.width) * 4);
                }
            }
            const UINT index = swapchain->GetCurrentBackBufferIndex();
            ID3D12Resource* const buffer = buffers[index].Get();
            winrt::check_hresult(allocator->Reset());
            winrt::check_hresult(list->Reset(allocator.Get(), nullptr));
            auto barrier = Transition(buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &barrier);
            constexpr float black[4] = {0, 0, 0, 1};
            list->ClearRenderTargetView(rtv_of(index), black, 0, nullptr);
            barrier = Transition(buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &barrier);
            if (!image.pixels.empty()) {
                D3D12_TEXTURE_COPY_LOCATION destination{};
                destination.pResource = buffer;
                destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION source{};
                source.pResource = upload.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                source.PlacedFootprint = footprint;
                list->CopyTextureRegion(&destination, (width - image.width) / 2, (height - image.height) / 2, 0,
                                        &source, nullptr);
            }
            barrier = Transition(buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            list->ResourceBarrier(1, &barrier);
            winrt::check_hresult(list->Close());
            ID3D12CommandList* const lists[] = {list.Get()};
            queue->ExecuteCommandLists(1, lists);
            const HRESULT presented = swapchain->Present(1, 0);
            wait_idle();
            if (FAILED(presented)) break;
            next += FramePeriod;
            if (const auto now = Clock::now(); next < now) next = now;
            std::unique_lock lock{mutex};
            wake.wait_until(lock, stop, next, [] { return false; });
        }
        // The renderer binds its own swapchain to the window next: idle, drop the swapchain before
        // anything else, and flush the queue again so DXGI lets the window go now, not later.
        wait_idle();
        upload->Unmap(0, nullptr);
        buffers = {};
        binding.Detach();
        swapchain.Reset();
        swapchain1.Reset();
        wait_idle();
    }

    std::jthread thread;
};

std::mutex window_mutex;
std::unique_ptr<WindowSplash> window_splash;

/// Each drawing thread keeps its own painter (and COM, for WIC) until it ends.
D3D12::MenuImage DrawOnThisThread(std::pair<u32, u32> screen) {
    struct Com {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~Com() {
            if (SUCCEEDED(result)) CoUninitialize();
        }
    };
    thread_local Com com;
    thread_local std::optional<BitmapSplash> splash;
    if (!splash) splash.emplace();
    return splash->Draw(screen);
}
} // namespace

void ShowLaunchSplash(void* composition_panel, unsigned width, unsigned height) {
    ReleaseLaunchSplash();
    if (logo_path.empty()) {
        try {
            logo_path = std::wstring{
                winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str()} +
                        L"\\Assets\\LaunchLogo.png";
        } catch (const winrt::hresult_error&) { /* The splash goes without its logo. */ }
    }
    opened.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    SetLaunchStep(LaunchStep::Loading);
    if (composition_panel == nullptr) return;
    std::scoped_lock lock{window_mutex};
    window_splash = std::make_unique<WindowSplash>(width, height);
}

void ReleaseLaunchSplash() {
    std::unique_ptr<WindowSplash> released;
    {
        std::scoped_lock lock{window_mutex};
        released = std::move(window_splash);
    }
}

D3D12::MenuImage DrawLaunchSplash(std::pair<u32, u32> screen) {
    return DrawOnThisThread(screen);
}

void SetLaunchStep(LaunchStep next, std::size_t done, std::size_t total) {
    progress_done.store(done, std::memory_order_relaxed);
    progress_total.store(total, std::memory_order_relaxed);
    step.store(next, std::memory_order_relaxed);
}

struct LaunchScreen::Impl {
    explicit Impl(Core::System& system_)
        : system{system_}, start_frames{system.GetPerfStats().TotalGameFrames()},
          thread{[this](std::stop_token stop) { Run(stop); }} {}

    ~Impl() {
        thread.request_stop();
        if (thread.joinable()) thread.join();
        if (shown_any && !hidden) {
            system.GPU().RunOnGpuThread([this] { D3D12::HideGameMenu(system.Renderer()); });
        }
    }

    void Run(std::stop_token stop) {
        try {
            const auto started = Clock::now();
            std::mutex mutex;
            std::condition_variable_any wake;
            auto next = started;
            while (!stop.stop_requested()) {
                if (system.GetPerfStats().TotalGameFrames() >= start_frames + GameFramesToClose ||
                    std::chrono::duration<float>(Clock::now() - started).count() > StartingTimeoutSeconds) {
                    system.GPU().RunOnGpuThread([this] { D3D12::HideGameMenu(system.Renderer()); });
                    hidden = true;
                    return;
                }
                auto image = DrawOnThisThread(D3D12::GameMenuScreenSize(system.Renderer()));
                if (!image.pixels.empty()) {
                    system.GPU().RunOnGpuThread([&] { D3D12::ShowGameMenu(system.Renderer(), std::move(image)); });
                    shown_any = true;
                }
                next += FramePeriod;
                if (const auto now = Clock::now(); next < now) next = now;
                std::unique_lock lock{mutex};
                wake.wait_until(lock, stop, next, [] { return false; });
            }
        } catch (const winrt::hresult_error& e) {
            LOG_WARNING(Frontend, "Launch screen stopped: {}", winrt::to_string(e.message()));
        } catch (const std::exception& e) {
            LOG_WARNING(Frontend, "Launch screen stopped: {}", e.what());
        }
    }

    Core::System& system;
    const unsigned long long start_frames;
    // Written by the drawing thread only, read after it is joined.
    bool shown_any{}, hidden{};
    std::jthread thread;
};

LaunchScreen::LaunchScreen(Core::System& system) : impl{std::make_unique<Impl>(system)} {}

LaunchScreen::~LaunchScreen() = default;

} // namespace Jadyx
