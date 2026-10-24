// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#undef NDEBUG
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "video_core/renderer_d3d12/d3d12_shader_workers.h"

int main() {
    using namespace D3D12;
    assert(!ParallelShadersEnabled());
    for (unsigned cores = 0; cores < 32; ++cores) {
        const size_t legacy = std::min<size_t>(std::max<size_t>(cores, 2) - 1, 4);
        assert(PipelineWorkerCount(cores, false) == legacy);
        assert(PipelineWorkerCount(cores, true) == std::min<size_t>(legacy, 3));
    }
    SetParallelShaders(true);
    assert(ParallelShadersEnabled());
    SetParallelShaders(false);

    std::atomic<int> created{}, destroyed{};
    struct Resource {
        std::atomic<int>& destruction_count;
        std::atomic_bool busy{};
        ~Resource() {
            ++destruction_count;
        }
    };
    {
        ShaderResourcePool<std::unique_ptr<Resource>> pool;
        auto factory = [&] {
            ++created;
            return std::unique_ptr<Resource>{
                new Resource{.destruction_count = destroyed, .busy = false}};
        };
        std::mutex gate_mutex;
        std::condition_variable gate;
        int entered = 0;
        std::array<std::thread, 3> workers;
        for (auto& worker : workers) {
            worker = std::thread([&] {
                pool.Use(factory, [&](auto& resource) {
                    assert(!resource->busy.exchange(true));
                    std::unique_lock lock{gate_mutex};
                    ++entered;
                    gate.notify_all();
                    // A global operation lock makes this fail instead of hanging the test.
                    assert(
                        gate.wait_for(lock, std::chrono::seconds(5), [&] { return entered == 3; }));
                    resource->busy.store(false);
                });
            });
        }
        for (auto& worker : workers)
            worker.join();
        assert(created == 3 && destroyed == 0);
        for (int i = 0; i < 100; ++i) {
            try {
                pool.Use(factory, [](auto&) { throw std::runtime_error("validation failed"); });
                assert(false);
            } catch (const std::runtime_error&) {
            }
            pool.Use(factory, [](auto& resource) { assert(!resource->busy.load()); });
        }
        assert(created == 3); // Failure and sequential calls reuse an exclusive instance.
        ShaderResourcePool<std::unique_ptr<Resource>> empty;
        try {
            empty.Use(
                []() -> std::unique_ptr<Resource> { throw std::runtime_error("factory failed"); },
                [](auto&) { assert(false); });
            assert(false);
        } catch (const std::runtime_error&) {
        }
        empty.Use(factory, [](auto& resource) { assert(resource); });
        assert(created == 4);
    }
    assert(destroyed == created); // No TLS instances outliving the DLL owner.
    std::cout << "PASS: worker counts, default off, three exclusive concurrent resources, reuse, "
                 "exceptions and destruction\n";
}
