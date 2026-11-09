// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

struct ID3D12Device;

namespace D3D12 {

class ShaderCompiler;

/// Diagnostic (on with "glitch_hunt_menu=1"): the renderer runs RunWaveSelfTest once when it
/// starts. Set before boot.
void SetWaveSelfTest(bool enabled);
/// Whether the self-test is still due; clears it.
[[nodiscard]] bool TakeWaveSelfTest();

/// Runs d3d12_wave_selftest.comp on its own queue and logs, mode by mode, which lanes got a wrong
/// result: the 32-lane warp emulation's ballots, masks and shuffles, and warp-aggregated atomics,
/// with each shuffle read at a per-lane index and through indices the whole wave agrees on.
/// Waits for the GPU; never throws.
void RunWaveSelfTest(ID3D12Device* device, const ShaderCompiler& compiler) noexcept;

} // namespace D3D12
