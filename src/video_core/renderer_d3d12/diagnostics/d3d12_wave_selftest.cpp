// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <eden_spirv_to_dxil.h>
#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/host_shaders/d3d12_wave_selftest_comp_spv.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/diagnostics/d3d12_wave_selftest.h"

namespace D3D12 {
namespace {

std::atomic_bool wave_selftest{};

constexpr u32 GROUP_SIZE = 64; ///< local_size_x of d3d12_wave_selftest.comp
constexpr u32 GROUPS = 512;
constexpr u32 THREADS = GROUPS * GROUP_SIZE;
constexpr u32 RUNS_PER_MODE = 24;
constexpr u64 RECORDS_BYTES = u64{THREADS} * 16;
constexpr u64 COUNTER_BYTES = 256;
constexpr u64 SLOTS_BYTES = u64{THREADS} * 4;

enum Mode : u32 {
    ShufflePerWarp,
    ShufflePerLane,
    ShufflePerWarpUniform,
    LeaderRead,
    LeaderReadUniform,
    AggregatedAtomic,
    AggregatedAtomicUniform,
    LeaderAtomics,
    LaneAtomics,
    NumModes,
};

constexpr std::array<const char*, NumModes> MODE_NAMES{
    "shuffle_per_warp",          "shuffle_per_lane",          "shuffle_per_warp_uniform",
    "leader_read",               "leader_read_uniform",       "aggregated_atomic",
    "aggregated_atomic_uniform", "leader_atomics",            "lane_atomics",
};

/// Root parameters: push constants, the three buffers (u0..u2) and spirv_to_dxil's runtime data.
constexpr u32 CONSTANTS_PARAM = 0;
constexpr u32 RECORDS_PARAM = 1;
constexpr u32 COUNTER_PARAM = 2;
constexpr u32 SLOTS_PARAM = 3;
constexpr u32 RUNTIME_DATA_PARAM = 4;

struct Record {
    u32 x, y, z, w;
    u32 Lane() const { return x & 0xFF; }
    u32 Subgroup() const { return (x >> 8) & 0xFF; }
    bool Active() const { return (x >> 31) != 0; }
};

/// What lane `lane` of subgroup `subgroup` in group `group` offers to a shuffle.
u32 Offer(u32 lane, u32 subgroup, u32 group) {
    return lane | (subgroup << 8) | (group << 12);
}

/// One mode's findings over its runs.
struct Tally {
    u32 runs{};
    u32 bad_runs{};
    u64 checked{};
    u64 wrong{};
    std::vector<std::string> examples;

    void Wrong(std::string what) {
        ++wrong;
        if (examples.size() < 4) {
            examples.push_back(std::move(what));
        }
    }
};

/// The 32-lane warp a lane belongs to: group, subgroup, lane >> 5.
using WarpKey = std::tuple<u32, u32, u32>;

void CheckRun(Mode mode, const Record* records, u32 counter, const u32* slots, Tally& tally) {
    const u64 wrong_before = tally.wrong;
    // Each warp's ballot word, from which lanes took the branch.
    std::map<WarpKey, u32> ballots;
    u32 num_active = 0;
    for (u32 i = 0; i < THREADS; ++i) {
        const Record& r = records[i];
        if (r.Active()) {
            ++num_active;
            ballots[{i / GROUP_SIZE, r.Subgroup(), r.Lane() >> 5}] |= 1u << (r.Lane() & 31);
        }
    }
    const auto warp_of = [&](u32 i) {
        return WarpKey{i / GROUP_SIZE, records[i].Subgroup(), records[i].Lane() >> 5};
    };
    const auto where = [&](u32 i) {
        const Record& r = records[i];
        return fmt::format("group {} subgroup {} lane {}", i / GROUP_SIZE, r.Subgroup(), r.Lane());
    };
    switch (mode) {
    case ShufflePerWarp:
    case ShufflePerLane:
    case ShufflePerWarpUniform:
        for (u32 i = 0; i < THREADS; ++i) {
            const Record& r = records[i];
            const u32 want = Offer(r.w, r.Subgroup(), i / GROUP_SIZE);
            ++tally.checked;
            if (r.y != want) {
                tally.Wrong(fmt::format("{} read lane {}: want {:x} got {:x}", where(i), r.w, want,
                                        r.y));
            }
        }
        break;
    case LeaderRead:
    case LeaderReadUniform:
        for (u32 i = 0; i < THREADS; ++i) {
            const Record& r = records[i];
            if (!r.Active()) {
                continue;
            }
            ++tally.checked;
            const u32 ballot = ballots[warp_of(i)];
            const u32 leader = 31 - static_cast<u32>(std::countl_zero(ballot));
            const u32 lt = (1u << (r.Lane() & 31)) - 1;
            const u32 want = Offer(leader + (r.Lane() & ~31u), r.Subgroup(), i / GROUP_SIZE);
            if (r.z != ballot) {
                tally.Wrong(fmt::format("{} ballot: want {:08x} got {:08x}", where(i), ballot, r.z));
            } else if (r.w != lt) {
                tally.Wrong(fmt::format("{} lt mask: want {:08x} got {:08x}", where(i), lt, r.w));
            } else if (r.y != want) {
                tally.Wrong(fmt::format("{} leader read: want {:x} got {:x} (ballot {:08x})",
                                        where(i), want, r.y, ballot));
            }
        }
        break;
    case AggregatedAtomic:
    case AggregatedAtomicUniform:
    case LaneAtomics: {
        ++tally.checked;
        if (counter != num_active) {
            tally.Wrong(fmt::format("counter: want {} got {}", num_active, counter));
        }
        // Every active thread must own exactly the slot it wrote.
        std::vector<u32> owners(THREADS, 0);
        for (u32 slot = 0; slot < THREADS; ++slot) {
            if (slots[slot] == 0) {
                continue;
            }
            if (slot >= num_active) {
                tally.Wrong(fmt::format("slot {} past the {} handed out holds thread {}", slot,
                                        num_active, slots[slot] - 1));
            }
        }
        for (u32 i = 0; i < THREADS; ++i) {
            const Record& r = records[i];
            if (!r.Active()) {
                continue;
            }
            ++tally.checked;
            if (r.y >= THREADS) {
                tally.Wrong(fmt::format("{} slot {} out of range", where(i), r.y));
                continue;
            }
            if (owners[r.y]++ != 0) {
                tally.Wrong(fmt::format("{} slot {} handed out twice (ballot {:08x})", where(i),
                                        r.y, mode == LaneAtomics ? 0u : ballots[warp_of(i)]));
            } else if (slots[r.y] != i + 1) {
                tally.Wrong(fmt::format("{} slot {} overwritten by thread {}", where(i), r.y,
                                        slots[r.y] - 1));
            }
        }
        break;
    }
    case LeaderAtomics: {
        ++tally.checked;
        if (counter != num_active) {
            tally.Wrong(fmt::format("counter: want {} got {}", num_active, counter));
        }
        // Each leader's range [old, old + warp count) must be its own.
        std::vector<u8> taken(std::max(num_active, counter), 0);
        for (u32 i = 0; i < THREADS; ++i) {
            const Record& r = records[i];
            if (!r.Active()) {
                continue;
            }
            const u32 ballot = ballots[warp_of(i)];
            if ((r.Lane() & 31) != 31 - static_cast<u32>(std::countl_zero(ballot))) {
                continue;
            }
            ++tally.checked;
            const u32 count = static_cast<u32>(std::popcount(ballot));
            if (r.z != ballot) {
                tally.Wrong(fmt::format("{} ballot: want {:08x} got {:08x}", where(i), ballot, r.z));
            }
            if (r.y > taken.size() || count > taken.size() - r.y) {
                tally.Wrong(fmt::format("{} range [{}, +{}) out of bounds", where(i), r.y, count));
                continue;
            }
            for (u32 s = r.y; s < r.y + count; ++s) {
                if (taken[s]++ != 0) {
                    tally.Wrong(fmt::format("{} range [{}, +{}) overlaps at {}", where(i), r.y,
                                            count, s));
                    break;
                }
            }
        }
        break;
    }
    default:
        break;
    }
    ++tally.runs;
    if (tally.wrong != wrong_before) {
        ++tally.bad_runs;
    }
}

} // Anonymous namespace

void SetWaveSelfTest(bool enabled) {
    wave_selftest.store(enabled, std::memory_order_relaxed);
}

bool TakeWaveSelfTest() {
    return wave_selftest.exchange(false, std::memory_order_relaxed);
}

void RunWaveSelfTest(ID3D12Device* device, const ShaderCompiler& compiler) noexcept {
    try {
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1));
        LOG_WARNING(Render, "D3D12: wave self-test: wave ops {}, lanes {}-{}",
                    options1.WaveOps ? "yes" : "no", options1.WaveLaneCountMin,
                    options1.WaveLaneCountMax);

        std::array<D3D12_ROOT_PARAMETER, 5> params{};
        params[CONSTANTS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[CONSTANTS_PARAM].Constants = {
            .ShaderRegister = 0, .RegisterSpace = PUSH_CONSTANT_SPACE, .Num32BitValues = 4};
        for (const u32 param : {RECORDS_PARAM, COUNTER_PARAM, SLOTS_PARAM}) {
            params[param].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            params[param].Descriptor = {.ShaderRegister = param - RECORDS_PARAM, .RegisterSpace = 0};
        }
        params[RUNTIME_DATA_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[RUNTIME_DATA_PARAM].Constants = {
            .ShaderRegister = 0,
            .RegisterSpace = RUNTIME_DATA_SPACE,
            .Num32BitValues = static_cast<UINT>(sizeof(dxil_spirv_compute_runtime_data) / 4),
        };
        for (auto& param : params) {
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        const D3D12_ROOT_SIGNATURE_DESC root_desc{
            .NumParameters = static_cast<UINT>(params.size()),
            .pParameters = params.data(),
        };
        const ComPtr<ID3D12RootSignature> root =
            CreateRootSignature(device, root_desc, "wave self-test root signature");
        const std::vector<u8> dxil =
            compiler.Compile(D3D12_WAVE_SELFTEST_COMP_SPV, DXIL_SPIRV_SHADER_COMPUTE);
        const D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{
            .pRootSignature = root.Get(),
            .CS = {dxil.data(), dxil.size()},
        };
        ComPtr<ID3D12PipelineState> pipeline;
        ThrowIfFailed(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)),
                      "CreateComputePipelineState (wave self-test)");

        const auto buffer = [&](u64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                D3D12_RESOURCE_FLAGS flags) {
            return CreateCommittedBuffer(device, size, heap, state, flags, "wave self-test buffer");
        };
        constexpr auto UAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const auto records = buffer(RECORDS_BYTES, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_STATE_COMMON, UAV);
        const auto counter = buffer(COUNTER_BYTES, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_STATE_COMMON, UAV);
        const auto slots = buffer(SLOTS_BYTES, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_STATE_COMMON, UAV);
        const auto zeros = buffer(SLOTS_BYTES, D3D12_HEAP_TYPE_UPLOAD,
                                  D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
        const auto download = buffer(RECORDS_BYTES + COUNTER_BYTES + SLOTS_BYTES,
                                     D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                                     D3D12_RESOURCE_FLAG_NONE);
        void* zero_data{};
        const D3D12_RANGE nothing{};
        ThrowIfFailed(zeros->Map(0, &nothing, &zero_data), "Map (wave self-test zeros)");
        std::memset(zero_data, 0, SLOTS_BYTES);
        zeros->Unmap(0, nullptr);

        const D3D12_COMMAND_QUEUE_DESC queue_desc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
        ComPtr<ID3D12CommandQueue> queue;
        ThrowIfFailed(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)),
                      "CreateCommandQueue (wave self-test)");
        ComPtr<ID3D12CommandAllocator> allocator;
        ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&allocator)),
                      "CreateCommandAllocator (wave self-test)");
        ComPtr<ID3D12GraphicsCommandList> list;
        ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                                nullptr, IID_PPV_ARGS(&list)),
                      "CreateCommandList (wave self-test)");
        ThrowIfFailed(list->Close(), "Close (wave self-test)");
        ComPtr<ID3D12Fence> fence;
        ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
                      "CreateFence (wave self-test)");
        const HANDLE event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        if (event == nullptr) {
            throw std::runtime_error("wave self-test fence event");
        }
        struct CloseEvent {
            HANDLE handle;
            ~CloseEvent() { CloseHandle(handle); }
        } const close_event{event};

        const auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                 D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER b{.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
            b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
            list->ResourceBarrier(1, &b);
        };
        std::array<Tally, NumModes> tallies{};
        u64 tick = 0;
        for (u32 mode = 0; mode < NumModes; ++mode) {
            for (u32 run = 0; run < RUNS_PER_MODE; ++run) {
                // Every fourth run takes the branch on all threads, the rest on a varying share.
                const std::array<u32, 4> constants{mode, run % 4 == 0 ? 100u : 20u + run * 7 % 70,
                                                   0x9E3779B9u * (run + 1), 0};
                const std::array<u32, sizeof(dxil_spirv_compute_runtime_data) / 4> runtime_data{
                    GROUPS, 1, 1};
                ThrowIfFailed(allocator->Reset(), "Reset (wave self-test)");
                ThrowIfFailed(list->Reset(allocator.Get(), pipeline.Get()),
                              "Reset list (wave self-test)");
                for (ID3D12Resource* const target : {counter.Get(), slots.Get()}) {
                    barrier(target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                }
                list->CopyBufferRegion(counter.Get(), 0, zeros.Get(), 0, COUNTER_BYTES);
                list->CopyBufferRegion(slots.Get(), 0, zeros.Get(), 0, SLOTS_BYTES);
                for (ID3D12Resource* const target : {counter.Get(), slots.Get()}) {
                    barrier(target, D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
                barrier(records.Get(), D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                list->SetComputeRootSignature(root.Get());
                list->SetComputeRoot32BitConstants(CONSTANTS_PARAM, 4, constants.data(), 0);
                list->SetComputeRoot32BitConstants(RUNTIME_DATA_PARAM,
                                                   static_cast<UINT>(runtime_data.size()),
                                                   runtime_data.data(), 0);
                list->SetComputeRootUnorderedAccessView(RECORDS_PARAM,
                                                        records->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(COUNTER_PARAM,
                                                        counter->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(SLOTS_PARAM, slots->GetGPUVirtualAddress());
                list->Dispatch(GROUPS, 1, 1);
                for (ID3D12Resource* const target : {records.Get(), counter.Get(), slots.Get()}) {
                    barrier(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                }
                list->CopyBufferRegion(download.Get(), 0, records.Get(), 0, RECORDS_BYTES);
                list->CopyBufferRegion(download.Get(), RECORDS_BYTES, counter.Get(), 0,
                                       COUNTER_BYTES);
                list->CopyBufferRegion(download.Get(), RECORDS_BYTES + COUNTER_BYTES, slots.Get(),
                                       0, SLOTS_BYTES);
                for (ID3D12Resource* const target : {records.Get(), counter.Get(), slots.Get()}) {
                    barrier(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                }
                ThrowIfFailed(list->Close(), "Close (wave self-test)");
                ID3D12CommandList* const lists[]{list.Get()};
                queue->ExecuteCommandLists(1, lists);
                ThrowIfFailed(queue->Signal(fence.Get(), ++tick), "Signal (wave self-test)");
                ThrowIfFailed(fence->SetEventOnCompletion(tick, event),
                              "SetEventOnCompletion (wave self-test)");
                if (WaitForSingleObjectEx(event, 10000, FALSE) != WAIT_OBJECT_0) {
                    throw std::runtime_error("GPU fence timeout");
                }
                void* mapped{};
                const D3D12_RANGE whole{0, RECORDS_BYTES + COUNTER_BYTES + SLOTS_BYTES};
                ThrowIfFailed(download->Map(0, &whole, &mapped), "Map (wave self-test)");
                const auto* const bytes = static_cast<const u8*>(mapped);
                std::vector<Record> read(THREADS);
                std::memcpy(read.data(), bytes, RECORDS_BYTES);
                u32 count;
                std::memcpy(&count, bytes + RECORDS_BYTES, sizeof(count));
                std::vector<u32> slot_values(THREADS);
                std::memcpy(slot_values.data(), bytes + RECORDS_BYTES + COUNTER_BYTES, SLOTS_BYTES);
                download->Unmap(0, &nothing);
                CheckRun(static_cast<Mode>(mode), read.data(), count, slot_values.data(),
                         tallies[mode]);
            }
        }
        LOG_WARNING(Render, "D3D12: wave self-test: {} groups of {}, {} runs per mode", GROUPS,
                    GROUP_SIZE, RUNS_PER_MODE);
        for (u32 mode = 0; mode < NumModes; ++mode) {
            const Tally& t = tallies[mode];
            LOG_WARNING(Render, "D3D12: wave self-test {}: {} ({}/{} runs wrong, {} wrong of {})",
                        MODE_NAMES[mode], t.wrong == 0 ? "OK" : "FAIL", t.bad_runs, t.runs,
                        t.wrong, t.checked);
            for (const std::string& example : t.examples) {
                LOG_WARNING(Render, "D3D12: wave self-test {}:   {}", MODE_NAMES[mode], example);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "D3D12: wave self-test failed: {}", e.what());
    }
}

} // namespace D3D12
