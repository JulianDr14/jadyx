// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include <d3d12.h>

namespace D3D12 {

/// Whether the GPU reads process memory in place: a heap opened over a VirtualAlloc block
/// (OpenExistingHeapFromAddress), a buffer placed on it and a GPU copy out of it, compared with
/// what the CPU wrote. Each step runs only if the one before it worked.
struct MemoryImportProbe {
    HRESULT open{E_NOTIMPL};
    HRESULT place{E_NOTIMPL};
    HRESULT copy{E_NOTIMPL};
    bool matches{};
};

/// Runs the probe on `device` with a queue and list of its own; waits for the GPU.
MemoryImportProbe ProbeMemoryImport(ID3D12Device* device);

/// "heap from an address yes, placed buffer yes, GPU copy yes, data matches yes", with the
/// failing call's HRESULT.
std::string DescribeMemoryImportProbe(const MemoryImportProbe& probe);

/// Runs the probe at device creation and logs it ("memory_import_probe=1"). Off by default: an
/// unknown driver path on the console.
void SetMemoryImportProbe(bool enabled);
[[nodiscard]] bool GetMemoryImportProbe();

} // namespace D3D12
