// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <wrl/client.h>
#include <spatialaudioclient.h>

#include "audio_core/sink/sink.h"
#include "audio_core/sink/surround_layout.h"
#include "common/polyfill_thread.h"

namespace AudioCore::Sink {

/// Output through the Windows spatial sound API, used when the console (or PC) has Dolby Atmos,
/// Windows Sonic or DTS on. Those formats leave the plain endpoint in stereo, so XAudio2 alone
/// would never see surround: here the games get 5.1 and each channel becomes a speaker object
/// that the system renders to Atmos or Sonic. Every stream is mixed by one render thread paced
/// by the system's period event; if the spatial stream fails, it keeps pacing in silence.
class SpatialSink final : public Sink {
public:
    /// nullptr when no spatial format is on: the caller then uses XAudio2.
    static std::unique_ptr<SpatialSink> TryCreate();
    ~SpatialSink() override;

    SinkStream* AcquireSinkStream(Core::System& system, u32 system_channels,
                                  const std::string& name, StreamType type) override;
    void CloseStream(SinkStream* stream) override;
    void CloseStreams() override;
    f32 GetDeviceVolume() const override;
    void SetDeviceVolume(f32 volume) override;
    void SetSystemVolume(f32 volume) override;

private:
    SpatialSink(Microsoft::WRL::ComPtr<ISpatialAudioClient> client_, bool com_initialized_);

    bool StartRendering(); ///< Lazily, with the first output stream; callers hold `mutex`.
    void RenderLoop(std::stop_token stop_token);
    bool RenderPeriod();
    void MixStreams(u32 frames); ///< Callers hold `mutex`.
    void StopRendering();

    mutable std::mutex mutex;
    std::vector<SinkStreamPtr> sink_streams;
    Microsoft::WRL::ComPtr<ISpatialAudioClient> client;
    Microsoft::WRL::ComPtr<ISpatialAudioObjectRenderStream> render_stream;
    std::array<Microsoft::WRL::ComPtr<ISpatialAudioObject>, Surround::GameChannels> bed;
    HANDLE period_event{};
    u32 max_frames{};
    std::vector<float> planes;  ///< GameChannels planes of max_frames; sized once at start.
    std::vector<s16> scratch;   ///< One stream's interleaved 5.1 for a period; sized once.
    bool render_attempted{};
    bool com_initialized{}; ///< By TryCreate's thread; released there, as XAudio2Sink does.
    unsigned long com_thread_id{};
    std::jthread render_thread;
};

} // namespace AudioCore::Sink
