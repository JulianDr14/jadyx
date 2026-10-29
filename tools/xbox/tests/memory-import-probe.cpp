// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <iostream>
#include <stdexcept>
#include <string>

#include <dxgi1_4.h>
#include <wrl/client.h>
#include "video_core/renderer_d3d12/d3d12_memory_import_probe.h"

using Microsoft::WRL::ComPtr;

namespace {

void Check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("FAIL " + what);
}

ComPtr<ID3D12Device> MakeDevice(bool warp) {
    ComPtr<IDXGIAdapter> adapter;
    if (warp) {
        ComPtr<IDXGIFactory4> factory;
        Check(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))), "DXGI factory");
        Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter");
    }
    ComPtr<ID3D12Device> device;
    Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                      IID_PPV_ARGS(&device))),
          "D3D12 device");
    return device;
}

void Probe(bool warp) {
    const ComPtr<ID3D12Device> device = MakeDevice(warp);
    const D3D12::MemoryImportProbe probe = D3D12::ProbeMemoryImport(device.Get());
    const std::string line = D3D12::DescribeMemoryImportProbe(probe);
    Check(SUCCEEDED(device->GetDeviceRemovedReason()), "the probe keeps the device");
    Check(SUCCEEDED(probe.open) || (probe.place == E_NOTIMPL && probe.copy == E_NOTIMPL),
          "nothing placed without a heap: " + line);
    Check(FAILED(probe.copy) || probe.matches, "a GPU copy of process memory matches: " + line);
    D3D12::MemoryImportProbe none;
    Check(D3D12::DescribeMemoryImportProbe(none) ==
              "heap from an address not tried, placed buffer not tried, GPU copy not tried, "
              "data matches not tried",
          "description of an untried probe");
    std::cout << "PASS memory import probe (" << (warp ? "WARP" : "adapter") << "): " << line
              << '\n';
}

} // Anonymous namespace

int main() try {
    Probe(false);
    Probe(true);
    D3D12::SetMemoryImportProbe(true);
    Check(D3D12::GetMemoryImportProbe(), "switch on");
    D3D12::SetMemoryImportProbe(false);
    Check(!D3D12::GetMemoryImportProbe(), "switch off");
    std::cout << "PASS memory import probe\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
