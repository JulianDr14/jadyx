// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <limits>
#include <fstream>
#include "common/scope_exit.h"
#include "common/fs/path_util.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

namespace D3D12 {
namespace FC = VideoCommon::FullCapture;
namespace {
constexpr u64 MAX_STAGING = 16ULL << 20;
template <typename T>
std::span<const u8> Bytes(const T& object) {
    static_assert(std::is_trivially_copyable_v<T>);
    return {reinterpret_cast<const u8*>(&object), sizeof(object)};
}
}

void RasterizerD3D12::RunFullCaptureSelfTest() {
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "trace" /
                      fmt::format("full_selftest_{}", stamp);
    if (!FC::Begin(path)) { LOG_ERROR(Render, "Full capture GPU self-test could not start"); return; }
    bool completed = false;
    SCOPE_EXIT {
        FC::End(completed);
        if (full_capture_staging.buffer) {
            staging.FreeDeferred(full_capture_staging);
            full_capture_staging = {};
        }
    };
    try {
        std::scoped_lock lock{texture_cache.mutex, buffer_cache.mutex};
        full_capture_operation = 0;
        using Format = VideoCore::Surface::PixelFormat;
        struct Case { Format format; bool volume; };
        for (const auto test : {Case{Format::A8B8G8R8_UNORM, false}, Case{Format::R32_UINT, false},
                                Case{Format::D32_FLOAT, false}, Case{Format::A8B8G8R8_UNORM, true}}) {
            ++full_capture_operation;
            VideoCommon::ImageInfo info{};
            info.type = test.volume ? VideoCommon::ImageType::e3D : VideoCommon::ImageType::e2D;
            info.format = test.format;
            info.size = {13, 7, test.volume ? 3U : 1U};
            info.resources = {1, 1};
            info.num_samples = 1;
            Image image{texture_runtime, info, 0, 0};
            const auto desc = image.Handle()->GetDesc();
            ComPtr<ID3D12Device> device;
            ThrowIfFailed(image.Handle()->GetDevice(IID_PPV_ARGS(&device)), "capture test device");
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT rows{};
            UINT64 row_bytes{}, bytes{};
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_bytes, &bytes);
            auto upload = staging.Request(bytes, MemoryUsage::Upload, true);
            SCOPE_EXIT { staging.FreeDeferred(upload); };
            const u32 all_rows = rows * footprint.Footprint.Depth;
            for (u32 row = 0; row < all_rows; ++row)
                for (u64 col = 0; col < row_bytes; ++col)
                    upload.mapped_span[row * footprint.Footprint.RowPitch + col] =
                        static_cast<u8>(row * 37 + col * 11 + full_capture_operation);
            FC::Data("selftest_expected_image", fmt::format("{{\"op\":{}}}", full_capture_operation),
                upload.mapped_span.first(bytes), row_bytes, footprint.Footprint.RowPitch, all_rows);
            image.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
            auto placed = footprint;
            placed.Offset = upload.offset;
            const D3D12_TEXTURE_COPY_LOCATION src{upload.buffer,
                D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = placed}};
            const D3D12_TEXTURE_COPY_LOCATION dst{image.Handle(),
                D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = 0}};
            scheduler.CommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            image.Transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            const auto state = image.State();
            CaptureFullImage(image, 0, "selftest", false);
            CaptureFullImage(image, 0, "selftest", true);
            if (image.State() != state) FC::Omit("selftest_image_state_changed");
            if (test.format == Format::A8B8G8R8_UNORM && !test.volume) {
                if (auto* alias = image.Reinterpreted(DXGI_FORMAT_R32_TYPELESS)) {
                    image.ReadReinterpreted();
                    CaptureFullImage(image, 0, "selftest_reinterpret", false, alias);
                } else FC::Omit("selftest_reinterpret_unavailable");
            }
            if (test.format == Format::D32_FLOAT) {
                if (auto* feedback = image.PrepareDepthFeedback())
                    CaptureFullImage(image, 0, "selftest_feedback", false, feedback->Handle());
                else FC::Omit("selftest_feedback_unavailable");
            }
            if (test.volume) {
                image.RefreshSliceArray();
                auto* slices = image.DiagnosticSliceArray();
                if (!slices) FC::Omit("selftest_slices_unavailable");
                else for (u32 layer = 0; layer < 3; ++layer) {
                    FC::Data("selftest_expected_slice", fmt::format("{{\"op\":{},\"layer\":{}}}",
                        full_capture_operation, layer), upload.mapped_span.subspan(
                            u64{layer} * rows * footprint.Footprint.RowPitch),
                        row_bytes, footprint.Footprint.RowPitch, rows);
                    CaptureFullImage(image, layer, "selftest_slice", false, slices);
                }
            }
        }
        // Reproduce the sanctuary's 83-layer input shape, with small texels. All
        // 581 subresources must survive packing into a single readback allocation.
        ++full_capture_operation;
        VideoCommon::ImageInfo array_info{};
        array_info.type = VideoCommon::ImageType::e2D;
        array_info.format = Format::A8B8G8R8_UNORM;
        array_info.size = {64, 64, 1};
        array_info.resources = {7, 83};
        array_info.num_samples = 1;
        Image array{texture_runtime, array_info, 0, 0};
        const auto array_desc = array.Handle()->GetDesc();
        constexpr u32 count = 7 * 83;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        std::vector<UINT> rows(count);
        std::vector<UINT64> row_bytes(count);
        std::vector<u32> subresources;
        UINT64 bytes{};
        ComPtr<ID3D12Device> array_device;
        ThrowIfFailed(array.Handle()->GetDevice(IID_PPV_ARGS(&array_device)), "capture array device");
        array_device->GetCopyableFootprints(&array_desc, 0, count, 0, footprints.data(),
                                            rows.data(), row_bytes.data(), &bytes);
        auto array_upload = staging.Request(bytes, MemoryUsage::Upload, true);
        SCOPE_EXIT { staging.FreeDeferred(array_upload); };
        array.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
        for (u32 sub = 0; sub < count; ++sub) {
            const auto& fp = footprints[sub];
            for (u32 row = 0; row < rows[sub]; ++row)
                for (u64 col = 0; col < row_bytes[sub]; ++col)
                    array_upload.mapped_span[fp.Offset + row * fp.Footprint.RowPitch + col] =
                        static_cast<u8>(sub * 17 + row * 37 + col * 11);
            FC::Data("selftest_expected_batch", fmt::format("{{\"subresource\":{}}}", sub),
                array_upload.mapped_span.subspan(fp.Offset), row_bytes[sub],
                fp.Footprint.RowPitch, rows[sub]);
            auto placed = fp;
            placed.Offset += array_upload.offset;
            const D3D12_TEXTURE_COPY_LOCATION src{array_upload.buffer,
                D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = placed}};
            const D3D12_TEXTURE_COPY_LOCATION dst{array.Handle(),
                D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}};
            scheduler.CommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            subresources.push_back(sub);
        }
        array.Transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        CaptureFullImageRange(array, subresources, "selftest_batch", false, array.Handle());
        ++full_capture_operation;
        Buffer buffer{buffer_runtime, 0, 4096};
        auto upload = staging.Request(4096, MemoryUsage::Upload, true);
        SCOPE_EXIT { staging.FreeDeferred(upload); };
        for (u32 i = 0; i < 4096; ++i) upload.mapped_span[i] = static_cast<u8>(i * 13 + 5);
        FC::Data("selftest_expected_buffer", "{}", upload.mapped_span.first(4096));
        buffer.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
        scheduler.CommandList()->CopyBufferRegion(buffer.Handle(), 0, upload.buffer, upload.offset, 4096);
        buffer.Transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const TracedBuffer traced{.kind = TracedBuffer::Kind::Storage, .buffer = &buffer,
                                  .size = 4096, .written = true};
        const auto generation = buffer.Generation();
        CaptureFullBuffers({&traced, 1}, false);
        CaptureFullBuffers({&traced, 1}, true);
        if (buffer.Generation() != generation) FC::Omit("selftest_buffer_generation_changed");
        completed = true;
        LOG_INFO(Render, "Full capture GPU self-test exported: {} (verify expected/image/buffer bytes)", path.string());
    } catch (const std::exception& e) {
        FC::Omit("selftest_exception");
        LOG_ERROR(Render, "Full capture GPU self-test: {}", e.what());
    }
}

StagingBufferRef& RasterizerD3D12::FullCaptureStaging() {
    // Exactly one pinned allocation for the whole session, reused only after Finish.
    // FreeDeferred otherwise tags the next recording tick and would need a second buffer.
    if (!full_capture_staging.buffer)
        full_capture_staging = staging.Request(MAX_STAGING, MemoryUsage::Download, true);
    return full_capture_staging;
}

void RasterizerD3D12::CaptureFullPipeline(const GraphicsPipeline& pipeline,
                                        const PipelineBindings& bindings) {
    if (!FC::Active()) return;
    try {
        const auto context = fmt::format("{{\"op\":{},\"pipeline\":{}}}",
                                        full_capture_operation, pipeline.KeyHash());
        FC::Data("graphics_key", context, Bytes(pipeline.Key()));
        FC::Data("graphics_registers", context, Bytes(maxwell3d->regs));
        FC::Data("push_constants", context, Bytes(bindings.push_constants));
        const auto& layout = pipeline.Layout();
        FC::Event("root_layout", fmt::format(
            "{{\"op\":{},\"runtime_words\":{},\"resource_index\":{},\"sampler_index\":{},"
            "\"integer_index\":{},\"resource_count\":{},\"sampler_count\":{}}}",
            full_capture_operation, layout.RuntimeDataWords(), layout.ResourceTableIndex(),
            layout.SamplerTableIndex(), layout.IntegerSamplerIndex(), layout.NumResourceDescriptors(),
            layout.NumSamplerDescriptors()));
        for (size_t stage = 0; stage < pipeline.NUM_STAGES; ++stage) {
            const auto dxil = pipeline.DiagnosticDxil(stage);
            if (!dxil.empty()) FC::Data("dxil", fmt::format(
                "{{\"op\":{},\"pipeline\":{},\"stage\":{}}}",
                full_capture_operation, pipeline.KeyHash(), stage), dxil);
            if (!pipeline.HasStage(stage)) continue;
            const auto& info = pipeline.StageInfo(stage);
            const auto descriptors = [&](std::string_view kind, const auto& list) {
                if (list.empty()) return;
                FC::Data("shader_descriptors", fmt::format(
                    "{{\"op\":{},\"stage\":{},\"kind\":\"{}\",\"count\":{}}}",
                    full_capture_operation, stage, kind, list.size()),
                    {reinterpret_cast<const u8*>(list.data()), list.size() * sizeof(list[0])});
            };
            descriptors("uniform", info.constant_buffer_descriptors);
            descriptors("storage", info.storage_buffers_descriptors);
            descriptors("texel", info.texture_buffer_descriptors);
            descriptors("image_buffer", info.image_buffer_descriptors);
            descriptors("texture", info.texture_descriptors);
            descriptors("image", info.image_descriptors);
        }
        for (const auto& sampler : *bindings.trace_filters) FC::Text("sampler", sampler);
    } catch (...) { FC::Omit("graphics_pipeline_exception"); }
}

void RasterizerD3D12::CaptureFullBuffers(std::span<const TracedBuffer> buffers, bool after) {
    if (!FC::Active()) return;
    u32 ordinal = 0;
    for (const auto& source : buffers) {
        const auto binding = ordinal++;
        if (after && !source.written) continue;
        const auto context = fmt::format(
            "{{\"op\":{},\"after\":{},\"binding\":{},\"kind\":{},\"slot\":{},"
            "\"address\":{},\"offset\":{},\"size\":{},\"stride_or_format\":{},\"written\":{}}}",
            full_capture_operation, after, binding, static_cast<u32>(source.kind), source.slot,
            source.device_addr, source.offset, source.size, source.stride, source.written);
        try {
            FC::Event("buffer_binding", context);
            if (!source.size) continue;
            if (source.kind == TracedBuffer::Kind::NullUniform) continue;
            if (source.mapped) {
                FC::Data("buffer_data", context, {source.mapped, source.size});
                continue;
            }
            if (!source.buffer || !source.buffer->Handle()) {
                FC::Omit("buffer_unavailable", context);
                continue;
            }
            auto& buffer = *source.buffer;
            const auto desc = buffer.Handle()->GetDesc();
            if (source.offset > desc.Width || source.size > desc.Width - source.offset ||
                source.size > (256ULL << 20)) {
                FC::Omit("buffer_range", context);
                continue;
            }
            // Stream arbitrarily sized bindings in bounded chunks; no 256-KiB prefixes.
            for (u64 at = 0; at < source.size;) {
                const u64 count = std::min<u64>(MAX_STAGING, source.size - at);
                auto& readback = FullCaptureStaging();
                if (readback.mapped_span.size() < count) {
                    FC::Omit("short_buffer_staging", context);
                    break;
                }
                const auto previous = buffer.State();
                const auto tick = scheduler.CurrentTick();
                buffer.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
                scheduler.CommandList()->CopyBufferRegion(readback.buffer, readback.offset,
                    buffer.Handle(), source.offset + at, count);
                // Buffers decay on ExecuteCommandLists; restoring before Finish is safe.
                buffer.Transition(previous, false);
                scheduler.Finish();
                FC::Data("buffer_data", fmt::format(
                    "{{\"binding\":{},\"chunk_offset\":{},\"copy_tick\":{},\"fence_waited\":true}}",
                    context, at, tick), readback.mapped_span.first(count));
                at += count;
            }
        } catch (...) { FC::Omit("buffer_capture_exception", context); }
    }
}

void RasterizerD3D12::CaptureFullImage(Image& image, u32 subresource,
                                     std::string_view role, bool after, ID3D12Resource* actual) {
    CaptureFullImageRange(image, {&subresource, 1}, role, after, actual);
}

void RasterizerD3D12::CaptureFullImageRange(Image& image, std::span<const u32> subresources,
                                          std::string_view role, bool after,
                                          ID3D12Resource* actual) {
    if (!FC::Active() || subresources.empty()) return;
    try {
        auto* resource = actual ? actual : image.Handle();
        if (!resource) { FC::Omit("image_unavailable"); return; }
        const auto desc = resource->GetDesc();
        const auto context_for = [&](u32 subresource) {
            return fmt::format(
                "{{\"op\":{},\"after\":{},\"role\":\"{}\",\"gpu_address\":{},"
                "\"cpu_address\":{},\"guest_format\":{},\"dxgi_format\":{},\"dimension\":{},"
                "\"width\":{},\"height\":{},\"depth_or_layers\":{},\"mips\":{},"
                "\"samples\":{},\"flags\":{},\"subresource\":{}}}",
                full_capture_operation, after, role, image.gpu_addr, image.cpu_addr,
                static_cast<u32>(image.info.format), static_cast<u32>(desc.Format),
                static_cast<u32>(desc.Dimension), desc.Width, desc.Height, desc.DepthOrArraySize,
                desc.MipLevels, desc.SampleDesc.Count, static_cast<u32>(desc.Flags), subresource);
        };
        if (desc.SampleDesc.Count != 1) {
            FC::Omit("msaa_raw_copy_unsupported", context_for(subresources.front()));
            return;
        }
        ComPtr<ID3D12Device> device;
        ThrowIfFailed(resource->GetDevice(IID_PPV_ARGS(&device)), "full capture image device");
        struct Copy {
            u32 subresource, rows;
            u64 row_bytes, total;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        };
        std::vector<Copy> batch;
        u64 used = 0;
        auto& readback = FullCaptureStaging();
        const auto flush = [&] {
            if (batch.empty()) return;
            const auto previous = image.DiagnosticState(resource);
            if (!previous) { FC::Omit("image_state_unavailable"); batch.clear(); used = 0; return; }
            const auto tick = scheduler.CurrentTick();
            image.DiagnosticTransition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
            for (const auto& copy : batch) {
                const D3D12_TEXTURE_COPY_LOCATION src{resource,
                    D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = copy.subresource}};
                auto placed = copy.footprint;
                placed.Offset += readback.offset;
                const D3D12_TEXTURE_COPY_LOCATION dst{readback.buffer,
                    D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = placed}};
                scheduler.CommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            image.DiagnosticTransition(resource, *previous);
            scheduler.Finish();
            for (const auto& copy : batch) {
                const auto context = context_for(copy.subresource);
                if (role.starts_with("pick_probe_") && desc.Format == DXGI_FORMAT_R32G32B32A32_TYPELESS) {
                    const std::array<float, 4> sentinel{-99999, -99999, -99999, -99999};
                    if (std::memcmp(readback.mapped_span.data() + copy.footprint.Offset,
                                    sentinel.data(), sizeof(sentinel)) == 0)
                        FC::Omit("pick_probe_no_fragment", context);
                }
                FC::Event("image_binding", context);
                FC::Data("image_data", fmt::format(
                    "{{\"image\":{},\"copy_tick\":{},\"fence_waited\":true,\"row_bytes\":{},"
                    "\"rows_per_slice\":{},\"depth\":{},\"gpu_row_pitch\":{},\"copy_format\":{}}}",
                    context, tick, copy.row_bytes, copy.rows, copy.footprint.Footprint.Depth,
                    copy.footprint.Footprint.RowPitch, static_cast<u32>(copy.footprint.Footprint.Format)),
                    readback.mapped_span.subspan(copy.footprint.Offset, copy.total),
                    copy.row_bytes, copy.footprint.Footprint.RowPitch,
                    u64{copy.rows} * copy.footprint.Footprint.Depth);
            }
            batch.clear();
            used = 0;
        };
        for (const auto subresource : subresources) {
            Copy copy{.subresource = subresource};
            device->GetCopyableFootprints(&desc, subresource, 1, 0, &copy.footprint,
                                          &copy.rows, &copy.row_bytes, &copy.total);
            const u64 all_rows = u64{copy.rows} * copy.footprint.Footprint.Depth;
            if (!copy.rows || !copy.row_bytes || !copy.total || copy.total > MAX_STAGING ||
                !all_rows || copy.row_bytes > copy.footprint.Footprint.RowPitch ||
                (all_rows - 1) * copy.footprint.Footprint.RowPitch + copy.row_bytes > copy.total ||
                copy.total > readback.mapped_span.size()) {
                FC::Omit("image_footprint_or_staging_limit", context_for(subresource));
                continue;
            }
            auto offset = (used + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                          ~(u64{D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT} - 1);
            if (copy.total > MAX_STAGING - offset) { flush(); offset = 0; }
            copy.footprint.Offset = offset;
            used = offset + copy.total;
            batch.push_back(copy);
        }
        flush();
    } catch (...) { FC::Omit("image_capture_exception"); }
}

void RasterizerD3D12::CaptureFullImages(const FullCaptureViews& views,
                                      const Framebuffer* framebuffer, bool after) {
    if (!FC::Active()) return;
    try {
        const auto capture = [&](Image& image, s32 base_mip, s32 mip_count,
                                 s32 base_layer, s32 layer_count, std::string_view role,
                                 ID3D12Resource* actual) -> u32 {
            const auto desc = actual->GetDesc();
            ComPtr<ID3D12Device> device;
            ThrowIfFailed(actual->GetDevice(IID_PPV_ARGS(&device)), "full capture planes");
            D3D12_FEATURE_DATA_FORMAT_INFO info{desc.Format};
            ThrowIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info)),
                          "full capture format planes");
            const u32 layers = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                ? 1 : desc.DepthOrArraySize;
            std::vector<u32> subresources;
            for (s32 layer = base_layer; layer < base_layer + layer_count; ++layer)
                for (s32 mip = base_mip; mip < base_mip + mip_count; ++mip)
                    for (u32 plane = 0; plane < info.PlaneCount; ++plane)
                        subresources.push_back(mip + layer * desc.MipLevels +
                            plane * desc.MipLevels * layers);
            CaptureFullImageRange(image, subresources, role, after, actual);
            return static_cast<u32>(subresources.size());
        };
        u32 binding = 0;
        for (const auto& binding_view : views) {
            const auto id = binding_view.id;
            const bool storage = binding_view.storage;
            const u32 ordinal = binding++;
            if (after && !storage) continue;
            auto& view = texture_cache.GetImageView(id);
            const auto meta = fmt::format(
                "{{\"op\":{},\"binding\":{},\"after\":{},\"storage\":{},\"address\":{},"
                "\"format\":{},\"type\":{},\"mip_base\":{},\"mip_count\":{},"
                "\"layer_base\":{},\"layer_count\":{}}}",
                full_capture_operation, ordinal, after, storage, view.GpuAddr(),
                static_cast<u32>(view.format), static_cast<u32>(view.type), view.range.base.level,
                view.range.extent.levels, view.range.base.layer, view.range.extent.layers);
            FC::Event("image_view", meta);
            auto* image = view.SourceImage();
            const auto actual = view.DiagnosticSrv(binding_view.declared, binding_view.depth_feedback);
            if (!storage && actual.null_srv) {
                FC::Event("null_srv", meta);
                continue;
            }
            if (!image || !image->Handle()) { FC::Omit("image_view_unavailable", meta); continue; }
            FC::Event("native_view", fmt::format(
                "{{\"view\":{},\"declared_type\":{},\"depth_feedback\":{},\"format\":{},\"mapping\":{}}}",
                meta, static_cast<u32>(binding_view.declared), binding_view.depth_feedback,
                actual.format, actual.mapping));
            const s32 base_layer = storage ? view.range.base.layer : actual.base_layer;
            const s32 layer_count = storage ? view.range.extent.layers : actual.layers;
            const s32 base_mip = storage ? view.range.base.level : actual.base_mip;
            const s32 mip_count = storage ? view.range.extent.levels : actual.mips;
            auto* resource = storage ? view.ImageHandle() : actual.resource;
            // Only cache read-only native SRVs. Outputs are always downloaded after writes;
            // aliases/feedback/storage stay conservative because they have separate copy lifetimes.
            // Moving a native image from SRV to any writable state changes write_version.
            constexpr D3D12_RESOURCE_STATES writes = D3D12_RESOURCE_STATE_RENDER_TARGET |
                D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST;
            const bool reusable = !storage && resource == image->Handle() &&
                                  (image->State() & writes) == 0;
            const auto key = reusable ? fmt::format("{}:{}:{}:{}:{}:{}",
                static_cast<const void*>(resource), image->DiagnosticWriteVersion(),
                base_mip, mip_count, base_layer, layer_count) : std::string{};
            if (reusable) {
                if (const auto it = full_capture_images.find(key); it != full_capture_images.end()) {
                    FC::Event("image_view_reference", fmt::format(
                        "{{\"view\":{},\"source_op\":{},\"source_binding\":{},\"subresources\":{}}}",
                        meta, it->second.operation, it->second.binding, it->second.subresources));
                    continue;
                }
            }
            const auto count = capture(*image, base_mip, mip_count, base_layer, layer_count,
                                       fmt::format("view_{}", ordinal), resource);
            if (reusable) full_capture_images.emplace(key,
                FullCaptureImageSnapshot{ComPtr<ID3D12Resource>{resource},
                                         full_capture_operation, ordinal, count});
        }
        if (!framebuffer) return;
        if (!after && FC::TargetsAfterOnly()) {
            FC::Event("targets_before_skipped", fmt::format("{{\"op\":{}}}", full_capture_operation));
            return;
        }
        for (u32 rt = 0; rt < VideoCommon::NUM_RT; ++rt) {
            auto* image = const_cast<Image*>(framebuffer->ColorImage(rt));
            if (!image || !image->Handle()) continue;
            auto* resource = framebuffer->ColorOnCopy(rt) ? image->DiagnosticReinterpreted() : image->Handle();
            if (!resource) { FC::Omit("render_target_copy_unavailable"); continue; }
            const auto base = framebuffer->ColorBase(rt);
            capture(*image, base.level, 1, base.layer, framebuffer->ColorLayers(rt),
                    fmt::format("rt{}", rt), resource);
        }
        if (auto* image = const_cast<Image*>(framebuffer->DepthImage()); image && image->Handle()) {
            const auto base = framebuffer->DepthBase();
            capture(*image, base.level, 1, base.layer, framebuffer->DepthLayers(),
                    "depth_stencil", image->Handle());
        }
    } catch (...) { FC::Omit("image_views_exception"); }
}
void RasterizerD3D12::CapturePickProbes(
    const GraphicsPipeline& pipeline, const PipelineBindings& bindings,
    std::span<const TracedBuffer> geometry,
    const Framebuffer& framebuffer, const DrawParams& params,
    Maxwell::PrimitiveTopology topology) {
    const auto directory = Common::FS::GetExeDirectory() / "pick_probes";
    std::error_code probe_path_error;
    if (!FC::Active() || pipeline.Key().unique_hashes[1] != 0x17d43777e7441cf4ULL ||
        pipeline.Key().unique_hashes[5] != 0x0b7e4e46d61d3694ULL ||
        !std::filesystem::exists(directory / "enabled.flag", probe_path_error)) return;
    if (!params.is_indexed || params.num_vertices != 3 || params.num_instances != 1 ||
        !framebuffer.ColorImage(0) || framebuffer.Extent().width != 4 || framebuffer.Extent().height != 4) {
        FC::Omit("pick_probe_unexpected_draw");
        return;
    }
    try {
        ComPtr<ID3D12Device> device;
        ThrowIfFailed(framebuffer.ColorImage(0)->Handle()->GetDevice(IID_PPV_ARGS(&device)),
                      "pick probe device");
        std::optional<D3D12_INDEX_BUFFER_VIEW> index;
        std::vector<std::pair<u32, D3D12_VERTEX_BUFFER_VIEW>> vertices;
        for (const auto& source : geometry) {
            if (!source.buffer) continue;
            if (source.kind == TracedBuffer::Kind::Index && (source.stride == 2 || source.stride == 4)) {
                index = {source.buffer->Address() + source.offset, source.size,
                         source.stride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
            } else if (source.kind == TracedBuffer::Kind::Vertex) {
                vertices.emplace_back(source.slot, D3D12_VERTEX_BUFFER_VIEW{
                    source.buffer->Address() + source.offset, source.size, source.stride});
            }
        }
        if (!index || vertices.empty() || params.first_index != 0)
            throw std::runtime_error("pick probe: missing or unsupported captured geometry");
        std::array<std::unique_ptr<Image>, 4> images;
        VideoCommon::ImageInfo info{};
        info.type = VideoCommon::ImageType::e2D;
        info.size = {8, 4, 1}; info.resources = {1, 1}; info.num_samples = 1;
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4};
        ComPtr<ID3D12DescriptorHeap> heap;
        ThrowIfFailed(device.Get()->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)),
                      "pick probe RTV heap");
        scheduler.DeferRelease(heap);
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> handles;
        const auto step = device.Get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (u32 i = 0; i < 4; ++i) {
            info.format = i == 3 ? VideoCore::Surface::PixelFormat::R16G16B16A16_UNORM :
                                  VideoCore::Surface::PixelFormat::R32G32B32A32_FLOAT;
            images[i] = std::make_unique<Image>(texture_runtime, info, 0, 0);
            handles[i].ptr = heap->GetCPUDescriptorHandleForHeapStart().ptr + i * step;
            D3D12_RENDER_TARGET_VIEW_DESC view{};
            view.Format = i == 3 ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R32G32B32A32_FLOAT;
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray = {0, 0, 1, 0};
            device.Get()->CreateRenderTargetView(images[i]->Handle(), &view, handles[i]);
        }
        query_cache.DisableStreams();
        SCOPE_EXIT {
            command_state.valid = false;
            query_cache.EnableCounters();
        };
        constexpr u32 passes = 39;
        for (u32 pass = 0; pass < passes; ++pass) {
            std::vector<u8> code;
            if (pass < 2) {
                const auto original = pipeline.DiagnosticDxil(4);
                code.assign(original.begin(), original.end());
            } else {
                std::ifstream file(directory / fmt::format("{}.dxil", pass),
                                   std::ios::binary | std::ios::ate);
                if (!file || file.tellg() < 32 || file.tellg() > (1 << 20))
                    throw std::runtime_error("pick probe: missing or oversized shader");
                code.resize(static_cast<size_t>(file.tellg())); file.seekg(0);
                file.read(reinterpret_cast<char*>(code.data()), code.size());
                if (!file || std::memcmp(code.data(), "DXBC", 4))
                    throw std::runtime_error("pick probe: invalid shader container");
            }
            FC::Data("pick_probe_dxil", fmt::format("{{\"op\":{},\"pass\":{}}}",
                     full_capture_operation, pass), code);
            auto probe = pipeline.CreatePickProbePipeline(code, pass != 0);
            scheduler.DeferRelease(probe);
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3> targets{handles[pass == 0 ? 3 : 0],
                                                            handles[1], handles[2]};
            for (u32 rt = 0; rt < 3; ++rt)
                images[rt == 0 && pass == 0 ? 3 : rt]->Transition(D3D12_RESOURCE_STATE_RENDER_TARGET);
            command_state.valid = false;
            BindDrawState(pipeline, bindings, framebuffer, params, topology, targets);
            auto* cmd = scheduler.CommandList();
            // ApplyGeometry consumes its pending bindings on the original guest draw.
            // Capture readbacks reset the list: every control must explicitly rebind IA.
            cmd->IASetIndexBuffer(&*index);
            for (const auto& [slot, view] : vertices) cmd->IASetVertexBuffers(slot, 1, &view);
            const float sentinel[4]{-99999, -99999, -99999, -99999};
            for (const auto target : targets) cmd->ClearRenderTargetView(target, sentinel, 0, nullptr);
            scheduler.SetPipelineState(probe.Get());
            // Unpredicated control: isolated copies cannot affect a guest query or attachment.
            cmd->DrawIndexedInstanced(params.num_vertices, params.num_instances, params.first_index,
                                      static_cast<INT>(params.base_vertex), params.base_instance);
            FC::Event("pick_probe", fmt::format(
                "{{\"op\":{},\"pass\":{},\"float_outputs\":{},\"predicated\":false}}",
                full_capture_operation, pass, pass != 0));
            for (u32 rt = 0; rt < 3; ++rt) {
                CaptureFullImage(*images[rt == 0 && pass == 0 ? 3 : rt], 0,
                                 fmt::format("pick_probe_{}_rt{}", pass, rt), true);
            }
        }
    } catch (const std::exception& error) {
        command_state.valid = false;
        FC::Omit("pick_probe_exception");
        LOG_ERROR(Render, "D3D12 pick capture probe: {}", error.what());
    }
}

} // namespace D3D12
