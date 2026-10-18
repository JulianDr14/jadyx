// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <d3d12.h>

namespace D3D12 {

/// Checks the arguments the D3D12 runtime itself validates on the calls below. A call that
/// fails them does not fail on its own: it marks the whole command list, whose Close then returns
/// E_INVALIDARG and loses every command of the frame (on the Series, the process). CommandStream
/// drops such a call instead and logs it, which also names the call the crash would have hidden.
bool ValidCopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION& dst, UINT x, UINT y, UINT z,
                            const D3D12_TEXTURE_COPY_LOCATION& src, const D3D12_BOX* box);
bool ValidCopyBufferRegion(ID3D12Resource* dst, UINT64 dst_offset, ID3D12Resource* src,
                           UINT64 src_offset, UINT64 bytes);
bool ValidBarrier(const D3D12_RESOURCE_BARRIER& barrier);
bool ValidCopyResource(ID3D12Resource* dst, ID3D12Resource* src);
bool ValidClearRects(const char* call, D3D12_CPU_DESCRIPTOR_HANDLE target, UINT rect_count,
                     const D3D12_RECT* rects);
bool ValidRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets);
bool ValidPredication(ID3D12Resource* buffer, UINT64 offset);
bool ValidExecuteIndirect(ID3D12CommandSignature* signature, ID3D12Resource* arguments,
                          UINT64 arguments_offset, ID3D12Resource* count, UINT64 count_offset);
bool ValidQuery(const char* call, ID3D12QueryHeap* heap);
bool ValidResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT count,
                           ID3D12Resource* dst, UINT64 dst_offset);

} // namespace D3D12
