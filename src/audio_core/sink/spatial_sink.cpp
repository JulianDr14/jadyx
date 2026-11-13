// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio_core/sink/spatial_sink.h"

#include <algorithm>
#include <span>

#include <mmdeviceapi.h>
#include <mmreg.h>
#include <objbase.h>
#include <wrl/implements.h>
#include <winrt/Windows.Media.Devices.h>

#include "audio_core/common/common.h"
#include "audio_core/sink/null_sink.h"
#include "audio_core/sink/realtime_pacer.h"
#include "audio_core/sink/sink_stream.h"
#include "audio_core/sink/surround_mix.h"
#include "common/logging.h"
#include "common/thread.h"

namespace AudioCore::Sink {
namespace {

using Microsoft::WRL::ComPtr;

/// Speaker object per game channel, in the Switch order (FL FR FC LFE BL BR). The 5.1 surrounds
/// sit at about 110 degrees, which the 7.1 bed calls the sides, as the XAudio2 matrix does.
constexpr std::array<AudioObjectType, Surround::GameChannels> BedObjects{
    AudioObjectType_FrontLeft,    AudioObjectType_FrontRight, AudioObjectType_FrontCenter,
    AudioObjectType_LowFrequency, AudioObjectType_SideLeft,   AudioObjectType_SideRight,
};

constexpr AudioObjectType BedMask() {
    u32 mask{};
    for (const AudioObjectType type : BedObjects) {
        mask |= static_cast<u32>(type);
    }
    return static_cast<AudioObjectType>(mask);
}

/// Fallback period when the spatial stream is gone: the guest still needs a clock.
constexpr u32 FallbackFrames = TargetSampleCount;
constexpr DWORD PeriodTimeoutMs = 100;
constexpr DWORD ActivationTimeoutMs = 2000;

constexpr WAVEFORMATEX ObjectFormat() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 1;
    format.nSamplesPerSec = TargetSampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = sizeof(float);
    format.nAvgBytesPerSec = TargetSampleRate * sizeof(float);
    return format;
}

class ActivationHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, Microsoft::WRL::FtmBase,
          IActivateAudioInterfaceCompletionHandler> {
public:
    ActivationHandler() : done{CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS)} {}
    ~ActivationHandler() override {
        if (done != nullptr) {
            CloseHandle(done);
        }
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done);
        return S_OK;
    }
    bool Wait() const {
        return done != nullptr && WaitForSingleObjectEx(done, ActivationTimeoutMs, FALSE) ==
                                      WAIT_OBJECT_0;
    }

private:
    HANDLE done;
};

/// The spatial client of the default output, or null (no device, no spatial API).
ComPtr<ISpatialAudioClient> ActivateSpatialClient() {
    std::wstring device_id;
    try {
        using namespace winrt::Windows::Media::Devices;
        device_id = MediaDevice::GetDefaultAudioRenderId(AudioDeviceRole::Default).c_str();
    } catch (const winrt::hresult_error& error) {
        LOG_WARNING(Service_Audio, "Spatial sound: no default output (HRESULT {:#010x})",
                    static_cast<u32>(error.code()));
        return nullptr;
    }
    if (device_id.empty()) {
        return nullptr;
    }
    const auto handler = Microsoft::WRL::Make<ActivationHandler>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT result = ActivateAudioInterfaceAsync(device_id.c_str(), __uuidof(ISpatialAudioClient),
                                                 nullptr, handler.Get(), &operation);
    if (FAILED(result) || !handler->Wait()) {
        LOG_WARNING(Service_Audio, "Spatial sound: activation failed (HRESULT {:#010x})",
                    static_cast<u32>(FAILED(result) ? result : HRESULT_FROM_WIN32(WAIT_TIMEOUT)));
        return nullptr;
    }
    HRESULT activate_result{};
    ComPtr<IUnknown> unknown;
    ComPtr<ISpatialAudioClient> client;
    result = operation->GetActivateResult(&activate_result, &unknown);
    if (FAILED(result) || FAILED(activate_result) || FAILED(unknown.As(&client))) {
        LOG_WARNING(Service_Audio, "Spatial sound: no spatial client (HRESULT {:#010x})",
                    static_cast<u32>(FAILED(result) ? result : activate_result));
        return nullptr;
    }
    return client;
}

/// The guest side of a spatial stream: the sink's render thread pulls its samples while it plays.
class SpatialSinkStream final : public SinkStream {
public:
    SpatialSinkStream(Core::System& system_, u32 system_channels_, std::string name_,
                      StreamType type_)
        : SinkStream{system_, type_} {
        name = std::move(name_);
        system_channels = system_channels_;
        device_channels = Surround::GameChannels;
    }
    ~SpatialSinkStream() override {
        Finalize();
    }

    void Finalize() override {
        Stop();
    }
    void Start(bool) override {
        paused = false;
    }
    void Stop() override {
        if (!paused) {
            SignalPause();
        }
    }
};

} // Anonymous namespace

std::unique_ptr<SpatialSink> SpatialSink::TryCreate() {
    // The client lives in the MTA: this thread stays in it for the sink's life.
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_initialized = SUCCEEDED(com_result);
    ComPtr<ISpatialAudioClient> client = ActivateSpatialClient();
    HRESULT available = E_NOINTERFACE;
    if (client != nullptr) {
        available = client->IsSpatialAudioStreamAvailable(
            __uuidof(ISpatialAudioObjectRenderStream), nullptr);
    }
    if (available != S_OK) {
        LOG_INFO(Service_Audio, "Spatial sound off (HRESULT {:#010x}); XAudio2 output",
                 static_cast<u32>(available));
        client.Reset();
        if (com_initialized) {
            CoUninitialize();
        }
        return nullptr;
    }
    return std::unique_ptr<SpatialSink>(new SpatialSink(std::move(client), com_initialized));
}

SpatialSink::SpatialSink(ComPtr<ISpatialAudioClient> client_, const bool com_initialized_)
    : client{std::move(client_)}, com_initialized{com_initialized_},
      com_thread_id{GetCurrentThreadId()} {
    device_channels = Surround::GameChannels;
}

SpatialSink::~SpatialSink() {
    StopRendering();
    CloseStreams();
    client.Reset();
    if (com_initialized && com_thread_id == GetCurrentThreadId()) {
        CoUninitialize();
    }
}

bool SpatialSink::StartRendering() {
    render_attempted = true;
    max_frames = FallbackFrames;
    const WAVEFORMATEX format = ObjectFormat();

    HRESULT result = client->IsAudioObjectFormatSupported(&format);
    UINT32 period_frames{};
    if (SUCCEEDED(result)) {
        result = client->GetMaxFrameCount(&format, &period_frames);
    }
    if (SUCCEEDED(result)) {
        period_event = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
        result = period_event != nullptr ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(result)) {
        SpatialAudioObjectRenderStreamActivationParams params{};
        params.ObjectFormat = &format;
        params.StaticObjectTypeMask = BedMask();
        params.Category = AudioCategory_GameMedia;
        params.EventHandle = period_event;
        PROPVARIANT activation{};
        activation.vt = VT_BLOB;
        activation.blob.cbSize = sizeof(params);
        activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
        result = client->ActivateSpatialAudioStream(&activation, IID_PPV_ARGS(&render_stream));
    }
    for (std::size_t i = 0; SUCCEEDED(result) && i < bed.size(); ++i) {
        result = render_stream->ActivateSpatialAudioObject(BedObjects[i], &bed[i]);
    }
    if (SUCCEEDED(result)) {
        result = render_stream->Start();
    }

    if (SUCCEEDED(result)) {
        max_frames = std::max<u32>(period_frames, 1);
        AudioObjectType native{};
        static_cast<void>(client->GetNativeStaticObjectTypeMask(&native));
        LOG_INFO(Service_Audio,
                 "Spatial sound output ready (games 5.1 as speaker objects, {} frames per period, "
                 "native bed mask {:#x})",
                 max_frames, static_cast<u32>(native));
    } else {
        LOG_ERROR(Audio_Sink,
                  "Spatial sound: render stream failed (HRESULT {:#010x}); using timed silent "
                  "output",
                  static_cast<u32>(result));
        bed = {};
        render_stream.Reset();
    }
    planes.assign(std::size_t{max_frames} * Surround::GameChannels, 0.0F);
    scratch.assign(std::size_t{max_frames} * Surround::GameChannels, 0);
    render_thread = std::jthread{[this](std::stop_token stop_token) { RenderLoop(stop_token); }};
    return render_stream != nullptr;
}

void SpatialSink::RenderLoop(std::stop_token stop_token) {
    Common::SetCurrentThreadName("SpatialOutput");
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool live = render_stream != nullptr;
    RealtimePacer pacer;
    while (!stop_token.stop_requested()) {
        if (live) {
            if (WaitForSingleObjectEx(period_event, PeriodTimeoutMs, FALSE) == WAIT_OBJECT_0 &&
                !RenderPeriod()) {
                live = false; // Spatial sound turned off or device lost: keep the guest's clock.
            }
            continue;
        }
        {
            std::scoped_lock lock{mutex};
            MixStreams(FallbackFrames);
        }
        pacer.Wait(FallbackFrames);
    }
    if (SUCCEEDED(com_result)) {
        CoUninitialize();
    }
}

bool SpatialSink::RenderPeriod() {
    UINT32 dynamic_objects{};
    UINT32 frames{};
    const HRESULT result = render_stream->BeginUpdatingAudioObjects(&dynamic_objects, &frames);
    if (FAILED(result)) {
        LOG_ERROR(Audio_Sink,
                  "Spatial sound: update failed (HRESULT {:#010x}); using timed silent output",
                  static_cast<u32>(result));
        return false;
    }
    frames = std::min(frames, max_frames);
    {
        std::scoped_lock lock{mutex};
        MixStreams(frames);
    }
    for (std::size_t channel = 0; channel < bed.size(); ++channel) {
        BYTE* buffer{};
        UINT32 bytes{};
        if (SUCCEEDED(bed[channel]->GetBuffer(&buffer, &bytes))) {
            Surround::WritePlane(
                std::span<const float>{planes.data() + channel * max_frames, frames},
                std::span<float>{reinterpret_cast<float*>(buffer), bytes / sizeof(float)});
        }
    }
    static_cast<void>(render_stream->EndUpdatingAudioObjects());
    return true;
}

void SpatialSink::MixStreams(const u32 frames) {
    std::fill(planes.begin(), planes.end(), 0.0F);
    const std::span<s16> period{scratch.data(), std::size_t{frames} * Surround::GameChannels};
    for (const auto& stream : sink_streams) {
        if (stream->GetStreamType() == StreamType::In || stream->IsPaused()) {
            continue;
        }
        stream->ProcessAudioOutAndRender(period, frames);
        Surround::AccumulateInterleaved(period, Surround::GameChannels, frames, planes, max_frames);
    }
}

void SpatialSink::StopRendering() {
    if (render_thread.joinable()) {
        render_thread.request_stop();
        render_thread.join();
    }
    if (render_stream != nullptr) {
        static_cast<void>(render_stream->Stop());
    }
    bed = {};
    render_stream.Reset();
    if (period_event != nullptr) {
        CloseHandle(period_event);
        period_event = nullptr;
    }
}

SinkStream* SpatialSink::AcquireSinkStream(Core::System& system, const u32 system_channels_,
                                           const std::string& name, const StreamType type) {
    std::scoped_lock lock{mutex};
    system_channels = system_channels_;
    SinkStreamPtr stream;
    if (type == StreamType::In) {
        stream = std::make_unique<NullSinkStreamImpl>(system, type);
        stream->SetSystemChannels(system_channels_);
    } else {
        if (!render_attempted) {
            static_cast<void>(StartRendering());
        }
        stream = std::make_unique<SpatialSinkStream>(system, system_channels_, name, type);
    }
    stream->SetDeviceVolume(device_volume);
    auto* result = stream.get();
    sink_streams.emplace_back(std::move(stream));
    return result;
}

void SpatialSink::CloseStream(SinkStream* stream) {
    std::scoped_lock lock{mutex};
    const auto it = std::find_if(sink_streams.begin(), sink_streams.end(),
                                 [stream](const auto& item) { return item.get() == stream; });
    if (it != sink_streams.end()) {
        sink_streams.erase(it);
    }
}

void SpatialSink::CloseStreams() {
    std::vector<SinkStreamPtr> streams;
    {
        std::scoped_lock lock{mutex};
        streams.swap(sink_streams);
    }
    streams.clear();
}

f32 SpatialSink::GetDeviceVolume() const {
    return device_volume;
}

void SpatialSink::SetDeviceVolume(const f32 volume) {
    std::scoped_lock lock{mutex};
    device_volume = volume;
    for (auto& stream : sink_streams) {
        stream->SetDeviceVolume(volume);
    }
}

void SpatialSink::SetSystemVolume(const f32 volume) {
    std::scoped_lock lock{mutex};
    for (auto& stream : sink_streams) {
        stream->SetSystemVolume(volume);
    }
}

} // namespace AudioCore::Sink
