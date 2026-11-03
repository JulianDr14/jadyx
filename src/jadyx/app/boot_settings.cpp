// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <string>

#include <fmt/format.h>
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>

#include "common/host_memory.h"
#include "common/page_table.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "common/windows/cpu_pin.h"
#include "common/windows/timed_wait_profile.h"
#include "common/windows/timer_resolution.h"
#include "core/arm/cpu_profile.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/file_sys/control_metadata.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/nvdrv/devices/nvdisp_disp0.h"
#include "core/hle/service/nvdrv/devices/nvhost_ctrl.h"
#include "core/loader/loader.h"
#include "dynarmic/interface/code_memory.h"
#include "video_core/cpu_write_queue.h"
#include "video_core/diagnostics/full_capture.h"
#include "video_core/fence_manager.h"
#include "video_core/frame_pipeline.h"
#include "video_core/gpu.h"
#include "video_core/gpu_written_pages.h"
#include "video_core/macro.h"
#include "video_core/perf_counters.h"
#include "video_core/performance_overlay.h"
#include "video_core/query_cache.h"
#include "video_core/readback_trace.h"
#include "video_core/renderer_d3d12/d3d12_async_pipelines.h"
#include "video_core/renderer_d3d12/d3d12_frame_gen_pacer.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_key_mode.h"

#include "jadyx/app/boot_diag.h"
#include "jadyx/app/boot_renderer_decls.h"
#include "jadyx/app/boot_settings.h"
#include "jadyx/app/boot_state.h"
#include "jadyx/app/cpu_probe.h"
#include "jadyx/i18n/localization.h"
#include "jadyx/input/uwp_controllers.h"
#include "jadyx/settings/nacp_language.h"
#include "jadyx/settings/uwp_system_settings.h"

namespace Jadyx {

// Force the device-light configuration the boot needs.
static void ApplyHeadlessBootSettings(const BootSurface& surface) {
    Settings::values.renderer_backend = surface.core_window != nullptr
                                            ? Settings::RendererBackend::Direct3D12
                                            : Settings::RendererBackend::Null;
    Settings::values.sink_id = Settings::AudioEngine::XAudio2;
    // The on-console failure mode is a hard crash with no jadyx_log.txt; the default 4 KiB write
    // buffering loses exactly the lines that say where it died. Flush every line instead.
    Settings::values.log_flush_line = true;
    // Player 1 is bound to the virtual_gamepad engine (uwp_input.h) and presented to the game as
    // the controller chosen in the library (a Pro Controller unless told otherwise).
    ApplyControllerStyleSettings(LoadControllerOptions().style);
    // Language and region from Settings > System, or from Windows (the console's own) when they
    // follow the system; once the game is loaded, ApplyGameLanguage narrows the language to what
    // it ships. Also sets the interface language for the in-game menu.
    ApplySystemSettings();
    // memory_layout_mode stays at its default until the Series-S budget is measured on-console; the
    // DRAM clamp is a separate reservation follow-up, not here.
}

/// Many games read the system language directly and fall back to English on their own when they
/// lack it, even when they ship a close variant (Spain's Spanish but not es-419). Give them the
/// closest language they declare instead (PickGameLanguage: variant, then English, then any).
void ApplyGameLanguage(Core::System& system) {
    FileSys::NACP nacp;
    if (system.GetAppLoader().ReadControlData(nacp) != Loader::ResultStatus::Success) {
        return;
    }
    const auto preferred = Settings::values.language_index.GetValue();
    const auto language = PickNacpLanguage(preferred, nacp);
    Settings::values.language_index = language;
    LOG_INFO(Frontend, "Headless boot: game languages {:08X}, preferred {}, system language {}",
             nacp.GetSupportedLanguages(), Settings::CanonicalizeEnum(preferred),
             Settings::CanonicalizeEnum(language));
}

static D3D12::AsyncPipelines ToAsyncPipelines(ShaderBuild shaders) {
    switch (shaders) {
    case ShaderBuild::Balanced: return D3D12::AsyncPipelines::Balanced;
    case ShaderBuild::Wait: return D3D12::AsyncPipelines::Off;
    case ShaderBuild::Skip: break;
    }
    return D3D12::AsyncPipelines::Full;
}

static void ApplyGpuFence(GpuFence fence) {
    using Settings::GpuFenceBehavior;
    Settings::values.gpu_fence_behavior.SetValue(fence == GpuFence::Immediate  ? GpuFenceBehavior::Immediate
                                                 : fence == GpuFence::Balanced ? GpuFenceBehavior::Balanced
                                                 : fence == GpuFence::Accurate ? GpuFenceBehavior::Accurate
                                                                               : GpuFenceBehavior::Default);
    VideoCommon::SetEarlySyncpoints(fence != GpuFence::Wait);
}

/// The game's graphics (its own over Settings > Graphics) where boot.cfg sets none. Upscaling and
/// the GPU fence are written on the GPU thread, the one that reads them once the game runs.
void ApplyGameGraphics(Core::System& system, const BootConfig& config, const GameGraphics& game) {
    const GraphicsSettings graphics = ResolveGraphics(LoadGraphicsSettings(), game);
    if (!config.upscaling) {
        const Upscaling upscaling = graphics.upscaling;
        system.GPU().RunOnGpuThread([upscaling] { ApplyUpscaling(upscaling); });
    }
    if (!config.frame_gen)
        D3D12::SetFrameGenMode(graphics.frame_gen ? D3D12::FrameGenMode::Auto : D3D12::FrameGenMode::Off);
    if (!config.async_pipelines) D3D12::SetAsyncPipelines(ToAsyncPipelines(graphics.shaders));
    if (!config.gpu_fence) {
        const GpuFence fence = graphics.gpu_fence;
        system.GPU().RunOnGpuThread([fence] { ApplyGpuFence(fence); });
    }
    const u32 clock_percent = config.guest_clock_percent.value_or(game.slow_guest_clock ? SlowGuestClockPercent : 100);
    system.CoreTiming().SetGuestClockPercent(clock_percent);
    WriteDiag("graphics: guest clock " + std::to_string(clock_percent) + " %, upscaling " +
              (config.upscaling ? std::string{"from boot.cfg"} : I18n::ToUtf8(I18n::Tr(UpscalingText(graphics.upscaling)))) +
              ", frame generation " +
              (config.frame_gen ? std::string{"from boot.cfg"} : std::string{graphics.frame_gen ? "auto" : "off"}) +
              ", shaders " +
              (config.async_pipelines ? std::string{"from boot.cfg"}
                                      : std::string{graphics.shaders == ShaderBuild::Skip       ? "skip"
                                                    : graphics.shaders == ShaderBuild::Balanced ? "balanced"
                                                                                                : "wait"}) +
              ", GPU fence " +
              (config.gpu_fence ? std::string{"from boot.cfg"} : I18n::ToUtf8(I18n::Tr(GpuFenceText(graphics.gpu_fence)))));
}

/// The advanced graphics that can change while the game runs, those that differ from `before`.
/// With the guest paused (the in-game menu).
void ApplyRunningAdvancedGraphics(Core::System& system, const GameAdvancedGraphics& before,
                                  const GameAdvancedGraphics& now) {
    if (now.texture_cap_mib != before.texture_cap_mib) {
        const u64 cap = u64{now.texture_cap_mib != 0 ? now.texture_cap_mib : BootConfig{}.texture_cap_mib} << 20;
        D3D12::SetTextureMemoryCap(cap);
        g_memory_coordinator_state.texture_floor.store(cap, std::memory_order_relaxed);
        WriteDiag("graphics: texture cap " + std::to_string(cap >> 20) + " MiB");
    }
    if (now.gpu_accuracy != before.gpu_accuracy) {
        const Settings::GpuAccuracy accuracy = now.gpu_accuracy;
        system.GPU().RunOnGpuThread([accuracy] {
            Settings::values.gpu_accuracy = accuracy;
            Settings::UpdateGPUAccuracy();
        });
        WriteDiag(std::string{"graphics: GPU accuracy "} + (accuracy == Settings::GpuAccuracy::Low ? "low" : "high"));
    }
    if (now.console_mode != before.console_mode) {
        Settings::values.use_docked_mode.SetValue(now.console_mode);
        // What a real console tells the game when it goes in or out of its dock.
        system.GetAppletManager().OperationModeChanged();
        WriteDiag(std::string{"graphics: console mode "} +
                  (now.console_mode == Settings::ConsoleMode::Handheld ? "handheld" : "docked"));
    }
}

void ApplyBootConfig(const BootConfig& config, const BootSurface& surface, bool mii_editor) {
    // As yuzu_cmd: the default 15.6 ms timer resolution makes the emulated vsync (and any sleep in
    // the core) tick every 15.6 ms and drop one frame in ten, visible as a stutter.
    const auto timer_resolution = Common::Windows::SetCurrentTimerResolutionToMaximum();
    WriteDiag("step: timer resolution " +
              std::to_string(std::chrono::duration<double, std::milli>(timer_resolution).count()) +
              " ms");
    WriteDiag("step: CPU " + CpuSetReport());
    // Read by each emulator thread when it is named.
    Common::Windows::SetCpuPin(config.cpu_pin);
    if (config.cpu_probe) Jadyx::RunCpuProbe(WriteDiag);
    ApplyHeadlessBootSettings(config.null_renderer ? BootSurface{} : surface);
    // After the System settings, which turn the overlay on when the player has it on.
    if (mii_editor) VideoCore::performance_overlay_enabled.store(false, std::memory_order_relaxed);
    if (config.null_audio) {
        Settings::values.sink_id = Settings::AudioEngine::Null;
    }
    Settings::values.cpu_accuracy = config.cpu_accuracy;
    WriteDiag(config.cpu_accuracy == Settings::CpuAccuracy::Accurate
                  ? "step: CPU accuracy Accurate"
                  : config.cpu_accuracy == Settings::CpuAccuracy::Unsafe ? "step: CPU accuracy Unsafe"
                                                                         : "step: CPU accuracy Auto");
    Settings::values.gpu_accuracy = config.gpu_accuracy;
    Settings::values.dma_accuracy = config.dma_accuracy;
    Settings::UpdateGPUAccuracy();
    // Set on every session: the settings outlive it, and the next game may have other choices.
    Settings::values.max_anisotropy.SetValue(config.anisotropy);
    Settings::values.use_docked_mode.SetValue(config.console_mode);
    WriteDiag("graphics: anisotropy " + std::to_string(static_cast<int>(config.anisotropy)) +
              ", ASTC top level " + (config.astc_drop_mip0 ? "dropped" : "kept") +
              ", texture cap " + std::to_string(config.texture_cap_mib) + " MiB, GPU accuracy " +
              (config.gpu_accuracy == Settings::GpuAccuracy::Low ? "low" : "high") +
              ", shader workers " + std::to_string(config.pso_workers) +
              ", console mode " + (config.console_mode == Settings::ConsoleMode::Handheld ? "handheld" : "docked"));
    AudioCore::Sink::SetXAudio2ProfileEnabled(config.audio_profile);
    Core::CpuProfile::SetEnabled(config.cpu_profile);
    // Read when Core::System builds the kernel's cores.
    Kernel::PhysicalCore::SetIdleSpinPolicy(config.idle_spin);
    // Read by the host timing thread's first wait.
    Common::SetPreciseTimerSleeps(config.timer_sleep);
    if (config.timer_spin_us) {
        Common::SetPreciseTimerSpinCap(std::chrono::microseconds{*config.timer_spin_us});
    }
    Common::Windows::TimedWaitProfile::enabled.store(config.timer_profile, std::memory_order_relaxed);
    // Read by HostMemory when Core::System builds the DRAM, so it has to be set before that.
    // The 384-MiB Series experiment covered only 388 of 2637 MiB requested by Game C (15%). The
    // resulting fastmem fault/recompile storm was markedly slower than the page table, so Auto
    // must take the safe path until a selective JIT/page-table fast path exists. Keep Hybrid and
    // Full as explicit diagnostics.
    const bool fastmem_enabled = config.fastmem == BootConfig::Fastmem::Hybrid ||
                                 config.fastmem == BootConfig::Fastmem::Full;
    Settings::values.cpuopt_fastmem = fastmem_enabled;
    Settings::values.cpuopt_fastmem_exclusives = fastmem_enabled;
    Common::ConfigureHostMemoryFastmem(config.fastmem_hot_mib,
                                       config.fastmem == BootConfig::Fastmem::Full);
    Common::ConfigureGuestSwap({
        .enabled = config.guest_swap,
        .path = (std::filesystem::path{std::wstring{
                     winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()}} /
                 L"jadyx" / L"permafrost.bin")
                    .wstring(),
        .reserve_bytes = u64{config.guest_swap_reserve_mib} << 20,
        .probation_ms = config.guest_swap_probation_ms,
        .min_idle_ms = config.guest_swap_idle_ms,
        .batch_bytes = u64{config.guest_swap_batch_mib} << 20,
        .fill_pages = config.guest_swap_fill_pages,
        .prefetch = config.guest_swap_prefetch,
        .pause_fill = config.guest_swap_pause_fill,
    });
    // Keep the shared setting for other backends. D3D12 prepares on workers and always waits
    // for the exact PSO when a dependent draw reaches the command stream.
    Settings::values.use_asynchronous_shaders.SetValue(config.async_shaders);
    WriteDiag(std::string("step: fastmem ") +
              (config.fastmem == BootConfig::Fastmem::Off      ? "off"
               : config.fastmem == BootConfig::Fastmem::Auto   ? "auto -> page table"
               : config.fastmem == BootConfig::Fastmem::Full   ? "full"
                                                               : "hybrid diagnostic") +
              (fastmem_enabled ? " (hot " + std::to_string(config.fastmem_hot_mib) + " MiB)" : "") +
              ", D3D12 pipeline workers on, dependent draws preserved" +
              ", JIT cache " + std::to_string(config.jit_cache_mib) + " MiB per core");
    D3D12::SetTracedFrame(config.traced_frame, config.traced_frames);
    D3D12::SetMaterialProbe(config.material_probe_x, config.material_probe_y);
    D3D12::SetMaterialCaptureFrame(config.material_capture_frame);
    D3D12::SetMaterialGpuProbeEnabled(config.material_gpu_probe);
    D3D12::SetMaterialSnapshotShaders(config.material_snapshot_ps[0], config.material_snapshot_ps[1]);
    D3D12::SetMaterialSnapshotVertexCounts(config.material_snapshot_vertices[0], config.material_snapshot_vertices[1]);
    D3D12::SetFrameDiagnostics(!config.play);
    // The GPU spends the same 5 GiB as the emulated DRAM and the JIT: the texture and buffer
    // caches evict against what the whole app has left, not DXGI's budget.
    g_test_memory_limit = u64{config.memory_limit_mib} << 20;
    Core::SetJitCodeCacheSize(config.jit_cache_mib << 20);
    Dynarmic::SetCodeCacheCap(0);
    Dynarmic::SetDualMappedCode(config.jit_dual_map);
    Core::SetPageTableExclusives(config.inline_exclusives);
    D3D12::SetAppMemoryQuery(QueryAppMemory);
    D3D12::SetBcArrayDecode(!config.bc_arrays_native);
    D3D12::SetTextureMemoryCap(u64{config.texture_cap_mib} << 20);
    D3D12::SetTextureMemoryLine(
        u64{config.texture_cap_line_mib.value_or(config.guest_swap_watch_mib + 256)} << 20);
    D3D12::SetHeapCompaction(u64{config.heap_compaction_mib} << 20);
    D3D12::SetSmallTextureAlignment(config.small_textures);
    D3D12::SetSpareHeaps(config.spare_heaps);
    D3D12::SetGpuUnswizzle(config.gpu_unswizzle);
    D3D12::MemoryReserve::Configure(u64{config.oom_reserve_mib} << 20);
    // Settings > Graphics until the game's own are known (ApplyGameGraphics).
    const GraphicsSettings global_graphics = LoadGraphicsSettings();
    D3D12::SetAsyncPipelines(config.async_pipelines.value_or(ToAsyncPipelines(global_graphics.shaders)));
    D3D12::SetWaitSmallDraws(config.wait_small_draws);
    D3D12::SetPipelineWorkers(config.pso_workers);
    D3D12::SetPipelineBootWorkers(config.pso_boot_workers);
    D3D12::SetPsoBlobs(config.pso_blobs);
    D3D12::SetCachedStaging(config.cached_staging);
    D3D12::SetCpuTextureWrites(config.cpu_texture_writes);
    // RGBA8 ASTC made loading frames upload 150-260 MiB at once and the console run out of memory
    // (0.2.50): textures become BC3 (a quarter of the memory), 2D arrays too unless
    // astc_arrays=rgba (BootConfig::astc_arrays_rgba), all decoded and encoded on the GPU.
    const bool astc_rgba = config.astc == BootConfig::Astc::Cpu ||
                           config.astc == BootConfig::Astc::GpuRgba;
    Settings::values.astc_recompression.SetValue(astc_rgba
                                                     ? Settings::AstcRecompression::Uncompressed
                                                     : Settings::AstcRecompression::Bc3);
    VideoCommon::SetAstcArrayRecompression(!config.astc_arrays_rgba);
    VideoCommon::SetOpaqueAstcToBc1(!config.astc_opaque_bc3);
    D3D12::SetAstcDropTopLevel(config.astc_drop_mip0);
    // The compute decoder was opt-in until its dispatches set spirv_to_dxil's compute runtime data
    // (0.2.58): 4 minutes of Game C on the Series decoded no ASTC on the CPU, without
    // corruption or hangs. "astc=bc3" brings back the CPU path.
    D3D12::SetAstcGpuDecode(config.astc == BootConfig::Astc::Gpu ||
                            config.astc == BootConfig::Astc::GpuRgba);
    D3D12::SetAstcGpuVerify(config.astc_verify);
    D3D12::SetStencilBlitVerify(config.stencil_blit_verify);
    D3D12::SetDxilSignMode(static_cast<D3D12::DxilSignMode>(config.dxil_sign));
    D3D12::SetDxilDiskCacheEnabled(config.dxil_cache);
    D3D12::SetAstcGpuSync(config.astc_sync);
    D3D12::SetAstcGpuFresh(config.astc_fresh);
    D3D12::SetDescriptorRemovalChecks(config.descriptor_checks);
    D3D12::SetDredEnabled(config.dred);
    D3D12::SetGpuBreadcrumbsEnabled(config.gpu_breadcrumbs);
    D3D12::SetCloseProbeEnabled(config.close_probe);
    D3D12::SetPipelineEviction(config.pso_eviction);
    D3D12::SetPipelinePrewarm(config.pso_prewarm);
    D3D12::SetTransformFeedbackStorage(config.xfb_storage);
    Tegra::SetIndirectMacroCpuRead(config.indirect_macro_cpu);
    VideoCommon::force_occlusion_visible.store(config.occlusion_force_visible,
                                               std::memory_order_relaxed);
    D3D12::SetDiagUavSync(config.uav_sync);
    VideoCommon::SetMipRenderTargets(config.mip_render_targets);
    VideoCommon::SetRenderTargetInheritance(config.render_target_inheritance);
    VideoCommon::SetReclaimSupersededImages(config.reclaim_superseded_images);
    D3D12::SetForceMinMaxFilter(config.force_min_max_filter);
    D3D12::SetSkipMinMaxFilter(config.skip_min_max_filter);
    D3D12::SetEmulateMinMaxFilter(config.emulate_min_max_filter);
    D3D12::HuntOnCompute(config.hunt_cs);
    D3D12::SetComputeInputsShader(config.cs_inputs);
    VideoCore::Perf::SetDetailedGpuProfile(config.gpu_profile);
    if (config.dump_guest_shaders) Settings::values.dump_guest_shaders.SetValue(true);
    VideoCommon::FullCapture::Enable(config.full_gpu_capture);
    VideoCommon::FullCapture::RequestSelfTest(config.full_gpu_capture_self_test);
    VideoCommon::FullCapture::SetTargetsAfterOnly(config.full_gpu_capture_targets_after);
    VideoCommon::ReadbackTrace::SetWriteOrigins(config.buffer_write_origins);
    if (config.full_gpu_capture) {
        const auto version = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
        VideoCommon::FullCapture::SetSession(fmt::format(
            "{{\"version\":\"{}.{}.{}.{}\",\"memory_limit_mib\":{},\"async_record\":{},"
            "\"async_present\":{},\"async_shaders\":{},\"targets_after_only\":{}}}",
            version.Major, version.Minor, version.Build, version.Revision, config.memory_limit_mib,
            config.async_record, config.async_present, config.async_shaders,
            config.full_gpu_capture_targets_after));
    }
    VideoCore::Perf::SetForceSwapIntervalOne(config.force_swap_interval_one);
    Settings::values.async_presentation.SetValue(config.async_present);
    if (config.airplane_mode) Settings::values.airplane_mode.SetValue(true); // else Settings > System
    if (config.upscaling) ApplyUpscaling(*config.upscaling); // before the renderer exists
    D3D12::SetAsyncRecording(config.async_record);
    D3D12::SetDescriptorDeferral(config.descriptor_defer && config.async_record);
    D3D12::SetPresentQueue(config.present_queue);
    D3D12::SetBorderColorMode(static_cast<D3D12::BorderColorMode>(config.border_color));
    D3D12::SetDepthBiasRounding(config.depth_bias_round);
    D3D12::SetUnormHalfDownRounding(config.unorm_round);
    D3D12::SetCubeDirectionSanitize(config.cube_sanitize);
    D3D12::SetHalfFloatFlush(config.half_ftz);
    D3D12::SetShaderAlphaToCoverage(config.a2c_shader);
    D3D12::SetSmallBcGuestPalette(config.small_bc_palette);
    D3D12::SetBcGpuDecode(config.bc_gpu_decode);
    D3D12::SetNotZeroedHeaps(config.not_zeroed_heaps);
    D3D12::SetShaderDepthBias(config.depth_bias_shader);
    D3D12::SetFrameGenMode(config.frame_gen.value_or(global_graphics.frame_gen ? D3D12::FrameGenMode::Auto
                                                                                : D3D12::FrameGenMode::Off));
    D3D12::SetMemoryImportProbe(config.memory_import_probe);
    Service::Nvidia::Devices::nvhost_ctrl::SetFailingEventStall(config.nv_event_stall);
    Service::Nvidia::Devices::nvdisp_disp0::SetReleaseFence(config.nv_release_fence);
    ApplyGpuFence(config.gpu_fence.value_or(global_graphics.gpu_fence));
    D3D12::PipelineKeyTracking::SetMode(config.pipeline_key_reuse);
    D3D12::SetUniformUploadCopy(config.upload_copy);
    D3D12::SetUniformReuse(config.uniform_reuse);
    VideoCommon::SetRenderTargetEarlyOut(config.rt_early_out);
    D3D12::SetStageViewCache(config.stage_view_cache);
    D3D12::SetTextureReplay(static_cast<D3D12::TextureReplayMode>(config.texture_replay));
    VideoCore::FramePipeline::SetPresentMailbox(config.present_mailbox);
    VideoCore::FramePipeline::SetVsyncNowait(config.vsync_nowait);
    VideoCore::FramePipeline::SetSwapIntervalHold(config.swap_interval_hold);
    D3D12::SetPsoBudget(config.pso_budget);
    D3D12::SetPsoLoading(config.pso_loading);
    VideoCommon::CpuWriteQueue::SetMode(config.cpu_write_defer);
    // The JIT load table and the write queue trust the bitmap, so it is kept for them too.
    VideoCommon::GpuWrittenPages::SetEnabled(
        config.dma_flush_filter ||
        config.cpu_write_defer != VideoCore::FramePipeline::Mode::Off ||
        config.jit_read_table != VideoCore::FramePipeline::Mode::Off);
    static_assert(u8(Common::PageTable::JitReadTable::On) == u8(VideoCore::FramePipeline::Mode::On) &&
                  u8(Common::PageTable::JitReadTable::Check) ==
                      u8(VideoCore::FramePipeline::Mode::Check));
    Common::PageTable::SetJitReadTable(
        static_cast<Common::PageTable::JitReadTable>(config.jit_read_table));
    D3D12::SetDrawSamplerReuse(config.draw_sampler_reuse);
    D3D12::SetPixelShaderScratchMode(static_cast<D3D12::PixelShaderScratchMode>(config.ps_scratch));
    if (config.debug_layer) {
        Settings::values.renderer_debug = true;
        D3D12::SetGpuBasedValidation(config.gpu_validation);
        WriteDiag("step: D3D12 debug layer requested");
    }
    WriteDiag(std::string("step: audio ") + (config.null_audio ? "timed Null" : "XAudio2"));
    WriteDiag(surface.core_window != nullptr && !config.null_renderer
                  ? "step: logging up, renderer Direct3D12 on a " + std::to_string(surface.width) +
                        "x" + std::to_string(surface.height) + " CoreWindow"
                  : std::string("step: logging up, renderer Null"));
}

} // namespace Jadyx
