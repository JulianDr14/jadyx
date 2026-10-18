// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_driver_guard.h"

namespace D3D12 {
namespace {

int FaultFilter(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

} // Anonymous namespace

// No objects with destructors here: __try cannot share a function with C++ unwinding.
HRESULT CallDriverGuardedImpl(HRESULT (*call)(void*), void* context, DWORD& fault_code) {
    DWORD code = 0;
    __try {
        return call(context);
    } __except (FaultFilter(code = GetExceptionCode())) {
        fault_code = code;
        return DRIVER_FAULT_HRESULT;
    }
}

} // namespace D3D12
