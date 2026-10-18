// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <fmt/format.h>

#include "video_core/renderer_d3d12/d3d12_command_recorder.h"
#include "video_core/renderer_d3d12/d3d12_command_validation.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

using ListRecorder = CommandRecorder<ID3D12GraphicsCommandList*>;

/// The calls the renderer makes on its direct command list, with the list's own names and
/// parameters, recorded through a ListRecorder: on the recorder's worker they reach the list later,
/// so everything a call points to (barriers, viewports, copy locations...) is copied when it is
/// recorded. Descriptor handles are copied as handles: OMSetRenderTargets and the clears read the
/// RTV/DSV behind them when the worker records them (see CpuDescriptorAllocator).
///
/// A call missing here is added with the same signature, or recorded as is through Record.
class CommandStream {
public:
    using List = ID3D12GraphicsCommandList;

    explicit CommandStream(ListRecorder& recorder_) : recorder{recorder_} {}

    /// Any other use of the list, as func(list), in recording order.
    template <typename Func>
    void Record(Func&& func) {
        recorder.Record(std::forward<Func>(func));
    }

    // --- Barriers and pipeline state -------------------------------------------------------

    void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
        if (GpuDiagnosticsEnabled()) {
            for (UINT i = 0; i < count; ++i) {
                const auto& b = barriers[i];
                if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                    TraceGpuDetail(fmt::format("barrier resource={} sub={} before={:x} after={:x} flags={}",
                        static_cast<void*>(b.Transition.pResource), b.Transition.Subresource,
                        static_cast<u32>(b.Transition.StateBefore), static_cast<u32>(b.Transition.StateAfter),
                        static_cast<u32>(b.Flags)));
                } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
                    TraceGpuDetail(fmt::format("UAV barrier resource={}", static_cast<void*>(b.UAV.pResource)));
                } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                    TraceGpuDetail(fmt::format("alias barrier before={} after={}",
                        static_cast<void*>(b.Aliasing.pResourceBefore), static_cast<void*>(b.Aliasing.pResourceAfter)));
                }
            }
        }
        // An invalid barrier would fail the whole list at Close: drop it, keep the rest.
        for (UINT i = 0; i < count; ++i) {
            if (!ValidBarrier(barriers[i])) {
                std::vector<D3D12_RESOURCE_BARRIER> valid;
                valid.reserve(count);
                for (UINT j = 0; j < count; ++j) {
                    if (j < i || (j > i && ValidBarrier(barriers[j]))) {
                        valid.push_back(barriers[j]);
                    }
                }
                if (!valid.empty()) {
                    ResourceBarrier(static_cast<UINT>(valid.size()), valid.data());
                }
                return;
            }
        }
        Array("ResourceBarrier", barriers, count,
              [count](List* list, const D3D12_RESOURCE_BARRIER* copy) {
                  list->ResourceBarrier(count, copy);
              },
              [count](const D3D12_RESOURCE_BARRIER* copy) {
                  std::string text;
                  for (UINT i = 0; i < count; ++i) {
                      text += DescribeBarrier(copy[i]);
                  }
                  return text;
              });
    }
    void DiscardResource(ID3D12Resource* resource) {
        Probed("DiscardResource",
               [resource](List* list) { list->DiscardResource(resource, nullptr); },
               [resource] { return DescribeResource(resource); });
    }
    void SetPipelineState(ID3D12PipelineState* pipeline) {
        if (GpuDiagnosticsEnabled()) TraceGpuDetail(fmt::format("bind PSO={}", static_cast<void*>(pipeline)));
        Probed("SetPipelineState", [pipeline](List* list) { list->SetPipelineState(pipeline); },
               [pipeline] { return fmt::format("PSO={}", static_cast<void*>(pipeline)); });
        Track([pipeline] { ProbeTrackPipeline(pipeline); });
    }
    void SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap* const* heaps) {
        Array("SetDescriptorHeaps", heaps, count,
              [count](List* list, ID3D12DescriptorHeap* const* copy) {
                  list->SetDescriptorHeaps(count, copy);
              },
              [count](ID3D12DescriptorHeap* const* copy) {
                  std::string text = fmt::format("count={}", count);
                  for (UINT i = 0; i < count; ++i) {
                      text += fmt::format(" heap={}", static_cast<void*>(copy[i]));
                  }
                  return text;
              });
        if (CloseProbeEnabled()) {
            recorder.Record(std::span<ID3D12DescriptorHeap* const>{heaps, count},
                            [count](List*, ID3D12DescriptorHeap* const* copy) {
                                ProbeTrackHeaps(count, copy);
                            });
        }
    }
    void SetPredication(ID3D12Resource* buffer, UINT64 offset, D3D12_PREDICATION_OP op) {
        if (!ValidPredication(buffer, offset)) {
            return;
        }
        Probed("SetPredication", [=](List* list) { list->SetPredication(buffer, offset, op); },
               [=] {
                   return fmt::format("offset={} op={} {}", offset, static_cast<u32>(op),
                                      DescribeResource(buffer));
               });
    }
    void SetMarker(UINT metadata, const void* data, UINT size) {
        Array("SetMarker", static_cast<const std::byte*>(data), size,
              [metadata, size](List* list, const std::byte* copy) {
                  list->SetMarker(metadata, copy, size);
              },
              [metadata, size](const std::byte*) {
                  return fmt::format("metadata={} size={}", metadata, size);
              });
    }

    // --- Root arguments ----------------------------------------------------------------------

    void SetGraphicsRootSignature(ID3D12RootSignature* signature) {
        Probed("SetGraphicsRootSignature",
               [signature](List* list) { list->SetGraphicsRootSignature(signature); },
               [signature] { return fmt::format("signature={}", static_cast<void*>(signature)); });
        Track([signature] { ProbeTrackRootSignature(false, signature); });
    }
    void SetComputeRootSignature(ID3D12RootSignature* signature) {
        Probed("SetComputeRootSignature",
               [signature](List* list) { list->SetComputeRootSignature(signature); },
               [signature] { return fmt::format("signature={}", static_cast<void*>(signature)); });
        Track([signature] { ProbeTrackRootSignature(true, signature); });
    }
    void SetGraphicsRootDescriptorTable(UINT param, D3D12_GPU_DESCRIPTOR_HANDLE table) {
        Probed("SetGraphicsRootDescriptorTable",
               [=](List* list) { list->SetGraphicsRootDescriptorTable(param, table); },
               [=] { return fmt::format("param={} table={:x}", param, table.ptr); });
    }
    void SetComputeRootDescriptorTable(UINT param, D3D12_GPU_DESCRIPTOR_HANDLE table) {
        Probed("SetComputeRootDescriptorTable",
               [=](List* list) { list->SetComputeRootDescriptorTable(param, table); },
               [=] { return fmt::format("param={} table={:x}", param, table.ptr); });
    }
    void SetGraphicsRoot32BitConstants(UINT param, UINT count, const void* data, UINT offset) {
        Array("SetGraphicsRoot32BitConstants", static_cast<const u32*>(data), count,
              [=](List* list, const u32* copy) {
                  list->SetGraphicsRoot32BitConstants(param, count, copy, offset);
              },
              [=](const u32*) {
                  return fmt::format("param={} count={} offset={}", param, count, offset);
              });
    }
    void SetComputeRoot32BitConstants(UINT param, UINT count, const void* data, UINT offset) {
        Array("SetComputeRoot32BitConstants", static_cast<const u32*>(data), count,
              [=](List* list, const u32* copy) {
                  list->SetComputeRoot32BitConstants(param, count, copy, offset);
              },
              [=](const u32*) {
                  return fmt::format("param={} count={} offset={}", param, count, offset);
              });
    }
    void SetGraphicsRootConstantBufferView(UINT param, D3D12_GPU_VIRTUAL_ADDRESS address) {
        Probed("SetGraphicsRootConstantBufferView", [=](List* list) { list->SetGraphicsRootConstantBufferView(param, address); },
               [=] { return fmt::format("param={} VA={:x}", param, address); });
    }
    void SetComputeRootConstantBufferView(UINT param, D3D12_GPU_VIRTUAL_ADDRESS address) {
        Probed("SetComputeRootConstantBufferView", [=](List* list) { list->SetComputeRootConstantBufferView(param, address); },
               [=] { return fmt::format("param={} VA={:x}", param, address); });
    }
    void SetComputeRootShaderResourceView(UINT param, D3D12_GPU_VIRTUAL_ADDRESS address) {
        Probed("SetComputeRootShaderResourceView", [=](List* list) { list->SetComputeRootShaderResourceView(param, address); },
               [=] { return fmt::format("param={} VA={:x}", param, address); });
    }
    void SetComputeRootUnorderedAccessView(UINT param, D3D12_GPU_VIRTUAL_ADDRESS address) {
        Probed("SetComputeRootUnorderedAccessView", [=](List* list) { list->SetComputeRootUnorderedAccessView(param, address); },
               [=] { return fmt::format("param={} VA={:x}", param, address); });
    }

    // --- Input assembler, rasterizer and output merger -------------------------------------

    void IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) {
        Probed("IASetPrimitiveTopology",
               [topology](List* list) { list->IASetPrimitiveTopology(topology); },
               [topology] { return fmt::format("topology={}", static_cast<u32>(topology)); });
    }
    void IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) {
        if (GpuDiagnosticsEnabled()) TraceGpuDetail(fmt::format("index buffer VA={:x} bytes={} format={}",
            view ? view->BufferLocation : 0, view ? view->SizeInBytes : 0,
            view ? static_cast<u32>(view->Format) : 0));
        const std::optional<D3D12_INDEX_BUFFER_VIEW> copy =
            view ? std::optional{*view} : std::nullopt;
        Probed("IASetIndexBuffer",
               [copy](List* list) { list->IASetIndexBuffer(copy ? &*copy : nullptr); },
               [copy] {
                   return copy ? fmt::format("VA={:x} bytes={} format={}", copy->BufferLocation,
                                             copy->SizeInBytes, static_cast<u32>(copy->Format))
                               : std::string{"null"};
               });
    }
    void IASetVertexBuffers(UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views) {
        if (GpuDiagnosticsEnabled() && views) {
            for (UINT i = 0; i < count; ++i) {
                TraceGpuDetail(fmt::format("vertex buffer slot={} VA={:x} bytes={} stride={}",
                    start + i, views[i].BufferLocation, views[i].SizeInBytes, views[i].StrideInBytes));
            }
        }
        if (views == nullptr) {
            Probed("IASetVertexBuffers",
                   [=](List* list) { list->IASetVertexBuffers(start, count, nullptr); },
                   [=] { return fmt::format("start={} count={} null", start, count); });
            return;
        }
        Array("IASetVertexBuffers", views, count,
              [=](List* list, const D3D12_VERTEX_BUFFER_VIEW* copy) {
                  list->IASetVertexBuffers(start, count, copy);
              },
              [=](const D3D12_VERTEX_BUFFER_VIEW* copy) {
                  std::string text = fmt::format("start={} count={}", start, count);
                  for (UINT i = 0; i < count; ++i) {
                      text += fmt::format(" [VA={:x} bytes={} stride={}]", copy[i].BufferLocation,
                                          copy[i].SizeInBytes, copy[i].StrideInBytes);
                  }
                  return text;
              });
    }
    void RSSetViewports(UINT count, const D3D12_VIEWPORT* viewports) {
        Array("RSSetViewports", viewports, count,
              [count](List* list, const D3D12_VIEWPORT* copy) { list->RSSetViewports(count, copy); },
              [count](const D3D12_VIEWPORT* copy) {
                  std::string text = fmt::format("count={}", count);
                  for (UINT i = 0; i < count; ++i) {
                      text += fmt::format(" [{},{} {}x{} depth {}..{}]", copy[i].TopLeftX,
                                          copy[i].TopLeftY, copy[i].Width, copy[i].Height,
                                          copy[i].MinDepth, copy[i].MaxDepth);
                  }
                  return text;
              });
    }
    void RSSetScissorRects(UINT count, const D3D12_RECT* rects) {
        Array("RSSetScissorRects", rects, count,
              [count](List* list, const D3D12_RECT* copy) { list->RSSetScissorRects(count, copy); },
              [count](const D3D12_RECT* copy) {
                  std::string text = fmt::format("count={}", count);
                  for (UINT i = 0; i < count; ++i) {
                      text += DescribeRect(copy[i]);
                  }
                  return text;
              });
    }
    void OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets,
                            BOOL single_range, const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
        if (!ValidRenderTargets(count, targets)) {
            return;
        }
        const std::optional<D3D12_CPU_DESCRIPTOR_HANDLE> depth_copy =
            depth ? std::optional{*depth} : std::nullopt;
        // A single range is one handle to the first of count consecutive descriptors.
        const UINT handles = targets == nullptr ? 0 : (single_range ? 1 : count);
        Array("OMSetRenderTargets", targets, handles,
              [=](List* list, const D3D12_CPU_DESCRIPTOR_HANDLE* copy) {
                  list->OMSetRenderTargets(count, handles == 0 ? nullptr : copy, single_range,
                                           depth_copy ? &*depth_copy : nullptr);
              },
              [=](const D3D12_CPU_DESCRIPTOR_HANDLE* copy) {
                  std::string text = fmt::format("count={} single_range={} DSV={:x}", count,
                                                 single_range, depth_copy ? depth_copy->ptr : 0);
                  for (UINT i = 0; i < handles; ++i) {
                      text += fmt::format(" RTV={:x}", copy[i].ptr);
                  }
                  return text;
              });
    }
    void OMSetBlendFactor(const FLOAT factor[4]) {
        const std::optional<std::array<FLOAT, 4>> copy =
            factor ? std::optional{std::array{factor[0], factor[1], factor[2], factor[3]}}
                   : std::nullopt;
        Probed("OMSetBlendFactor",
               [copy](List* list) { list->OMSetBlendFactor(copy ? copy->data() : nullptr); },
               [copy] {
                   return copy ? fmt::format("{},{},{},{}", (*copy)[0], (*copy)[1], (*copy)[2],
                                             (*copy)[3])
                               : std::string{"null"};
               });
    }
    /// Needs ID3D12GraphicsCommandList1; callers set it only when a pipeline tests the bounds
    /// and the values change, so the interface query stays off the per-draw path.
    void OMSetDepthBounds(FLOAT min, FLOAT max) {
        Probed("OMSetDepthBounds",
               [min, max](List* list) {
                   ComPtr<ID3D12GraphicsCommandList1> list1;
                   if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list1)))) {
                       list1->OMSetDepthBounds(min, max);
                   }
               },
               [min, max] { return fmt::format("min={} max={}", min, max); });
    }
    void OMSetStencilRef(UINT reference) {
        Probed("OMSetStencilRef", [reference](List* list) { list->OMSetStencilRef(reference); },
               [reference] { return fmt::format("reference={}", reference); });
    }

    // --- Draws, dispatches and clears --------------------------------------------------------

    void DrawInstanced(UINT vertices, UINT instances, UINT first_vertex, UINT first_instance) {
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("DrawInstanced vertices={} instances={} first={} instance={}",
            vertices, instances, first_vertex, first_instance)) : 0;
        Breadcrumb(token, false);
        Probed("DrawInstanced",
               [=](List* list) {
                   list->DrawInstanced(vertices, instances, first_vertex, first_instance);
               },
               [=] {
                   return fmt::format("vertices={} instances={} first={} instance={}", vertices,
                                      instances, first_vertex, first_instance);
               });
        Breadcrumb(token, true);
    }
    void DrawIndexedInstanced(UINT indices, UINT instances, UINT first_index, INT base_vertex,
                              UINT first_instance) {
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("DrawIndexed indices={} instances={} first={} base={} instance={}",
            indices, instances, first_index, base_vertex, first_instance)) : 0;
        Breadcrumb(token, false);
        Probed("DrawIndexedInstanced",
               [=](List* list) {
                   list->DrawIndexedInstanced(indices, instances, first_index, base_vertex,
                                              first_instance);
               },
               [=] {
                   return fmt::format("indices={} instances={} first={} base={} instance={}",
                                      indices, instances, first_index, base_vertex,
                                      first_instance);
               });
        Breadcrumb(token, true);
    }
    void Dispatch(UINT x, UINT y, UINT z) {
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("Dispatch groups={}x{}x{}", x, y, z)) : 0;
        Breadcrumb(token, false);
        Probed("Dispatch", [=](List* list) { list->Dispatch(x, y, z); },
               [=] { return fmt::format("groups={}x{}x{}", x, y, z); });
        Breadcrumb(token, true);
    }
    void ExecuteIndirect(ID3D12CommandSignature* signature, UINT max_commands,
                         ID3D12Resource* arguments, UINT64 arguments_offset,
                         ID3D12Resource* count, UINT64 count_offset) {
        if (!ValidExecuteIndirect(signature, arguments, arguments_offset, count, count_offset)) {
            return;
        }
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("ExecuteIndirect signature={} max={} args={} offset={} count={} offset={}",
            static_cast<void*>(signature), max_commands, static_cast<void*>(arguments), arguments_offset,
            static_cast<void*>(count), count_offset)) : 0;
        Breadcrumb(token, false);
        Probed("ExecuteIndirect",
               [=](List* list) {
                   list->ExecuteIndirect(signature, max_commands, arguments, arguments_offset,
                                         count, count_offset);
               },
               [=] {
                   return fmt::format("signature={} max={} offset={} count={} offset={} args {}",
                                      static_cast<void*>(signature), max_commands,
                                      arguments_offset, static_cast<void*>(count), count_offset,
                                      DescribeResource(arguments));
               });
        Breadcrumb(token, true);
    }
    void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE target, const FLOAT color[4],
                               UINT rect_count, const D3D12_RECT* rects) {
        if (!ValidClearRects("ClearRenderTargetView", target, rect_count, rects)) {
            return;
        }
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("clear RTV={:x} rgba={},{},{},{} rects={}",
            target.ptr, color[0], color[1], color[2], color[3], rect_count)) : 0;
        Breadcrumb(token, false);
        const std::array<FLOAT, 4> value{color[0], color[1], color[2], color[3]};
        Array("ClearRenderTargetView", rects, rects ? rect_count : 0,
              [=](List* list, const D3D12_RECT* copy) {
                  list->ClearRenderTargetView(target, value.data(), rect_count,
                                              rect_count == 0 ? nullptr : copy);
              },
              [=](const D3D12_RECT* copy) {
                  std::string text = fmt::format("RTV={:x} rects={}", target.ptr, rect_count);
                  for (UINT i = 0; rects && i < rect_count; ++i) {
                      text += DescribeRect(copy[i]);
                  }
                  return text;
              });
        Breadcrumb(token, true);
    }
    void ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE target, D3D12_CLEAR_FLAGS flags,
                               FLOAT depth, UINT8 stencil, UINT rect_count,
                               const D3D12_RECT* rects) {
        if (!ValidClearRects("ClearDepthStencilView", target, rect_count, rects)) {
            return;
        }
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("clear DSV={:x} flags={} depth={} stencil={} rects={}",
            target.ptr, static_cast<u32>(flags), depth, stencil, rect_count)) : 0;
        Breadcrumb(token, false);
        Array("ClearDepthStencilView", rects, rects ? rect_count : 0,
              [=](List* list, const D3D12_RECT* copy) {
                  list->ClearDepthStencilView(target, flags, depth, stencil, rect_count,
                                              rect_count == 0 ? nullptr : copy);
              },
              [=](const D3D12_RECT* copy) {
                  std::string text = fmt::format("DSV={:x} flags={} rects={}", target.ptr,
                                                 static_cast<u32>(flags), rect_count);
                  for (UINT i = 0; rects && i < rect_count; ++i) {
                      text += DescribeRect(copy[i]);
                  }
                  return text;
              });
        Breadcrumb(token, true);
    }

    // --- Copies and queries ------------------------------------------------------------------

    void CopyBufferRegion(ID3D12Resource* dst, UINT64 dst_offset, ID3D12Resource* src,
                          UINT64 src_offset, UINT64 bytes) {
        if (!ValidCopyBufferRegion(dst, dst_offset, src, src_offset, bytes)) {
            return;
        }
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("CopyBuffer dst={} offset={} src={} offset={} bytes={}",
            static_cast<void*>(dst), dst_offset, static_cast<void*>(src), src_offset, bytes)) : 0;
        Breadcrumb(token, false);
        Probed("CopyBufferRegion",
               [=](List* list) { list->CopyBufferRegion(dst, dst_offset, src, src_offset, bytes); },
               [=] {
                   return fmt::format("dst offset={} src offset={} bytes={} dst {} src {}",
                                      dst_offset, src_offset, bytes, DescribeResource(dst),
                                      DescribeResource(src));
               });
        Breadcrumb(token, true);
    }
    void CopyResource(ID3D12Resource* dst, ID3D12Resource* src) {
        if (!ValidCopyResource(dst, src)) {
            return;
        }
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("CopyResource dst={} src={}", static_cast<void*>(dst), static_cast<void*>(src))) : 0;
        Breadcrumb(token, false);
        Probed("CopyResource", [=](List* list) { list->CopyResource(dst, src); },
               [=] {
                   return fmt::format("dst {} src {}", DescribeResource(dst),
                                      DescribeResource(src));
               });
        Breadcrumb(token, true);
    }
    void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
                           const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
        if (!ValidCopyTextureRegion(*dst, x, y, z, *src, box)) {
            return;
        }
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("CopyTexture dst={} type={} sub_or_format={} xyz={},{},{} src={} type={} sub_or_format={} box={}",
            static_cast<void*>(dst->pResource), static_cast<u32>(dst->Type),
            dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? dst->SubresourceIndex : static_cast<u32>(dst->PlacedFootprint.Footprint.Format),
            x, y, z, static_cast<void*>(src->pResource), static_cast<u32>(src->Type),
            src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? src->SubresourceIndex : static_cast<u32>(src->PlacedFootprint.Footprint.Format), box != nullptr)) : 0;
        Breadcrumb(token, false);
        const D3D12_TEXTURE_COPY_LOCATION dst_copy = *dst;
        const D3D12_TEXTURE_COPY_LOCATION src_copy = *src;
        const std::optional<D3D12_BOX> box_copy = box ? std::optional{*box} : std::nullopt;
        Probed("CopyTextureRegion",
               [=](List* list) {
                   list->CopyTextureRegion(&dst_copy, x, y, z, &src_copy,
                                           box_copy ? &*box_copy : nullptr);
               },
               [=] {
                   std::string text = fmt::format("to {} at {},{},{} from {}",
                                                  DescribeLocation(dst_copy), x, y, z,
                                                  DescribeLocation(src_copy));
                   if (box_copy) {
                       text += fmt::format(" box={},{},{}..{},{},{}", box_copy->left,
                                           box_copy->top, box_copy->front, box_copy->right,
                                           box_copy->bottom, box_copy->back);
                   }
                   return text;
               });
        Breadcrumb(token, true);
    }
    void BeginQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
        if (!ValidQuery("BeginQuery", heap)) {
            return;
        }
        Probed("BeginQuery", [=](List* list) { list->BeginQuery(heap, type, index); },
               [=] {
                   return fmt::format("heap={} type={} index={}", static_cast<void*>(heap),
                                      static_cast<u32>(type), index);
               });
        Track([=] { ProbeTrackQuery(true, heap, type, index); });
    }
    void EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
        if (!ValidQuery("EndQuery", heap)) {
            return;
        }
        Probed("EndQuery", [=](List* list) { list->EndQuery(heap, type, index); },
               [=] {
                   return fmt::format("heap={} type={} index={}", static_cast<void*>(heap),
                                      static_cast<u32>(type), index);
               });
        Track([=] { ProbeTrackQuery(false, heap, type, index); });
    }
    void ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT first, UINT count,
                          ID3D12Resource* dst, UINT64 dst_offset) {
        if (!ValidResolveQueryData(heap, type, count, dst, dst_offset)) {
            return;
        }
        Probed("ResolveQueryData",
               [=](List* list) { list->ResolveQueryData(heap, type, first, count, dst, dst_offset); },
               [=] {
                   return fmt::format("heap={} type={} first={} count={} offset={} dst {}",
                                      static_cast<void*>(heap), static_cast<u32>(type), first,
                                      count, dst_offset, DescribeResource(dst));
               });
    }

private:
    /// With gpu_breadcrumbs=1, the GPU writes a command's detail token as it starts it (finished
    /// false) and once it and all work before it are done (finished true). See DumpBreadcrumbs.
    void Breadcrumb(u64 token, bool finished) {
        if (token == 0) {
            return;
        }
        const D3D12_GPU_VIRTUAL_ADDRESS address = GpuBreadcrumbAddress();
        if (address == 0) {
            return;
        }
        recorder.Record([=](List* list) {
            WriteGpuBreadcrumb(list, address, static_cast<u32>(token), finished);
        });
    }

    /// Records func(list). With close_probe=1 the worker also runs it on the scratch lists,
    /// which name it as call with describe() when they fail Close (see ProbeAppend).
    template <typename Func, typename Describe>
    void Probed(const char* call, Func&& func, Describe&& describe) {
        if (!CloseProbeEnabled()) {
            recorder.Record(std::forward<Func>(func));
            return;
        }
        recorder.Record([call, func = std::forward<Func>(func),
                         describe = std::forward<Describe>(describe)](List* list) mutable {
            func(list);
            using Stored = decltype(func);
            const auto run = [](void* ctx, List* scratch) { (*static_cast<Stored*>(ctx))(scratch); };
            ProbeAppend(list, run, &func, call, describe());
        });
    }

    /// With close_probe=1, func() on the worker after the calls recorded before: keeps the state
    /// the probe sets on its scratch list in step with the real list.
    template <typename Func>
    void Track(Func&& func) {
        if (CloseProbeEnabled()) {
            recorder.Record([func = std::forward<Func>(func)](List*) { func(); });
        }
    }

    /// Probed for a call that points to count Ts: func(list, copy) and describe(copy).
    template <typename T, typename Func, typename Describe>
    void Array(const char* call, const T* data, UINT count, Func&& func, Describe&& describe) {
        const std::span<const T> span{data, count};
        if (!CloseProbeEnabled()) {
            recorder.Record(span, std::forward<Func>(func));
            return;
        }
        recorder.Record(span, [call, func = std::forward<Func>(func),
                               describe = std::forward<Describe>(describe)](
                                  List* list, const T* copy) mutable {
            func(list, copy);
            auto once = [&func, copy](List* scratch) { func(scratch, copy); };
            using Once = decltype(once);
            const auto run = [](void* ctx, List* scratch) { (*static_cast<Once*>(ctx))(scratch); };
            ProbeAppend(list, run, &once, call, describe(copy));
        });
    }

    static std::string DescribeResource(ID3D12Resource* resource) {
        if (resource == nullptr) {
            return "(null)";
        }
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        return fmt::format("({} dim={} {}x{}x{} mips={} format={} samples={} flags={:x})",
                           static_cast<void*>(resource), static_cast<u32>(desc.Dimension),
                           desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels,
                           static_cast<u32>(desc.Format), desc.SampleDesc.Count,
                           static_cast<u32>(desc.Flags));
    }

    static std::string DescribeLocation(const D3D12_TEXTURE_COPY_LOCATION& location) {
        if (location.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
            return fmt::format("sub={} of {}", location.SubresourceIndex,
                               DescribeResource(location.pResource));
        }
        const auto& f = location.PlacedFootprint;
        return fmt::format("footprint offset={} format={} {}x{}x{} pitch={} of {}", f.Offset,
                           static_cast<u32>(f.Footprint.Format), f.Footprint.Width,
                           f.Footprint.Height, f.Footprint.Depth, f.Footprint.RowPitch,
                           DescribeResource(location.pResource));
    }

    static std::string DescribeRect(const D3D12_RECT& rect) {
        return fmt::format(" [{},{}..{},{}]", rect.left, rect.top, rect.right, rect.bottom);
    }

    static std::string DescribeBarrier(const D3D12_RESOURCE_BARRIER& b) {
        switch (b.Type) {
        case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
            return fmt::format(" [transition sub={} {:x}->{:x} flags={} {}]",
                               b.Transition.Subresource,
                               static_cast<u32>(b.Transition.StateBefore),
                               static_cast<u32>(b.Transition.StateAfter),
                               static_cast<u32>(b.Flags), DescribeResource(b.Transition.pResource));
        case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
            return fmt::format(" [alias {} -> {}]", DescribeResource(b.Aliasing.pResourceBefore),
                               DescribeResource(b.Aliasing.pResourceAfter));
        case D3D12_RESOURCE_BARRIER_TYPE_UAV:
            return fmt::format(" [UAV {}]", DescribeResource(b.UAV.pResource));
        default:
            return fmt::format(" [type={}]", static_cast<u32>(b.Type));
        }
    }

    ListRecorder& recorder;
};

} // namespace D3D12
