// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
using u8 = std::uint8_t;
using u64 = std::uint64_t;
namespace Xbyak {
struct Reg64 {
  unsigned index{};
  unsigned width{64};
  Reg64 cvt32() const {
    auto r = *this;
    r.width = 32;
    return r;
  }
  Reg64 cvt8() const {
    auto r = *this;
    r.width = 8;
    return r;
  }
};
using Reg = Reg64;
struct Address {
  unsigned base{}, index{};
};
Address operator+(Reg64 base, Reg64 index) {
  Address address{};
  address.base = base.index;
  address.index = index.index;
  return address;
}
Reg64 operator*(Reg64 reg, int scale) {
  assert(scale == 1);
  return reg;
}
} // namespace Xbyak
struct Memory {
  Xbyak::Address address{};
  const void *pointer{};
};
struct Operand {
  Memory operator[](Xbyak::Address address) {
    Memory memory{};
    memory.address = address;
    return memory;
  }
  Memory operator[](const void *pointer) {
    Memory memory{};
    memory.pointer = pointer;
    return memory;
  }
};
struct Code {
  Operand ptr, byte;
  std::array<u64, 2> registers{};
  void mov(Xbyak::Reg64 destination, u64 value) {
    assert(destination.width == 64);
    registers[destination.index] = value;
  }
  void movzx(Xbyak::Reg64 destination, Xbyak::Reg64 source) {
    assert(source.width == 8);
    registers[destination.index] = registers[source.index] & 255;
  }
  void movzx(Xbyak::Reg64 destination, Memory memory) {
    const auto address =
        registers[memory.address.base] + registers[memory.address.index];
    registers[destination.index] = *reinterpret_cast<const u8 *>(address);
  }
  void lea(Xbyak::Reg64 destination, Memory memory) {
    // A synthetic JIT mapping far above this process's static table.
    const auto delta =
        reinterpret_cast<u64>(memory.pointer) - 0x700000000000ULL;
    assert(delta <= 0x7fffffffULL || delta >= 0xffffffff80000000ULL);
    registers[destination.index] = reinterpret_cast<u64>(memory.pointer);
  }
};
namespace IR {
struct Inst {};
} // namespace IR
struct Allocator {
  u64 result{};
  std::array<int, 1> GetArgumentInfo(IR::Inst *) { return {}; }
  Xbyak::Reg64 UseScratchGpr(Code &, int) { return {}; }
  Xbyak::Reg64 ScratchGpr(Code &) {
    Xbyak::Reg64 reg{};
    reg.index = 1;
    return reg;
  }
  void DefineValue(Code &code, IR::Inst *, Xbyak::Reg64 reg) {
    result = code.registers[reg.index];
  }
};
struct EmitContext {
  Allocator reg_alloc;
};
struct EmitX64 {
  Code code;
  void EmitSM4AccessSubstitutionBox(EmitContext &, IR::Inst *);
};
// PRODUCTION
int main() {
  EmitX64 emitter;
  EmitContext context;
  for (u64 index = 0; index < 65536; ++index) {
    emitter.code.registers[0] = 0xdeadbeef00000000ULL | index;
    emitter.EmitSM4AccessSubstitutionBox(context, nullptr);
    assert(context.reg_alloc.result == substitution_box[index & 255]);
  }
}
