// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

using DAddr = std::uint64_t;
using GPUVAddr = std::uint64_t;
using Range = std::pair<DAddr, std::size_t>;
using Bytes = std::array<unsigned char, 64>;

namespace VideoCommon {
enum class CacheType { None };
}
namespace Settings {
struct Switch {
  bool enabled = false;
  bool GetValue() const { return enabled; }
};
struct Values {
  Switch skip_cpu_inner_invalidation;
} values;
} // namespace Settings

struct Mutex {
  unsigned locks = 0;
  bool held = false;
  void lock() {
    assert(!held);
    held = true;
    ++locks;
  }
  void unlock() {
    assert(held);
    held = false;
  }
};

struct Cache {
  Mutex mutex;
  Bytes cached{};
  std::array<bool, 64> dirty{};
  std::vector<Range> invalidated;

  void InvalidateRegion(DAddr address, std::size_t size) {
    assert(address >= 4096 && address + size <= 4160 && size);
    invalidated.emplace_back(address, size);
    const auto offset = static_cast<std::size_t>(address - 4096);
    std::fill_n(dirty.begin() + static_cast<std::ptrdiff_t>(offset), size,
                true);
  }
  void WriteMemory(DAddr address, std::size_t size) {
    assert(mutex.held);
    InvalidateRegion(address, size);
  }
  void Synchronize(const Bytes &guest) {
    for (std::size_t i = 0; i < cached.size(); ++i) {
      if (dirty[i]) {
        cached[i] = guest[i];
        dirty[i] = false;
      }
    }
  }
};

struct Rasterizer {
  Cache texture_cache;
  Cache buffer_cache;
  Cache query_cache;
  Cache pipeline_cache;

  void InnerInvalidation(std::span<const Range> sequences) {
    // PRODUCTION_INVALIDATION
  }
};

struct Accumulator {
  std::vector<Range> pending;
  void Add(GPUVAddr address, std::size_t size) {
    pending.emplace_back(address, size);
  }
  template <typename Callback> bool InvalidateAll(Callback callback) {
    const bool any = !pending.empty();
    for (const auto &[address, size] : pending) {
      callback(address, size);
    }
    pending.clear();
    return any;
  }
};

struct MemoryManager {
  Bytes guest{};
  Accumulator accumulator;
  std::vector<Range> page_stash2;
  Rasterizer *rasterizer = nullptr;

  void WriteBlockImpl(GPUVAddr address, const void *bytes, std::size_t size,
                      VideoCommon::CacheType, bool unsafe) {
    assert(unsafe && address >= 8192 && address + size <= 8256);
    std::memcpy(guest.data() + (address - 8192), bytes, size);
  }
  template <bool>
  void GetSubmappedRangeImpl(GPUVAddr address, std::size_t size,
                             std::vector<Range> &stash) {
    // Synthetic GPU-to-device mapping; cache invalidation uses device
    // addresses.
    stash.emplace_back(address - 8192 + 4096, size);
  }
  void WriteBlockCached(GPUVAddr gpu_dest_addr, const void *src_buffer,
                        std::size_t size) {
    // PRODUCTION_WRITE
  }
  void FlushCaching() {
    // PRODUCTION_FLUSH
  }
};

int main() {
  for (const bool skip : {false, true}) {
    Settings::values.skip_cpu_inner_invalidation.enabled = skip;
    Rasterizer rasterizer;
    MemoryManager memory;
    memory.rasterizer = &rasterizer;
    for (const unsigned char value : std::array<unsigned char, 2>{0x53, 0xa7}) {
      const std::array<unsigned char, 12> payload{value, value, value, value,
                                                  value, value, value, value,
                                                  value, value, value, value};
      memory.WriteBlockCached(8192, payload.data(), payload.size());
      memory.WriteBlockCached(8200, payload.data(), payload.size()); // Overlap.
      memory.WriteBlockCached(8232, payload.data(),
                              payload.size()); // Disjoint.
      assert(rasterizer.buffer_cache.cached != memory.guest);
      const unsigned locks_before = rasterizer.texture_cache.mutex.locks;
      memory.FlushCaching();
      assert(memory.page_stash2.empty() && memory.accumulator.pending.empty());
      for (auto *cache :
           {&rasterizer.texture_cache, &rasterizer.buffer_cache,
            &rasterizer.query_cache, &rasterizer.pipeline_cache}) {
        cache->Synchronize(memory.guest);
        assert(cache->cached == memory.guest);
        assert(cache->invalidated.size() == 3);
        assert((cache->invalidated[0] == Range{4096, 12}));
        assert((cache->invalidated[1] == Range{4104, 12}));
        assert((cache->invalidated[2] == Range{4136, 12}));
        cache->invalidated.clear();
      }
      assert(rasterizer.texture_cache.mutex.locks == locks_before + 1);
      assert(rasterizer.buffer_cache.mutex.locks == locks_before + 1);
      memory.FlushCaching(); // Empty drain must not replay the old batch.
      assert(rasterizer.buffer_cache.mutex.locks == locks_before + 1);
    }
    const std::array<Range, 2> invalid{Range{0, 4}, Range{4096, 0}};
    rasterizer.InnerInvalidation(invalid);
    for (auto *cache : {&rasterizer.texture_cache, &rasterizer.buffer_cache,
                        &rasterizer.query_cache, &rasterizer.pipeline_cache}) {
      assert(cache->invalidated.empty());
    }
  }
}
