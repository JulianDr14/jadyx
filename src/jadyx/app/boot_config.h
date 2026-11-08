// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/settings.h"
#include "common/wake_flag.h"
#include "common/windows/cpu_pin.h"
#include "jadyx/app/boot_renderer_decls.h"
#include "jadyx/input/uwp_input.h"
#include "jadyx/settings/graphics_settings.h"
#include "jadyx/settings/upscaling.h"
#include "video_core/frame_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_async_pipelines.h"
#include "video_core/renderer_d3d12/d3d12_frame_gen_pacer.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_key_mode.h"

namespace Jadyx {

/// Games never emit the sentinels; without an explicit time they run this long.
constexpr u32 DEFAULT_GAME_RUN_SECONDS = 120;

/// Optional boot.cfg next to boot.nro (written by package-appx.ps1 -RunSeconds).
struct BootConfig {
    /// > 0: the payload does not emit the sentinels (deko3d examples, games, ...); run it this long.
    u32 run_seconds{};
    /// > 0: the game's bottleneck capture starts this many seconds into the session ("hunt_at="),
    /// so PC runs time a scene with no one at the pad.
    u32 hunt_at{};
    /// > 0: a frame chain timeline (the T key) starts this many seconds in ("frame_trace_at=").
    u32 frame_trace_at{};
    /// > 0: frame chain timelines saved automatically when the game raises its swap interval
    /// (drops to a lower frame rate), 300 vsyncs before and 180 after ("frame_trace_on_drop=").
    u32 frame_trace_on_drop{};
    /// The bottleneck capture starts at the 64th dispatch of this compute shader ("hunt_cs=").
    u64 hunt_cs{};
    /// The storage buffers of this compute shader are dumped around four dispatches ("cs_inputs=").
    u64 cs_inputs{};
    /// > 0: the session closes this many seconds in ("quit_at="), for unattended PC runs.
    u32 quit_at{};
    /// PC test budget; hard process-commit enforcement belongs to local-run.ps1.
    u32 memory_limit_mib{};
    /// Per-core JIT code cache ("jit_cache_mib="). Full, it is cleared with all the blocks'
    /// metadata, so it bounds the JIT's memory: a large game compiled 400+ MiB in two minutes on
    /// the default 512 MiB per core, and 1 GiB with the metadata, without ever releasing any.
    /// At 128 the same game filled it twice during its load (~150 MiB a core) and recompiled
    /// everything; it is committed as it fills, so a smaller game never pays for the rest.
    /// Short of memory, the caches stop growing instead (JIT_CAP_FLOOR): at 128 for a game that
    /// had come close, it compiled 1.2M blocks loading.
    u32 jit_cache_mib{256};
    /// Guest loads and stores call shared thunks ("jit_thunks=0" emits them inline in each block:
    /// no call per access, but much bigger code in the same caches).
    bool jit_thunks{true};
    /// JIT code memory mapped twice, executable and writable, so no page changes protection per
    /// compiled block ("jit_dual_map=0" keeps paged W^X). Used only where the runtime passes a probe.
    bool jit_dual_map{true};
    /// Texture memory past which clean textures unused for a while are evicted even with headroom
    /// left ("texture_cap_mib=", 0 turns it off). They decode again from guest memory when used.
    /// At 1024 a heavy world entry kept 830 MiB of textures through its load and ran out of memory;
    /// at 768 the cap evicts during the load and the same entry held.
    u32 texture_cap_mib{768};
    /// Free app memory the textures leave alone ("texture_cap_line_mib=", 0 keeps the cap fixed).
    /// Above it the cap grows into the rest; texture_cap_mib stays the floor. Below guest swap's low
    /// mark, so cold guest memory leaves first: a world with 1150 MiB of live textures and 1 GiB free
    /// sat over the cap for good, evicting, moving and creating textures all the time (30 heaps a
    /// minute), and came with bursts of half-second frames. 768 leaves room for a load spike (500
    /// MiB in ten seconds was seen) while they evict.
    std::optional<u32> texture_cap_line_mib{768};
    /// Small sampled textures placed at 4 KiB instead of 64 KiB ("small_textures=0" turns it off).
    bool small_textures{true};
    /// A texture heap class that grew gets one spare heap made on another thread while memory
    /// is plentiful ("spare_heaps=0" turns it off).
    bool spare_heaps{true};
    /// Block-linear color textures of 64 KiB and more unswizzled on the GPU on upload
    /// ("gpu_unswizzle=0" brings back the CPU).
    bool gpu_unswizzle{true};
    /// Commit held back for the driver: a pipeline object that runs out of memory releases it and
    /// is created again ("oom_reserve_mib=0" turns it off).
    u32 oom_reserve_mib{32};
    /// What a draw does while its pipeline builds: is skipped, so the game never stops for a new
    /// pipeline ("full"), is skipped unless it is a full-screen pass or a target's first
    /// ("balanced"), or waits ("async_pipelines=off"). Unset follows the game's graphics settings.
    std::optional<D3D12::AsyncPipelines> async_pipelines;
    /// Draws of this many vertices or fewer always wait for their pipeline ("wait_small_draws=",
    /// 0 turns it off).
    u32 wait_small_draws{};
    /// Pipeline build workers, 1-8 ("pso_workers="); 0 keeps the default of 4.
    u32 pso_workers{};
    /// Workers that build the disk cache on the loading screen, 1-12 ("pso_boot_workers="); 0
    /// keeps the default of one per logical CPU.
    u32 pso_boot_workers{};
    /// The driver's compiled PSOs kept on disk and handed back to it ("pso_blobs=1"); off by
    /// default, as the console's driver may not take them.
    bool pso_blobs{};
    /// Upload staging in CPU-cached memory instead of write-combined ("cached_staging=0" turns it
    /// off).
    bool cached_staging{true};
    /// Plain textures in CPU-writable heaps, their first upload written in place
    /// ("cpu_texture_writes=0" turns it off); cache-coherent UMA devices only.
    bool cpu_texture_writes{true};
    /// Over the texture cap, MiB of live textures a frame moves out of mostly free heaps
    /// ("heap_compaction_mib=", 0 keeps them in place).
    u32 heap_compaction_mib{32};
    /// D3D12 debug layer (renderer_debug); PC only, the console has no SDK layers.
    bool debug_layer{};
    /// With the debug layer, GPU-based validation too ("debug_layer=gbv").
    bool gpu_validation{};
    /// DRED breadcrumbs and page-fault tracking ("dred=1"); diagnostic and deliberately opt-in.
    bool dred{};
    /// GPU-written breadcrumbs around every command ("gpu_breadcrumbs=1", turns dred on): a hang
    /// report names the command the GPU never finished. Slows the GPU down.
    bool gpu_breadcrumbs{};
    /// Each command also closed alone on a scratch list ("close_probe=1"): names the call that
    /// fails a command list's Close. Slow, diagnostic only.
    bool close_probe{};
    /// Release idle pipelines' PSOs ("pso_eviction=0" keeps them unless memory nearly runs out).
    bool pso_eviction{true};
    /// Build dormant PSOs in the background while memory is plentiful ("pso_prewarm=0" leaves
    /// them to their first draw).
    bool pso_prewarm{true};
    /// Transform feedback through vertex shader storage writes instead of stream output
    /// ("xfb=storage"): the fallback, for A/B comparison.
    bool xfb_storage{};
    /// Indirect draw macros read GPU-written arguments back on the CPU ("indirect_macro_cpu=1"):
    /// A/B against drawing from the GPU-side arguments.
    bool indirect_macro_cpu{};
    /// Sample counts reach the guest as never zero ("occlusion_force_visible=1"): A/B for
    /// objects the game's occlusion culling drops.
    bool occlusion_force_visible{};
    /// A global UAV barrier after every draw and dispatch ("uav_sync=1"): A/B for flicker from a
    /// barrier the renderer misses.
    bool uav_sync{};
    /// Depth targets at a mip level of a mipmapped image become separate aliased images again
    /// ("mip_rt=0"): reproduces the alias path depth pyramids took before.
    bool mip_render_targets{true};
    /// A render target redrawn at another size uploads guest memory again instead of taking the
    /// previous size's contents on the GPU ("rt_inherit=0").
    bool render_target_inheritance{true};
    bool reclaim_superseded_images{true};
    /// Filtered MIN/MAX sampler reductions go to the hardware although the device reports no
    /// support ("min_max_filter=force").
    bool force_min_max_filter{};
    /// Filtered MIN/MAX samplers filter normally with no shader emulation ("min_max_filter=off"):
    /// A/B for the cost of the emulation; results are wrong.
    bool skip_min_max_filter{};
    /// The shader emulates filtered MIN/MAX reductions the device has ("min_max_filter=emulate"):
    /// times on a desktop GPU the path consoles without the reductions take.
    bool emulate_min_max_filter{};
    /// Look for a removed device after every descriptor a draw writes ("descriptor_checks=1"), to
    /// name the view that removed it; costs a kernel call per descriptor.
    bool descriptor_checks{};
    /// Controller-accessible bounded F7 equivalent, shown only in diagnostic packages.
    bool material_capture_menu{};
    /// Controller bottleneck capture ("hunt=1"): LocalState/jadyx/log/jadyx_hunt.txt, read by
    /// tools/xbox/hunt-report.py. Shown only in diagnostic packages (package-appx.ps1 -Hunt).
    bool hunt_menu{};
    /// Fine per-draw D3D12 CPU timers ("gpu_profile=1"). Off because clock reads are measurable.
    bool gpu_profile{};
    /// Writes every translated guest shader to the dump folder ("dump_guest_shaders=1").
    bool dump_guest_shaders{};
    bool full_gpu_capture{};
    bool full_gpu_capture_self_test{};
    /// Render targets are read back only after each operation ("gpu_capture_targets=after").
    bool full_gpu_capture_targets_after{};
    /// Logs CPU buffer writes lost to GPU-written ranges ("buffer_write_origins=1").
    bool buffer_write_origins{};
    /// Per-core JIT elapsed time and slow callback counts ("cpu_profile=1"). Diagnostic only.
    bool cpu_profile{};
    /// In-process sampling profiler over every named thread ("profile=1"; "profile=full" adds the
    /// gpu_profile and cpu_profile timers): stacks and CPU time per thread in
    /// LocalState/jadyx/log/jadyx_profile.txt, read by tools/xbox/profile-report.py. Off unless a
    /// diagnostic package asks for it (package-appx.ps1 -Profile).
    bool profile{};
    u32 profile_hz{250};      ///< "profile_hz=": samples per second of each running thread.
    u32 profile_window{20};   ///< "profile_window=": seconds per report window.
    /// How idle emulated cores wait for work ("idle_spin=off|adaptive|adaptive:<us>|fixed:<us>").
    Common::SpinPolicy idle_spin{};
    /// Per-game JIT profile, compiled before the guest runs ("jit_prewarm=1", the default),
    /// only learned ("jit_prewarm=record") or disabled ("jit_prewarm=0").
    enum class JitPrewarm { Off, Record, Warm } jit_prewarm{JitPrewarm::Warm};
    /// Fixed prewarm budget per core for A/B tests ("jit_prewarm_mib="), 0 for the learned one
    /// (PrewarmBudget). The learned step is still observed and stored as usual.
    u32 jit_prewarm_mib{};
    /// XAudio2 performance samples and queue counters ("audio_profile=1").
    bool audio_profile{};
    /// Timed silent output for comparison and audio-device diagnosis ("audio=null").
    bool null_audio{};
    /// Diagnostic only: ignore requests for 30 Hz presentation ("force_swap_interval=1").
    bool force_swap_interval_one{};
    /// Copy and present frames on a thread of their own, so the GPU thread never waits for the
    /// display ("async_present=0" presents on the GPU thread, as Present used to).
    bool async_present{true};
    /// Present on a GPU queue of its own, so a frame's copy to the screen does not wait behind
    /// the emulation's next work ("present_queue=0" turns it off).
    bool present_queue{true};
    /// Sampler border colors as the guest GPU keeps them ("border_color=exact|8bit|bw",
    /// D3D12::BorderColorMode).
    u8 border_color{};
    /// Pipelines keyed by the depth bias D3D12 applies, slope and clamp in 1/16 steps
    /// ("depth_bias_round=0" turns it off).
    bool depth_bias_round{true};
    /// Shader outputs to 8-bit UNORM targets round ties down like the guest ("unorm_round=0"
    /// turns it off).
    bool unorm_round{true};
    /// Cube samples with NaN or zero directions read a defined face ("cube_sanitize=0" turns it
    /// off).
    bool cube_sanitize{true};
    /// Lowered fp16 math flushes half denormals as the guest's FTZ ("half_ftz=0" turns it off).
    bool half_ftz{true};
    /// Single-sample alpha-to-coverage with the guest's dither, in the shader ("a2c_shader=0"
    /// turns it off).
    bool a2c_shader{true};
    /// BC1-3 textures up to 32 KiB decoded on the CPU with the guest's palette
    /// ("small_bc_palette=0" turns it off).
    bool small_bc_palette{true};
    /// BC textures kept as plain texels (arrays, unaligned extents, the guest palette) decoded by
    /// a compute shader instead of the CPU ("bc_gpu_decode=0" turns it off).
    bool bc_gpu_decode{true};
    /// Texture heaps and staging created without the OS zeroing their pages
    /// ("not_zeroed_heaps=0" turns it off).
    bool not_zeroed_heaps{true};
    /// Frames generated between the game's ("frame_gen=off|auto|30|60"): auto takes games under
    /// 28 fps (on average) to 30 and under 56 to 60. Unset follows the game's graphics settings
    /// (on is auto).
    std::optional<D3D12::FrameGenMode> frame_gen;
    /// Single-sample depth bias added in the fragment shader ("depth_bias_shader=0" turns it off).
    bool depth_bias_shader{true};
    /// Whether the GPU reads process memory in place, probed and logged at device creation
    /// ("memory_import_probe=1").
    bool memory_import_probe{};
    /// This boot's image upscaling instead of the saved one, for test runs ("upscaling=fsr";
    /// upscaling.h ParseUpscaling lists the values). Not saved.
    std::optional<Upscaling> upscaling;
    /// Make the D3D12 command list calls on a thread of their own, so the GPU thread only records
    /// them ("async_record=0" makes them on the GPU thread, as before the recorder). boot.cfg
    /// wins over Settings > Graphics and the game's own Graphics.
    bool async_record{true};
    /// Draw descriptor tables written on the recording thread ("descriptor_defer=0" writes them on
    /// the GPU thread); needs async_record. On since the console A/B of 0.3.9.111.
    bool descriptor_defer{true};
    /// An NV event the guest keeps giving up on stalls the guest until the GPU reaches it
    /// ("nv_event_stall=1", as before); by default the GPU commands are flushed and the wait goes on.
    bool nv_event_stall{false};
    /// Release the displayed buffers with a fence on the composite ("nv_release_fence=0" releases
    /// them with none and waits for every composite, as before).
    bool nv_release_fence{true};
    /// When the host side of a GPU syncpoint is signalled ("gpu_fence=immediate", "=safe", "=wait",
    /// "=balanced", "=accurate"), over Settings > Graphics and the game's own (GpuFence). "=safe"
    /// waits as often as "=wait" where a GPU-written buffer is downloaded every frame.
    std::optional<GpuFence> gpu_fence;
    /// Keep the previous draw's pipeline while no register its key reads changed
    /// ("pipeline_key_reuse=1"; "=check" refreshes the key and logs every reuse that would have
    /// drawn with another pipeline; "=0" turns it off).
    D3D12::PipelineKeyTracking::Mode pipeline_key_reuse{D3D12::PipelineKeyTracking::Mode::On};
    /// Keys that differ only in state their pipeline cannot show share one pipeline ("pso_fold=on";
    /// "=check" keeps the pipelines apart, logs what it would save and compares the ones it would
    /// merge; "=off"). Off until it has passed on the console.
    D3D12::PipelineKeyFold::Mode pso_fold{D3D12::PipelineKeyFold::Mode::Off};
    /// The rules it may use, a hexadecimal mask ("pso_fold_rules=ff"): one build tests each.
    u32 pso_fold_rules{D3D12::PipelineKeyFold::ALL_RULES};
    // The draw-path reuse below is on since the console A/B of 0.3.9.111 (per-draw CPU down about
    // 15%); each one turns off with "<key>=0".
    bool upload_copy{true}; ///< Short uniform copies through write-combined stores.
    bool uniform_reuse{true}; ///< Rebind unchanged streamed uniforms within a submission.
    bool rt_early_out{true}; ///< Skip re-preparing unchanged attachments per draw ("rt_early_out=0").
    bool stage_view_cache{true}; ///< Reuse each stage's resolved views and samplers.
    /// Bind the last draw's texture descriptors when a draw resolves the same views and samplers
    /// ("texture_replay=0|1|check", D3D12::TextureReplayMode).
    u8 texture_replay{1};
    /// The frame chain (VideoCore::FramePipeline): "present_mailbox=0|1|check" and
    /// "vsync_nowait=0|1|check".
    VideoCore::FramePipeline::Mode present_mailbox{VideoCore::FramePipeline::Mode::On};
    VideoCore::FramePipeline::Mode vsync_nowait{VideoCore::FramePipeline::Mode::On};
    /// "guest_clock_scale=50-100", over the game's own setting (CoreTiming::SetGuestClockPercent).
    std::optional<u32> guest_clock_percent;
    /// "exact_gpu_reads=0|1", over the game's own setting (VideoCore::SetExactGpuReads).
    std::optional<bool> exact_gpu_reads;
    /// "swap_interval_hold=0|1|check".
    VideoCore::FramePipeline::Mode swap_interval_hold{VideoCore::FramePipeline::Mode::Off};
    /// "pso_budget=0|1|check" (D3D12::SetPsoBudget).
    VideoCore::FramePipeline::Mode pso_budget{VideoCore::FramePipeline::Mode::On};
    /// "pso_loading=0|1|check" (D3D12::SetPsoLoading).
    VideoCore::FramePipeline::Mode pso_loading{VideoCore::FramePipeline::Mode::On};
    /// "cpu_write_defer=0|1|check" (VideoCommon::CpuWriteQueue). boot.cfg wins over Settings >
    /// Graphics and the game's own Graphics, where it is off until the player turns it on.
    VideoCore::FramePipeline::Mode cpu_write_defer{VideoCore::FramePipeline::Mode::Off};
    /// "cpu_write_test=1" (VideoCommon::CpuWriteQueue::TestPhase). Diagnostic only.
    bool cpu_write_test{};
    /// "jit_read_table=0|1|check" (Common::PageTable::SetJitReadTable).
    VideoCore::FramePipeline::Mode jit_read_table{VideoCore::FramePipeline::Mode::On};
    /// Pushbuffer reads skip FlushRegion over memory no cache holds GPU writes for.
    bool dma_flush_filter{true};
    bool draw_sampler_reuse{true}; ///< Gather-local TSC resolution.
    /// Where pixel and vertex shaders keep large local memory: 0 auto (boot probe), 1 pool,
    /// 2 private arrays ("ps_scratch=" and "vs_scratch=auto|uav|native",
    /// D3D12::LocalMemoryScratchMode).
    u8 ps_scratch{};
    u8 vs_scratch{};
    /// Indexed local memory of up to this many words stays private when the pool is on
    /// ("scratch_private=N", D3D12::SetLocalMemoryPrivateWords).
    u32 scratch_private{64};
    /// Host timing waits sleep on a high-resolution timer and wait only its lateness on the CPU
    /// ("timer_sleep=0" waits the last timer tick on the CPU, as before).
    bool timer_sleep{true};
    /// Most microseconds a host timing wait spends on the CPU before its deadline
    /// ("timer_spin=<us>"); past that a late timer makes the event late. Unset follows the
    /// timer's learned lateness ("timer_spin=auto"), which on Series keeps nearly every wait on
    /// the CPU: 0.3.8.70 measured a whole core spent there and 2-3 FPS less than with 100 us.
    std::optional<u32> timer_spin_us{100};
    /// Exclusive loads and stores emitted inline through the page table ("inline_exclusives=0"
    /// calls the exclusive monitor for each one, as before 0.3.8.71).
    bool inline_exclusives{true};
    /// Where the guest cores and other named threads run among the app's cores ("cpu_pin=cores"
    /// gives each guest core a physical core, "cpu_pin=split" its first logical processor and the
    /// rest the second ones, "cpu_pin=map:name=core.logical,..." or "cpu_pin=topology" each
    /// named thread where the map says, the default); off leaves them to the scheduler.
    Common::Windows::CpuPin cpu_pin{Common::Windows::CpuPin::Map};
    bool cpu_probe{}; ///< Measures the app's logical processors before the guest (cpu_probe.h).
    bool timer_profile{}; ///< HostTiming waits only; opt-in, separate from draw profiling.
    /// File name of a game in LocalState\games to boot instead of boot.nro (package-appx.ps1 -Game).
    std::string game;
    /// Eden's log filter (e.g. "*:Info HW.GPU:Debug"); empty keeps the default.
    std::string log_filter;
    /// Graphics bugs, unsupported features and every UNIMPLEMENTED/assert, deduplicated and
    /// counted in LocalState/jadyx/log/jadyx_graphics_bugs.{log,json} ("bug_tracker=0" disables it).
    bool bug_tracker{true};
    /// Null renderer even with a window, to tell GPU hangs from CPU ones.
    bool null_renderer{};
    /// Presented frame whose draws the D3D12 renderer logs; 0 keeps its default.
    u32 traced_frame{};
    /// Presented frames the trace spans from traced_frame on (boot.cfg "trace_frames=").
    u32 traced_frames{1};
    s32 material_probe_x{-1}, material_probe_y{-1};
    u32 material_capture_frame{}; // Opt-in deterministic homebrew diagnostic gate; zero in Series package.
    bool material_gpu_probe{};
    std::array<u64, 2> material_snapshot_ps{};
    std::array<u32, 2> material_snapshot_vertices{};
    /// Keep D3D12 block-compressed 2D arrays compressed instead of decoding them on the CPU.
    bool bc_arrays_native{};
    /// How ASTC reaches D3D12 ("astc="): BC3 for single textures and RGBA8 for arrays. "gpu" (the
    /// default since 0.2.59) performs both decode and BC3 encode on the GPU; "bc3" keeps the CPU
    /// reference path and "cpu" expands every image to RGBA8.
    enum class Astc { Bc3, Gpu, GpuRgba, Cpu } astc{Astc::Gpu};
    /// ASTC 2D arrays as BC3 too ("astc_arrays=bc3", the default) or RGBA8 ("astc_arrays=rgba").
    /// RGBA8 is four times the memory: one array of 121 layers took 650 MiB and closed a game at
    /// the 5 GiB limit. BC arrays read some layers wrong on the Series (0.2.44), seen with BC4
    /// arrays uploaded by copy; "rgba" stays as the way back if ASTC arrays show it too.
    bool astc_arrays_rgba{};
    /// ASTC without alpha as BC1, half of BC3 ("astc_opaque=bc1", the default), or as BC3 like
    /// the rest ("astc_opaque=bc3").
    bool astc_opaque_bc3{};
    /// Big mipmapped ASTC textures without their top level, a quarter of their memory
    /// ("astc_mip0=drop", the default), or whole ("astc_mip0=keep").
    bool astc_drop_mip0{true};
    bool astc_verify{};
    bool stencil_blit_verify{};
    /// DXIL signing: 0 the validator, 1 own digest ("dxil_sign=hash"), 2 both ("dxil_sign=verify").
    int dxil_sign{};
    /// Keep signed DXIL across sessions, on by default ("dxil_cache=0" turns it off).
    bool dxil_cache{true};
    bool gpu_failure_probe{};
    bool gpu_removal_probe{};
    unsigned rom_storage_checks{}; // 1=record token, 2=restore after a process restart
    bool file_manager_gate{};
    bool network_import_gate{};
    bool log_export_gate{};
    bool astc_sync{};
    bool astc_fresh{};
    /// Played by hand ("play=1"): runs until the app is closed, without frame dumps or draw trace.
    bool play{};
    /// Every minute of play, the process heaps and the memory map ("memory_audit=0" turns it off):
    /// what the commit no owner accounts for is made of. Walking the heaps locks them for a moment.
    bool memory_audit{true};
    bool pool_trim{true};
    bool host_trim{true};
    /// Permafrost: cold guest memory written to LocalState/jadyx/permafrost.bin when the app runs short
    /// ("guest_swap=0" turns it off, private DRAM backing only). Below guest_swap_watch_mib of headroom the
    /// guest's use of its memory is sampled. Below guest_swap_low_mib granules idle for
    /// guest_swap_idle_ms and then untouched on probation for guest_swap_probation_ms go to disk
    /// until guest_swap_high_mib is free again, guest_swap_batch_mib at a time (0: all at once);
    /// guest_swap_reserve_mib of commit is held for a demand commit that would fail. A read back
    /// pauses the guest threads while it commits and fills its pages (guest_swap_pause_fill=0:
    /// guest_swap_fill_pages pages at a time, unpaused); guest_swap_prefetch=0 leaves hot granules
    /// to the faults.
    bool guest_swap{true};
    u32 guest_swap_watch_mib{1536};
    u32 guest_swap_low_mib{1024};
    u32 guest_swap_high_mib{1280};
    u32 guest_swap_idle_ms{8000};
    u32 guest_swap_probation_ms{1500};
    u32 guest_swap_reserve_mib{32};
    u32 guest_swap_batch_mib{64};
    u32 guest_swap_fill_pages{4};
    bool guest_swap_prefetch{true};
    bool guest_swap_pause_fill{true};
    /// One owner for the free memory (jadyx/app/memory_coordinator.h, "memory_coordinator=0" turns
    /// it off): when Permafrost evicts what the guest reads straight back, the textures and the
    /// trims give way first and only then Permafrost's marks come down, never below
    /// memory_coordinator_floor_mib for its low mark.
    bool memory_coordinator{true};
    u32 memory_coordinator_floor_mib{512};
    /// Guest memory through the host-mapped arena. Xbox uses a bounded hybrid section; "full" is
    /// diagnostic and falls back to hybrid if the complete mapping cannot be created.
    enum class Fastmem { Off, Auto, Hybrid, Full } fastmem{Fastmem::Off};
    u32 fastmem_hot_mib{384};
    /// "cpu_accuracy=accurate|auto|unsafe". Unsafe: reduced-error floating point cut the guest core's
    /// frame enough to stop most falls to 20 fps while the camera turns.
    Settings::CpuAccuracy cpu_accuracy{Settings::CpuAccuracy::Unsafe};
    /// GPU and DMA accuracy ("gpu_accuracy=low|high", "dma_accuracy=default|unsafe|safe"), for
    /// A/B runs of what the high level costs the GPU thread.
    Settings::GpuAccuracy gpu_accuracy{Settings::GpuAccuracy::High};
    /// A game's advanced graphics; there is no boot.cfg key.
    Settings::AnisotropyMode anisotropy{Settings::AnisotropyMode::Default};
    Settings::ConsoleMode console_mode{Settings::ConsoleMode::Docked};
    Settings::DmaAccuracy dma_accuracy{Settings::DmaAccuracy::Default};
    /// Legacy setting for backends with draw-skipping async shaders. D3D12 always preserves
    /// dependent draws, while preparing DXIL/PSOs on workers regardless of this setting.
    bool async_shaders{true};
    /// Buttons to press at given times ("input=25:L+R" lines).
    std::vector<InputStep> input_script;
    /// Pulse phase-2 gate ("pulse_cycles=", with tools/xbox/pulse_nro as boot.nro): capture,
    /// destroy and restore the homebrew this many times (pulse_gate.h). 0 runs normally.
    u32 pulse_cycles{};
    u32 pulse_seed{1}; ///< "pulse_seed=": the random delays between captures
    /// Pulse phase-1 gate ("pulse_store=1", pulse_store_gate.h): the Store in LocalState, with
    /// process terminations across restarts. Runs before the phase-2 gate when both are set.
    bool pulse_store{};
    /// Played from the library: Pulse sessions (pulse_sessions.h), when the player has them on.
    bool pulse_sessions{};
    /// Off after a failed resume, for the fresh boot that follows, or by "pulse_resume=0": the
    /// game boots fresh and the stored session is left as it was.
    bool pulse_resume{true};
    /// "airplane=1": the game sees no internet connection, as in the Switch's airplane mode.
    bool airplane_mode{};
};

/// A game's advanced graphics as its starting config. The session reads them while it sets up, before
/// the game's file is open, and boot.cfg, parsed after this, still wins.
void UseGameAdvancedGraphics(BootConfig& config, u64 program_id);

/// Applies install_path's boot.cfg, when there is one, to `config`. Some keys take effect right here
/// (the ones with no field in BootConfig).
void ParseBootConfigFile(const std::string& install_path, BootConfig& config);

} // namespace Jadyx
