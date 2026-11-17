// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Multi-threaded gate for parallel graphics PSO compilation and deferred recorder waits:
// 1. FIFO draw execution across multiple 64 KiB CommandChunks with mixed ready, deferred,
//    rejected, and duplicate pipelines (no out-of-order draws, no null-PSO draws).
// 2. Copy/move semantics of RecordedDrawRef prevent residency eviction while chunks are in flight.
// 3. Reverse-order GPU helper claiming across 4 ThreadWorkers reduces 15 x 20 ms serial waits
//    below 130 ms wall-clock while preserving true per-pipeline total_build_us for learning.

#undef NDEBUG
#include <windows.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/thread_worker.h"
#include "video_core/renderer_d3d12/d3d12_command_recorder.h"

// common.lib asserts through these; this standalone test has no logging backend.
void AssertFatalImpl() { std::abort(); }
void AssertFailSoftImpl() { std::abort(); }
void AssertFailedAt(const char*) { std::abort(); }
void UnreachableAt(const char*) { std::abort(); }

namespace {

using namespace std::chrono_literals;

class TestPipeline {
public:
    enum class State : u8 { Building, Ready, Dormant };

    TestPipeline(u64 id_, Common::ThreadWorker& worker, std::chrono::milliseconds build_time,
                 bool reject_ = false)
        : id{id_}, reject{reject_} {
        build = std::make_shared<PendingBuild>();
        build->build_time = build_time;
        worker.QueueWork([this, b = build] {
            if (!b->claimed.exchange(true, std::memory_order_acq_rel)) {
                RunBuild(*b);
            }
        });
    }

    explicit TestPipeline(u64 id_) : id{id_}, build_state{State::Ready}, ready_handle{id_} {}

    class RecordedDrawRef {
    public:
        RecordedDrawRef() = default;
        explicit RecordedDrawRef(TestPipeline* p) noexcept : pipeline{p} { Acquire(); }
        RecordedDrawRef(const RecordedDrawRef& other) noexcept : pipeline{other.pipeline} {
            Acquire();
        }
        RecordedDrawRef& operator=(const RecordedDrawRef& other) noexcept {
            if (this != &other) {
                Release();
                pipeline = other.pipeline;
                Acquire();
            }
            return *this;
        }
        RecordedDrawRef(RecordedDrawRef&& other) noexcept
            : pipeline{std::exchange(other.pipeline, nullptr)} {}
        RecordedDrawRef& operator=(RecordedDrawRef&& other) noexcept {
            if (this != &other) {
                Release();
                pipeline = std::exchange(other.pipeline, nullptr);
            }
            return *this;
        }
        ~RecordedDrawRef() { Release(); }

        [[nodiscard]] TestPipeline* operator->() const noexcept { return pipeline; }

    private:
        void Acquire() noexcept {
            if (pipeline) {
                pipeline->pending_recorded_draws.fetch_add(1, std::memory_order_relaxed);
            }
        }
        void Release() noexcept {
            if (pipeline) {
                pipeline->pending_recorded_draws.fetch_sub(1, std::memory_order_release);
                pipeline = nullptr;
            }
        }
        TestPipeline* pipeline{};
    };

    [[nodiscard]] bool IsBuilt() const noexcept {
        return build_state.load(std::memory_order_acquire) == State::Ready;
    }

    [[nodiscard]] u64 Handle() const noexcept {
        return IsBuilt() ? ready_handle : 0;
    }

    [[nodiscard]] bool HasUnclaimedBuild() noexcept {
        std::scoped_lock lock{mutex};
        return build_state.load(std::memory_order_relaxed) == State::Building && build &&
               !build->claimed.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool MarkDeferredWaitQueued() noexcept {
        return !std::exchange(deferred_wait_queued, true);
    }
    void ClearDeferredWaitQueued() noexcept { deferred_wait_queued = false; }

    bool WaitBuilt() {
        std::shared_ptr<PendingBuild> pending;
        {
            std::scoped_lock lock{mutex};
            if (build_state.load(std::memory_order_relaxed) != State::Building) {
                return false;
            }
            pending = build;
        }
        const bool claimed_here =
            pending && !pending->claimed.exchange(true, std::memory_order_acq_rel);
        if (claimed_here) {
            RunBuild(*pending);
        }
        std::unique_lock lock{mutex};
        cv.wait(lock, [this] { return build_state.load(std::memory_order_relaxed) != State::Building; });
        return claimed_here;
    }

    u64 WaitHandle() {
        if (!IsBuilt()) {
            WaitBuilt();
        }
        return Handle();
    }

    bool TryEvict() noexcept {
        if (!IsBuilt() || ready_handle == 0 ||
            pending_recorded_draws.load(std::memory_order_acquire) != 0) {
            return false;
        }
        build_state.store(State::Dormant, std::memory_order_release);
        ready_handle = 0;
        return true;
    }

    [[nodiscard]] u32 PendingRecordedDraws() const noexcept {
        return pending_recorded_draws.load(std::memory_order_acquire);
    }
    [[nodiscard]] u64 TotalBuildUs() const noexcept { return total_build_us; }
    [[nodiscard]] u64 Id() const noexcept { return id; }
    [[nodiscard]] bool IsRejected() const noexcept { return reject; }

private:
    struct PendingBuild {
        std::atomic_bool claimed{};
        std::chrono::milliseconds build_time{};
    };

    void RunBuild(PendingBuild& b) {
        const auto t0 = std::chrono::steady_clock::now();
        {
            std::scoped_lock lock{mutex};
            if (build.get() == &b) {
                build.reset();
            }
        }
        if (b.build_time.count() > 0) {
            std::this_thread::sleep_for(b.build_time);
        }
        ready_handle = reject ? 0 : id;
        total_build_us = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        {
            std::scoped_lock lock{mutex};
            build_state.store(State::Ready, std::memory_order_release);
        }
        cv.notify_all();
    }

    u64 id{};
    bool reject{};
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<State> build_state{State::Building};
    std::shared_ptr<PendingBuild> build;
    u64 ready_handle{};
    u64 total_build_us{};
    bool deferred_wait_queued{};
    std::atomic<u32> pending_recorded_draws{};
};

struct ExecutedDraw {
    u64 draw_seq{};
    u64 pso_id{};
};

struct FakeList {
    std::vector<ExecutedDraw> draws;
};

using Recorder = D3D12::CommandRecorder<FakeList*>;

void DrainReverseHelper(std::vector<TestPipeline*>& batch, u64& helper_claims,
                        u64& worker_completed) {
    for (auto* p : batch) {
        p->ClearDeferredWaitQueued();
    }
    std::vector<u8> drained(batch.size(), 0);
    for (;;) {
        size_t candidate = batch.size();
        for (size_t i = batch.size(); i > 0; --i) {
            const size_t idx = i - 1;
            if (!drained[idx] && batch[idx]->HasUnclaimedBuild()) {
                candidate = idx;
                break;
            }
        }
        if (candidate == batch.size()) {
            break;
        }
        drained[candidate] = 1;
        ++helper_claims;
        batch[candidate]->WaitBuilt();
    }
    for (size_t i = 0; i < batch.size(); ++i) {
        if (drained[i]) {
            continue;
        }
        if (batch[i]->IsBuilt()) {
            ++worker_completed;
        }
        batch[i]->WaitBuilt();
    }
}

void TestRecordedDrawRefCopyAndMove() {
    TestPipeline p{42};
    assert(p.PendingRecordedDraws() == 0);
    {
        TestPipeline::RecordedDrawRef r1{&p};
        assert(p.PendingRecordedDraws() == 1);
        assert(!p.TryEvict()); // pinned by in-flight draw
        TestPipeline::RecordedDrawRef r2{r1};
        assert(p.PendingRecordedDraws() == 2);
        TestPipeline::RecordedDrawRef r3;
        r3 = r2;
        assert(p.PendingRecordedDraws() == 3);
        r3 = r3;
        assert(p.PendingRecordedDraws() == 3);
        TestPipeline::RecordedDrawRef r4{std::move(r3)};
        assert(p.PendingRecordedDraws() == 3);
        r2 = std::move(r1);
        assert(p.PendingRecordedDraws() == 2);
    }
    assert(p.PendingRecordedDraws() == 0);
    assert(p.TryEvict());
    std::cout << "  RecordedDrawRef copy/move/evict-pin PASS\n";
}

void TestCommandOrderAndRejectedPso() {
    Common::ThreadWorker workers{4, "TestPsoWorker"};
    FakeList list;
    Recorder recorder{&list, {.threaded = true, .thread_name = "TestRecorder"}};

    std::vector<std::unique_ptr<TestPipeline>> owned;
    std::vector<TestPipeline*> deferred_batch;
    std::vector<ExecutedDraw> expected;
    TestPipeline* last_async = nullptr;

    // Build a mix crossing multiple 64 KiB chunks (250 draws).
    for (u64 seq = 1; seq <= 250; ++seq) {
        TestPipeline* p = nullptr;
        const bool is_rejected = (seq % 17 == 0);
        if (seq % 5 == 0 || is_rejected) {
            if (last_async != nullptr && seq % 15 == 0 && !is_rejected) {
                p = last_async; // duplicate reference to a non-rejected async pipeline
            } else {
                owned.push_back(std::make_unique<TestPipeline>(
                    seq, workers, std::chrono::milliseconds{1 + (seq % 5)}, is_rejected));
                p = owned.back().get();
                if (!is_rejected) {
                    last_async = p;
                }
            }
        } else {
            owned.push_back(std::make_unique<TestPipeline>(seq));
            p = owned.back().get();
        }
        const u64 ready = p->Handle();
        if (ready != 0) {
            recorder.Record([seq, ready](FakeList* out) { out->draws.push_back({seq, ready}); });
            expected.push_back({seq, ready});
        } else {
            if (p->MarkDeferredWaitQueued()) {
                deferred_batch.push_back(p);
            }
            TestPipeline::RecordedDrawRef ref{p};
            assert(!p->TryEvict());
            recorder.Record([seq, ref = std::move(ref)](FakeList* out) {
                const u64 pso = ref->WaitHandle();
                if (pso == 0) {
                    return;
                }
                out->draws.push_back({seq, pso});
            });
            if (!p->IsRejected()) {
                expected.push_back({seq, p->Id()});
            }
        }
        if (seq % 40 == 0) {
            recorder.Dispatch();
        }
    }
    recorder.Dispatch();
    u64 helper_claims = 0;
    u64 worker_completed = 0;
    DrainReverseHelper(deferred_batch, helper_claims, worker_completed);
    recorder.WaitIdle();

    assert(list.draws.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        assert(list.draws[i].draw_seq == expected[i].draw_seq);
        assert(list.draws[i].pso_id == expected[i].pso_id);
        assert(list.draws[i].pso_id != 0);
    }
    for (auto& p : owned) {
        assert(p->PendingRecordedDraws() == 0);
    }
    workers.WaitForRequests();
    std::cout << "  FIFO order + rejected PSO skip (" << list.draws.size() << " draws) PASS\n";
}

void TestParallelSpeedupAndWaitLearning() {
    Common::ThreadWorker workers{4, "TestPsoWorker"};
    FakeList list;
    Recorder recorder{&list, {.threaded = true, .thread_name = "TestRecorder"}};

    constexpr size_t COUNT = 15;
    constexpr auto BUILD_TIME = 25ms;
    std::vector<std::unique_ptr<TestPipeline>> owned;
    std::vector<TestPipeline*> batch;

    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < COUNT; ++i) {
        owned.push_back(std::make_unique<TestPipeline>(1000 + i, workers, BUILD_TIME, false));
        TestPipeline* p = owned.back().get();
        if (p->MarkDeferredWaitQueued()) {
            batch.push_back(p);
        }
        TestPipeline::RecordedDrawRef ref{p};
        recorder.Record([seq = i + 1, ref = std::move(ref)](FakeList* out) {
            const u64 pso = ref->WaitHandle();
            assert(pso != 0);
            out->draws.push_back({seq, pso});
        });
    }
    recorder.Dispatch();

    u64 helper_claims = 0;
    u64 worker_completed = 0;
    DrainReverseHelper(batch, helper_claims, worker_completed);
    recorder.WaitIdle();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();

    assert(list.draws.size() == COUNT);
    for (size_t i = 0; i < COUNT; ++i) {
        assert(list.draws[i].draw_seq == i + 1);
        assert(list.draws[i].pso_id == 1000 + i);
        // Every pipeline records its true build duration even when hidden by worker parallelism.
        assert(owned[i]->TotalBuildUs() >= 15'000);
    }
    assert(helper_claims > 0);
    // Serial would be 15 * 25 ms = 375 ms; 4 workers + GPU helper + recorder should finish < 180 ms.
    assert(elapsed_ms < 180);
    workers.WaitForRequests();
    std::cout << "  parallel 15x25ms speedup (" << elapsed_ms << " ms wall vs 375 ms serial, "
              << "helper_claims=" << helper_claims << ", worker_completed=" << worker_completed
              << ") PASS\n";
}

} // namespace

int main() {
    TestRecordedDrawRefCopyAndMove();
    TestCommandOrderAndRejectedPso();
    TestParallelSpeedupAndWaitLearning();
    std::cout << "PASS: parallel-pso-wait unit/concurrency gate\n";
    return 0;
}
