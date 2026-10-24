// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from NXbox's maxwell_dma_clear fixture (Copyright 2026 NSPX).

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using u8 = unsigned char;
using u32 = std::uint32_t;
using GPUVAddr = std::uint64_t;
#define ASSERT assert

struct Buffer : std::vector<u8> {
  void resize_destructive(size_t size) { resize(size); }
};

struct Memory {
  std::array<u8, 64> guest{};
  std::array<u8, 64> cached{};
  bool dirty = false;
  unsigned safe_writes = 0;
  unsigned unsafe_writes = 0;
  size_t written_size = 0;

  void WriteBlockUnsafe(GPUVAddr address, const void *bytes, size_t size) {
    assert(address == 4096 && size <= guest.size());
    ++unsafe_writes;
    written_size = size;
    std::memcpy(guest.data(), bytes, size);
  }
  void WriteBlock(GPUVAddr address, const void *bytes, size_t size) {
    WriteBlockUnsafe(address, bytes, size);
    --unsafe_writes;
    ++safe_writes;
    dirty = true;
  }
  void PrepareImage() {
    if (dirty) {
      cached = guest;
      dirty = false;
    }
  }
};

struct Acceleration {
  bool enabled = false;
  unsigned calls = 0;
  Memory *memory = nullptr;
  bool BufferClear(GPUVAddr address, u32 elements, u32 value) {
    assert(address == 4096);
    ++calls;
    if (enabled) {
      for (u32 i = 0; i < elements; ++i) {
        std::memcpy(memory->cached.data() + 4 * i, &value, 4);
      }
    }
    return enabled;
  }
};

struct Engine {
  struct Regs {
    GPUVAddr offset_out = 4096;
    u32 line_length_in = 8;
    struct {
      u32 component_size_minus_one = 3;
      u32 remap_consta_value = 0x591642c8;
    } remap_const;
  } regs;
  Memory memory_manager;
  Acceleration accelerate;
  Buffer read_buffer;

  Engine() { accelerate.memory = &memory_manager; }
  void Fill() {
    // PRODUCTION_FILL
  }
};

int main() {
  unsigned cases = 0;
  for (const u32 component_size : {1U, 2U, 4U}) {
    for (const bool enabled : {false, true}) {
      for (const u32 elements : {1U, 3U, 8U}) {
        Engine engine;
        engine.accelerate.enabled = enabled;
        engine.regs.line_length_in = elements;
        engine.regs.remap_const.component_size_minus_one = component_size - 1;
        engine.Fill();
        const bool accelerated = enabled && component_size == 4;
        assert(engine.accelerate.calls == (component_size == 4 ? 1U : 0U));
        assert(engine.memory_manager.safe_writes == (accelerated ? 0U : 1U));
        assert(engine.memory_manager.unsafe_writes == (accelerated ? 1U : 0U));
        engine.memory_manager.PrepareImage();
        assert(engine.memory_manager.cached == engine.memory_manager.guest);
        // Preserve the existing narrow-fill byte layout and untouched tail.
        std::array<u8, 64> expected{};
        const size_t size = static_cast<size_t>(elements) * component_size;
        for (size_t i = 0; i < size; ++i) {
          expected[i] =
              static_cast<u8>(engine.regs.remap_const.remap_consta_value >>
                              (8 * (i % sizeof(u32))));
        }
        assert(engine.memory_manager.written_size == size);
        assert(engine.memory_manager.guest == expected);
        ++cases;
      }
    }
  }
  assert(cases == 18);
}
