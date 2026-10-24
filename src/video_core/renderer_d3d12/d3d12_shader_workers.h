// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace D3D12 {

// Boot-only experiment: apply before constructing the renderer, never during compilation.
inline std::atomic_bool parallel_shaders{};
inline void SetParallelShaders(bool enabled) {
    parallel_shaders.store(enabled, std::memory_order_relaxed);
}
inline bool ParallelShadersEnabled() {
    return parallel_shaders.load(std::memory_order_relaxed);
}

constexpr size_t PipelineWorkerCount(unsigned hardware_threads, bool parallel) {
    const size_t available = std::max<size_t>(hardware_threads, 2) - 1;
    return std::min<size_t>(available, parallel ? 3 : 4);
}

/// Exclusive reusable compiler resources. Only checkout/return holds the mutex; creation and
/// compilation run outside it. The owner must outlive its callers and the DLL must outlive this
/// pool. No thread-local COM objects survive unloading a renderer's libraries.
template <class Resource>
class ShaderResourcePool {
    struct Entry {
        Resource resource;
        std::unique_ptr<Entry> next;
    };

public:
    template <class Factory, class Operation>
    void Use(Factory&& factory, Operation&& operation) {
        std::unique_ptr<Entry> entry;
        {
            std::scoped_lock lock{mutex};
            if (idle) {
                entry = std::move(idle);
                idle = std::move(entry->next);
            }
        }
        if (!entry) {
            entry = std::make_unique<Entry>(Entry{.resource = factory(), .next = {}});
        }
        try {
            operation(entry->resource);
        } catch (...) {
            Return(std::move(entry));
            throw;
        }
        Return(std::move(entry));
    }

private:
    void Return(std::unique_ptr<Entry> entry) {
        std::scoped_lock lock{mutex};
        entry->next = std::move(idle);
        idle = std::move(entry);
    }

    std::mutex mutex;
    std::unique_ptr<Entry> idle;
};

} // namespace D3D12
