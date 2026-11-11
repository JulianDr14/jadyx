// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"

namespace D3D12 {

class Scheduler;

/// Captures the next `frames` presented frames into LogDir/glitch/glitch_<n>.txt (any thread).
/// 0 keeps the boot.cfg default ("glitch_frames=", SetGlitchFrames).
void HuntGlitchesNextFrames(u32 frames = 0);
/// Frames a capture takes when the request names none.
void SetGlitchFrames(u32 frames);

/// Geometry glitch hunt: what every draw and dispatch of a few frames read, to tell why
/// geometry deforms and objects flicker. Per draw: outcome, shaders, geometry, the render
/// condition it ran under, and each vertex, index, uniform and storage binding with its guest
/// hash and cache state. Bindings whose host copy should equal guest memory are copied back
/// after the draw and compared with the guest bytes the draw was given, a few frames later, so
/// no capture frame waits for the GPU. Purely observational: it changes no binding path.
/// Read on the PC with tools/xbox/glitch-report.py. GPU thread only.
class GlitchHunt {
public:
    GlitchHunt(const Device& device, Scheduler& scheduler, StagingBufferPool& staging);
    ~GlitchHunt();

    [[nodiscard]] bool Capturing() const noexcept {
        return state == State::Capturing;
    }
    /// Frame being captured, from 0.
    [[nodiscard]] u32 Frame() const noexcept {
        return frame;
    }
    /// Sequence number of the next call in the frame.
    [[nodiscard]] u32 NextCall() noexcept {
        return call++;
    }

    /// After each presented frame. `summary` is appended to the frame's header line.
    void FrameEnd(double interval_ms, std::string_view summary);

    /// Appends a line (no newline) to the capture.
    void Line(std::string_view text);

    /// Copies size bytes of resource at offset (in a copy-readable state) into readback memory
    /// and compares them with guest a few frames later, writing an "M" line on a mismatch.
    /// False when the frame's budget is spent or the range was already compared unchanged.
    /// A resource in `resource_state`, when that is not copy-readable, goes to COPY_SOURCE and back around
    /// the copy, leaving the tracked state as it was.
    bool Compare(u32 call_index, std::string_view role, ID3D12Resource* resource, u64 offset,
                 std::span<const u8> guest, u64 guest_hash,
                 D3D12_RESOURCE_STATES resource_state = D3D12_RESOURCE_STATE_COMMON);

    /// Copies size bytes of resource at offset (copy-readable) back and, a few frames later,
    /// writes an "H" line describing what the GPU held there: float statistics, zero words,
    /// the bounding box of the first three floats of every stride bytes, and a hash. For
    /// buffers the GPU wrote (stream output, compute), which guest memory cannot check.
    bool Inspect(u32 call_index, std::string_view role, ID3D12Resource* resource, u64 offset,
                 u64 size, u32 stride, std::string extra);

    /// Lets copy record a copy of size bytes into (readback, offset), then writes a "T" line
    /// with the u32 words at word_stride bytes apart a few frames later, after `extra`.
    bool InspectWords(u32 call_index, std::string_view role, u64 size, u32 word_stride,
                      std::string extra,
                      const std::function<void(ID3D12Resource*, u64)>& copy);

    /// Copies size bytes of resource back (state handled as in Compare) and, a few frames later,
    /// appends them to the capture's .bin file with a "D" line naming their file offset, size and
    /// `extra`. For the buffers a compute writes, before and after its dispatch (v12).
    bool Dump(u32 call_index, std::string_view role, ID3D12Resource* resource, u64 offset,
              u64 size, D3D12_RESOURCE_STATES resource_state, std::string extra);
    /// Appends guest bytes to the .bin file now, with a "Y" line like Dump's.
    void DumpGuest(u32 call_index, std::string_view role, std::span<const u8> guest,
                   std::string_view extra);
    /// True the first time key is seen in this capture: for what only needs saving once.
    [[nodiscard]] bool FirstInCapture(u64 key) {
        return seen_in_capture.insert(key).second;
    }
    /// Bytes Dump may still reserve this frame (it keeps room for the compares).
    [[nodiscard]] u64 DumpRoom() const noexcept;

    /// Host queries around one recorded draw: what it rasterized, to tell a draw the game
    /// issued but that drew nothing on the host (flicker) from one the game did not issue.
    struct Probe {
        u32 slot;
        bool occlusion; ///< samples that passed depth/stencil
        bool stats;     ///< pipeline statistics: primitives in/out of clipping, shaded pixels
        /// The guest's own occlusion slot isolated around the draw (read instead of ours).
        ID3D12QueryHeap* guest_heap;
        u32 guest_slot;
    };
    /// Begins the queries the guest's own counters leave free (D3D12 does not nest queries of
    /// one type). nullopt when neither is free or the heaps could not be made.
    std::optional<Probe> BeginProbe(bool occlusion, bool stats,
                                    ID3D12QueryHeap* guest_heap = nullptr, u32 guest_slot = 0);
    /// Ends them and, a few frames later, writes a "P" line for call_index.
    void EndProbe(u32 call_index, const Probe& probe);
    /// The call number the next NextCall returns.
    [[nodiscard]] u32 PeekCall() const noexcept {
        return call;
    }

    /// Copies one subresource of a render target back now, waiting for the GPU, and writes a
    /// "V" line after `head`: per-tile channel means and hashes of what the pass left there.
    /// The first frames also save a downsampled PPM next to the capture. transition(true)
    /// records the barriers into COPY_SOURCE, transition(false) the ones back.
    void ReadTarget(std::string_view head, ID3D12Resource* resource, u32 subresource,
                    DXGI_FORMAT format, const std::function<void(bool)>& transition);

private:
    enum class State { Idle, Armed, Capturing, Resolving };
    enum class Kind : u8 { Compare, Inspect, Words, Probe, Dump };
    struct Pending {
        Kind kind;
        u32 frame;
        u32 call;
        std::string role;
        u64 readback_offset;
        std::vector<u8> guest;
        u64 size{};
        u32 stride{};
        std::string extra;
    };
    struct FrameReadback {
        u64 tick{};
        StagingBufferRef ref{};
        u64 used{};
        u64 probe_used{};
        std::vector<Pending> pending;
    };

    /// Readback room of the frame being recorded: its offset in current.ref, or nullopt.
    std::optional<u64> Reserve(u64 size, bool probe = false);
    void ResolveInspect(const Pending& pending, const u8* host);
    void Begin();
    void Resolve(bool all_done);
    void Flush();
    /// Appends bytes to the .bin file; returns their offset in it.
    u64 AppendBin(const u8* data, u64 size);
    void CopyOut(ID3D12Resource* resource, u64 offset, u64 size, u64 at,
                 D3D12_RESOURCE_STATES resource_state);

    const Device& device;
    Scheduler& scheduler;
    StagingBufferPool& staging;
    ComPtr<ID3D12QueryHeap> occlusion_heap;
    ComPtr<ID3D12QueryHeap> stats_heap;
    bool probe_failed{};
    u32 probe_slot{};
    u64 probes{};
    State state{State::Idle};
    u32 frames_left{};
    u32 warmup_left{};
    u32 frame{};
    u32 call{};
    u32 captures{};
    std::string text;
    std::string path;
    std::string bin_path;
    u64 bin_size{};
    std::unordered_set<u64> seen_in_capture;
    u64 dump_used{}; ///< Dump bytes reserved this frame
    u64 targets_read{};
    /// Readbacks of the frame being recorded, then of the frames waiting for the GPU.
    FrameReadback current;
    std::vector<FrameReadback> in_flight;
    /// Ranges compared this frame: (resource ^ offset ^ size, guest hash).
    std::vector<std::pair<u64, u64>> compared;
    u64 compares{};
    u64 inspects{};
    u64 mismatches{};
    u64 skipped_budget{};
};

} // namespace D3D12
