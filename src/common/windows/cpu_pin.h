// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Common::Windows {

/// Where named threads are placed among the logical processors allocated to the app.
enum class CpuPin {
    /// The scheduler places every thread.
    Off,
    /// Each guest core thread runs on both logical processors of a physical core of its own, so
    /// no two guest cores share one; other threads float.
    Cores,
    /// Each guest core thread runs on the first logical processor of a core of its own; the GPU,
    /// recorder and other named threads run on the second ones.
    Split,
    /// Each thread named in the map (SetCpuPinMap) runs where the map says; other threads float.
    Map,
};

/// The map "cpu_pin=topology" stands for: the GPU thread on a physical core of its own with the
/// recorder on its sibling, guest core 1 on another with vsync, present and audio, and guest cores
/// 2 and 0 sharing the third. Core 1 sets the guest's frame and core 2 mostly spins on yields, so
/// core 1 must not share with either: with 0 and 1 together the game fell to 20 fps while walking
/// and turning the camera. Pipeline builds (130-190 ms each in the driver) go to the vsync logical
/// processor at background priority: floating, they took the free siblings of the GPU thread and
/// of the guest cores and the frame went past 33 ms while a new area's shaders built.
inline constexpr std::string_view CpuPinTopology =
    "GPU=0.0,D3D12Recorder=0.1,CPUCore_2=1.0,CPUCore_0=1.1,CPUThread=1.1,"
    "CPUCore_1=2.0,VSyncThread=2.1,D3D12Present=2.1,"
    "DSP_AudioRenderer_Main=2.1,AudioRenderSystemManager=2.1,XAudio2Output=2.1,"
    "D3D12PipelineBuilder=2.1";

/// Sets the map CpuPin::Map uses: comma-separated "name=core" or "name=core.logical" entries.
/// core counts the app's physical cores from 0; logical picks one of its logical processors,
/// both when left out. A name also matches threads whose name starts with it (worker pools).
/// Returns false, leaving the map unchanged, if the text does not parse.
bool SetCpuPinMap(std::string_view map);

/// Read by each thread when it is named (Common::SetCurrentThreadName); set it before the
/// emulator's threads start.
void SetCpuPin(CpuPin pin);

/// Places the calling thread by its name under the current CpuPin. Called by
/// SetCurrentThreadName.
void PinCurrentThread(const char* name);

/// The physical cores and logical processors the plan uses, and the threads placed so far.
[[nodiscard]] std::string CpuPinReport();

/// The CPU set ids of the app's logical processors, one list per physical core, primary first.
[[nodiscard]] std::vector<std::vector<unsigned long>> AppCoreCpuSets();

} // namespace Common::Windows
