// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the boot frontend calls in the renderer and the texture cache without including their
// headers.

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/common_types.h"

namespace VideoCore {
class RendererBase;
}

namespace D3D12 {
// renderer_d3d12.h; its includes need Mesa's headers, which only video_core sees.
void SetTracedFrame(u32 frame, u32 count);
void TraceNextFrames(u32 count);
void TraceNextMaterialFrame(u64 pixel_shader = 0);
void HuntNextFrames(u32 frames = 8);     // diagnostics/d3d12_draw_hunt.h
void HuntOnCompute(u64 hash);            // diagnostics/d3d12_draw_hunt.h
void SetComputeInputsShader(u64 hash);   // diagnostics/d3d12_compute_inputs.h
void SetMaterialProbe(s32 x, s32 y);
void SetMaterialCaptureFrame(u32 frame);
void SetMaterialSnapshotShaders(u64 first, u64 second);
void SetMaterialSnapshotVertexCounts(u32 first, u32 second);
void SetDumpedShader(u64 unique_hash);
void SetMaterialGpuProbeEnabled(bool enabled);
void SetSkippedShader(u64 unique_hash);
void SetGeometryDroppedShader(u64 unique_hash);
void SetFrameDiagnostics(bool enabled);
void SetPsoCostProbe(bool enabled);      // d3d12_graphics_pipeline.h
std::string DescribePsoCost();            // likewise
void ShowLoadProgress(VideoCore::RendererBase& renderer, size_t done, size_t total);
void ShowCpuLoadProgress(VideoCore::RendererBase& renderer, size_t done, size_t total);
void RemoveDeviceForProbe(VideoCore::RendererBase& renderer);
void SetBcArrayDecode(bool enabled); // d3d12_texture_cache.h
void SetAstcGpuDecode(bool enabled); // d3d12_texture_cache.h
void SetBcGpuDecode(bool enabled); // d3d12_texture_cache.h
void SetNotZeroedHeaps(bool enabled); // d3d12_resource_utils.h
void SetAstcDropTopLevel(bool enabled); // d3d12_texture_cache.h
void SetGpuUnswizzle(bool enabled); // d3d12_texture_cache.h
void SetTextureMemoryCap(u64 bytes); // d3d12_texture_cache.h
void SetTextureMemoryLine(u64 bytes); // d3d12_texture_cache.h
void SetTextureYield(bool yield); // d3d12_texture_cache.h
void GetTextureMemoryUse(u64& heap_bytes, u64& cap); // d3d12_texture_cache.h
void SetHeapCompaction(u64 bytes_per_frame); // d3d12_texture_cache.h
void SetSmallTextureAlignment(bool enabled); // d3d12_resource_allocator.h
void SetSpareHeaps(bool enabled); // d3d12_resource_allocator.h
void SetPsoBlobs(bool enabled); // d3d12_pso_blob_cache.h
void SetCachedStaging(bool enabled); // d3d12_staging_buffer_pool.h
void SetCpuTextureWrites(bool enabled); // d3d12_resource_allocator.h
void SetAstcGpuVerify(bool enabled); // d3d12_texture_cache.h
void SetStencilBlitVerify(bool enabled); // d3d12_texture_cache.h
enum class DxilSignMode; // d3d12_shader_compiler.h
void SetDxilSignMode(DxilSignMode mode);
void SetDxilDiskCacheEnabled(bool enabled); // d3d12_dxil_disk_cache.h
void SetAstcGpuSync(bool enabled); // d3d12_texture_cache.h
void SetForceMinMaxFilter(bool enabled); // d3d12_texture_cache.h
void SetSkipMinMaxFilter(bool enabled); // d3d12_texture_cache.h
void SetEmulateMinMaxFilter(bool enabled); // d3d12_texture_cache.h
void SetAstcGpuFresh(bool enabled); // d3d12_texture_cache.h
void SetGpuBasedValidation(bool enabled); // d3d12_device.h
void SetDredEnabled(bool enabled);         // d3d12_device.h
void SetGpuBreadcrumbsEnabled(bool enabled); // d3d12_device.h
void SetCloseProbeEnabled(bool enabled);     // d3d12_device.h
void SetPresentQueue(bool enabled);          // d3d12_device.h
enum class BorderColorMode : u8; // d3d12_render_corrections.h: Exact, Steps8, BlackWhite
void SetBorderColorMode(BorderColorMode mode);
void SetDepthBiasRounding(bool enabled);     // d3d12_render_corrections.h
void SetUnormHalfDownRounding(bool enabled); // d3d12_render_corrections.h
void SetCubeDirectionSanitize(bool enabled); // d3d12_render_corrections.h
void SetHalfFloatFlush(bool enabled);        // d3d12_render_corrections.h
void SetShaderAlphaToCoverage(bool enabled); // d3d12_render_corrections.h
void SetSmallBcGuestPalette(bool enabled);   // d3d12_render_corrections.h
void SetShaderDepthBias(bool enabled);       // d3d12_render_corrections.h
void SetMemoryImportProbe(bool enabled);     // d3d12_memory_import_probe.h
void SetPipelineEviction(bool enabled);      // d3d12_pipeline_cache.h
void SetPipelinePrewarm(bool enabled);       // d3d12_pipeline_cache.h
void SetComputeWorkgroupSplit(u32 factor);   // d3d12_pipeline_cache.h
void SetTransformFeedbackStorage(bool enabled); // d3d12_pipeline_cache.h
void SetDescriptorRemovalChecks(bool enabled); // d3d12_device.h
void SetUniformUploadCopy(bool enabled);
void SetUniformReuse(bool enabled);
void SetStageViewCache(bool enabled); // d3d12_graphics_pipeline.h
enum class TextureReplayMode : u8; // d3d12_graphics_pipeline.h: Off, On, Check
void SetTextureReplay(TextureReplayMode mode);
void SetSubmitDraws(u32 draws); // d3d12_rasterizer.h
void SetDrawSamplerReuse(bool enabled);
enum class LocalMemoryScratchMode : u8; // d3d12_local_memory_scratch.h: Auto, Uav, Native
void SetPixelShaderScratchMode(LocalMemoryScratchMode mode);
void SetVertexShaderScratchMode(LocalMemoryScratchMode mode);
void SetLocalMemoryPrivateWords(u32 words); // d3d12_local_memory_scratch.h
void SetAsyncRecording(bool enabled);          // d3d12_scheduler.h
void SetDescriptorDeferral(bool enabled);      // d3d12_descriptor_heap.h
void SetDiagUavSync(bool enabled);             // d3d12_scheduler.h
using AppMemoryQuery = bool (*)(u64& used, u64& limit); // d3d12_device.h
void SetAppMemoryQuery(AppMemoryQuery query);            // d3d12_device.h
bool LastGpuMemory(u64& used, u64& budget);              // d3d12_device.h
namespace MemoryReserve {
void Configure(std::uint64_t bytes); // d3d12_memory_reserve.h
}
} // namespace D3D12

namespace AudioCore::Sink {
void SetXAudio2ProfileEnabled(bool enabled) noexcept; // audio_core/sink/xaudio2_sink.h
}

namespace VideoCommon {
void SetAstcArrayRecompression(bool enabled) noexcept; // texture_cache/util.h
void SetOpaqueAstcToBc1(bool enabled) noexcept;        // texture_cache/util.h
void SetMipRenderTargets(bool enabled) noexcept;       // texture_cache/texture_cache_base.h
void SetRenderTargetInheritance(bool enabled) noexcept; // texture_cache/texture_cache_base.h
void SetReclaimSupersededImages(bool enabled) noexcept; // texture_cache/texture_cache_base.h
void SetRenderTargetEarlyOut(bool enabled) noexcept;   // texture_cache/texture_cache_base.h
} // namespace VideoCommon
