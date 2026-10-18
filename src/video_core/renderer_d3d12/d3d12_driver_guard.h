// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <type_traits>

#include <windows.h>

namespace D3D12 {

/// What a driver call that faulted returns instead of its own result.
inline constexpr HRESULT DRIVER_FAULT_HRESULT = static_cast<HRESULT>(0x887A0020L); // DXGI_ERROR_DRIVER_INTERNAL_ERROR

HRESULT CallDriverGuardedImpl(HRESULT (*call)(void*), void* context, DWORD& fault_code);

/// Runs a driver call that can fault inside the driver. The Series shader compiler dereferences
/// null on some pipelines and takes the process with it; a pipeline it cannot build is a pipeline
/// that failed: its draws are skipped. On a fault returns DRIVER_FAULT_HRESULT and the exception
/// code in fault_code. Only for calls that build an object (pipelines): a fault in a call that
/// changes shared state would leave that state behind.
template <typename Func>
HRESULT CallDriverGuarded(Func&& func, DWORD& fault_code) {
    using F = std::remove_reference_t<Func>;
    return CallDriverGuardedImpl(
        [](void* context) -> HRESULT { return (*static_cast<F*>(context))(); },
        static_cast<void*>(&func), fault_code);
}

} // namespace D3D12
