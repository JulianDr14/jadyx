// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Compute pipeline claim gate (D3D12::ComputePipeline on a real device): a build still queued
// behind a busy worker is run by whoever waits for it, once; a build already running on its
// worker is waited for, not run twice; many threads waiting at once (GPU thread, command
// recorder) share one build; a DXIL that fails leaves the PSO null for every waiter; without a
// worker the build runs in the constructor. Run with tools/xbox/tests/run-compute-pipeline-claim.ps1.

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <d3dcompiler.h>

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_d3d12/d3d12_compute_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_stats.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"

using namespace D3D12;
using namespace std::chrono_literals;

namespace {

void Check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        std::exit(1);
    }
}

/// An empty compute shader: it binds nothing, so any compute root signature takes it.
std::vector<u8> EmptyShader() {
    static constexpr char SOURCE[] = "[numthreads(1, 1, 1)] void main() {}";
    ComPtr<ID3DBlob> code, error;
    ThrowIfFailed(D3DCompile(SOURCE, std::strlen(SOURCE), nullptr, nullptr, nullptr, "main",
                             "cs_5_0", 0, 0, &code, &error),
                  "compute shader");
    const auto* bytes = static_cast<const u8*>(code->GetBufferPointer());
    return {bytes, bytes + code->GetBufferSize()};
}

struct Bench {
    Device device;
    RootSignatureCache root_signatures{device};
    Shader::Info info{};
    const PipelineLayout* layout{};
    std::vector<u8> shader{EmptyShader()};

    Bench() {
        const std::array<const Shader::Info*, 1> infos{&info};
        layout = &root_signatures.Get(infos, true);
    }

    /// The compile step counts its runs and can be held or made to fail.
    std::unique_ptr<ComputePipeline> Make(Common::ThreadWorker* worker, std::atomic<int>& runs,
                                          std::chrono::milliseconds delay = 0ms,
                                          bool fail = false, u64 hash = 1) {
        auto compile = [this, &runs, delay, fail] {
            runs.fetch_add(1);
            std::this_thread::sleep_for(delay);
            if (fail) {
                throw std::runtime_error("DXIL failed on purpose");
            }
            return shader;
        };
        return std::make_unique<ComputePipeline>(device, nullptr, worker, hash, compile, info,
                                                 *layout);
    }
};

/// Holds a worker's only thread until released.
struct Blocker {
    std::atomic_bool started{};
    std::atomic_bool release{};

    void Queue(Common::ThreadWorker& worker) {
        worker.QueueWork([this] {
            started = true;
            started.notify_all();
            release.wait(false);
        });
        started.wait(false);
    }
    void Release() {
        release = true;
        release.notify_all();
    }
};

void TestClaimQueued(Bench& bench) {
    Common::ThreadWorker worker{1, "ClaimTest"};
    Blocker blocker;
    blocker.Queue(worker);
    std::atomic<int> runs{0};
    const u64 inline_before = GetPipelineStats().inline_builds.load();
    auto pipeline = bench.Make(&worker, runs);
    Check(!pipeline->IsBuilt(), "queued behind a busy worker, not built yet");
    Check(pipeline->Handle() == nullptr, "no handle while building");
    pipeline->WaitBuilt(); // the worker is still held: only the claim can finish this
    Check(pipeline->IsBuilt() && pipeline->Handle() != nullptr, "the waiter built it");
    Check(runs == 1, "the claimed build ran once");
    Check(GetPipelineStats().inline_builds.load() == inline_before + 1, "counted as inline");
    blocker.Release();
    worker.WaitForRequests();
    Check(runs == 1, "the worker's job skips a build already claimed");
    std::cout << "  claim of a queued build PASS\n";
}

void TestWaitRunning(Bench& bench) {
    Common::ThreadWorker worker{1, "ClaimTest"};
    std::atomic<int> runs{0};
    auto pipeline = bench.Make(&worker, runs, 100ms);
    while (runs == 0) {
        std::this_thread::yield();
    }
    // Running on the worker now: waited for, not taken over.
    const u64 inline_before = GetPipelineStats().inline_builds.load();
    pipeline->WaitBuilt();
    Check(pipeline->IsBuilt() && pipeline->Handle() != nullptr, "built by the worker");
    Check(runs == 1, "not run a second time by the waiter");
    Check(GetPipelineStats().inline_builds.load() == inline_before, "no inline build");
    worker.WaitForRequests();
    std::cout << "  wait for a running build PASS\n";
}

void TestManyWaiters(Bench& bench) {
    Common::ThreadWorker worker{1, "ClaimTest"};
    Blocker blocker;
    blocker.Queue(worker);
    std::atomic<int> runs{0};
    auto pipeline = bench.Make(&worker, runs, 20ms);
    const auto wait_before = GetPipelineStats().Read(PipelineStage::WaitCompute).count;
    constexpr int WAITERS = 8;
    std::vector<ID3D12PipelineState*> handles(WAITERS);
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < WAITERS; ++i) {
            // As the command recorder does for a dispatch recorded before its PSO.
            threads.emplace_back([&, i] { handles[i] = pipeline->WaitHandle(); });
        }
    }
    blocker.Release();
    worker.WaitForRequests();
    Check(runs == 1, "one build for every waiter");
    for (ID3D12PipelineState* handle : handles) {
        Check(handle != nullptr && handle == pipeline->Handle(), "every waiter got the PSO");
    }
    Check(GetPipelineStats().Read(PipelineStage::WaitCompute).count > wait_before,
          "recorder waits are measured");
    Check(pipeline->WaitHandle() == pipeline->Handle(), "built: WaitHandle returns at once");
    std::cout << "  " << WAITERS << " concurrent waiters PASS\n";
}

void TestFailure(Bench& bench) {
    Common::ThreadWorker worker{1, "ClaimTest"};
    Blocker blocker;
    blocker.Queue(worker);
    std::atomic<int> runs{0};
    auto pipeline = bench.Make(&worker, runs, 0ms, true);
    Check(pipeline->WaitHandle() == nullptr, "a failed DXIL gives no PSO");
    Check(pipeline->IsBuilt() && pipeline->Handle() == nullptr, "failed: built, without a PSO");
    Check(!pipeline->OutOfMemory(), "a failed DXIL is not out of memory");
    blocker.Release();
    worker.WaitForRequests();
    Check(runs == 1, "a failed build is not retried");
    std::cout << "  failed DXIL PASS\n";
}

void TestInline(Bench& bench) {
    std::atomic<int> runs{0};
    auto pipeline = bench.Make(nullptr, runs);
    Check(runs == 1 && pipeline->IsBuilt() && pipeline->Handle() != nullptr,
          "without a worker the constructor builds it");
    pipeline->WaitBuilt();
    Check(runs == 1, "nothing left to claim");
    std::cout << "  inline build PASS\n";
}

void TestStress(Bench& bench) {
    // Workers and waiters race for the same builds: each still runs exactly once.
    constexpr int PIPELINES = 200;
    std::vector<std::atomic<int>> runs(PIPELINES);
    std::vector<std::unique_ptr<ComputePipeline>> pipelines;
    {
        Common::ThreadWorker worker{4, "ClaimTest"};
        for (int i = 0; i < PIPELINES; ++i) {
            pipelines.push_back(bench.Make(&worker, runs[i], std::chrono::milliseconds(i % 3), false,
                                           static_cast<u64>(i) + 100));
        }
        std::vector<std::jthread> waiters;
        for (int t = 0; t < 4; ++t) {
            waiters.emplace_back([&, t] {
                for (int i = 0; i < PIPELINES; ++i) {
                    ComputePipeline& pipeline = *pipelines[(i * 7 + t * 53) % PIPELINES];
                    Check(pipeline.WaitHandle() != nullptr, "stress: every PSO built");
                }
            });
        }
        waiters.clear();
        worker.WaitForRequests();
    }
    for (int i = 0; i < PIPELINES; ++i) {
        Check(runs[i] == 1, "stress: every build ran exactly once");
    }
    std::cout << "  stress " << PIPELINES << " pipelines, 4 workers, 4 waiters PASS\n";
}

} // Anonymous namespace

int main() try {
    Common::Log::Initialize();
    Settings::values.renderer_debug = false;
    Bench bench;
    TestClaimQueued(bench);
    TestWaitRunning(bench);
    TestManyWaiters(bench);
    TestFailure(bench);
    TestInline(bench);
    TestStress(bench);
    std::cout << "compute pipeline claim: queued claim, running wait, waiters, failure, inline, "
                 "stress PASS\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
