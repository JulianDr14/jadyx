// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Windows ABI adapter: execute the production thunk's emitter operations on
// ARM64 too.
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <functional>
#include <type_traits>
using u64 = std::uint64_t;
using u32 = std::uint32_t;
constexpr u32 EXCEPTION_ACCESS_VIOLATION = 0xc0000005;
constexpr u32 EXCEPTION_UNWINDING = 2;
constexpr u32 EXCEPTION_EXIT_UNWIND = 4;
enum Disposition { ExceptionContinueExecution, ExceptionContinueSearch };
struct CONTEXT {
  u64 Rip{};
  u64 Rsp{};
};
using PCONTEXT = CONTEXT *;
struct EXCEPTION_RECORD {
  u32 ExceptionCode{};
  u32 ExceptionFlags{};
  u32 NumberParameters{};
  std::array<u64, 2> ExceptionInformation{};
};
struct FakeCall {
  u64 call_rip{};
  u64 ret_rip{};
};
struct Register {
  unsigned index;
};
struct Address {
  Register base;
  int offset;
};
Address operator-(Register base, int offset) {
  Address address{};
  address.base = base;
  address.offset = -offset;
  return address;
}
struct Operand {
  Address operator[](Address address) { return address; }
};
struct Code {
  Register rsp{.index = 0}, ABI_PARAM1{.index = 1}, ABI_PARAM2{.index = 2},
      ABI_PARAM3{.index = 3}, ABI_PARAM4{.index = 4}, eax{.index = 5};
  Operand ptr;
  std::array<u64, 6> registers{};
  alignas(16) std::array<u64, 32> stack{};
  u64 entry{};
  static constexpr u64 sentinel = 0x12345678;
  Code() {
    entry = reinterpret_cast<u64>(&stack[17]);
    registers[0] = entry;
    stack[17] = sentinel;
  }
  void lea(Register destination, Address address) {
    registers[destination.index] =
        registers[address.base.index] + address.offset;
  }
  void mov(Register destination, Register source) {
    registers[destination.index] = registers[source.index];
  }
  void mov(Register destination, u64 value) {
    registers[destination.index] = value;
  }
  void add(Register destination, int value) {
    registers[destination.index] += value;
  }
  template <class Callback, class... Args>
  void invoke(Callback callback, Args... args) {
    if constexpr (std::is_void_v<std::invoke_result_t<Callback, Args...>>) {
      callback(args...);
    } else {
      registers[eax.index] = callback(args...);
    }
  }
  template <class Callback> void CallLambda(Callback callback) {
    assert(registers[0] % 16 == 0);
    auto *home = reinterpret_cast<u64 *>(registers[0]);
    for (unsigned i = 0; i < 4; ++i)
      home[i] = registers[i + 1];
    assert(stack[17] ==
           sentinel); // All four legal home spills preserve the return PC.
    auto &cb = *reinterpret_cast<std::function<FakeCall(u64)> *>(registers[1]);
    auto *ctx = reinterpret_cast<PCONTEXT>(registers[2]);
    if constexpr (std::is_invocable_v<Callback, decltype(cb), PCONTEXT,
                                      const EXCEPTION_RECORD *>) {
      invoke(callback, cb, ctx,
             reinterpret_cast<const EXCEPTION_RECORD *>(registers[3]));
    } else {
      invoke(callback, cb, ctx);
    }
  }
  void ret() { assert(registers[0] == entry && stack[17] == sentinel); }
};
int main() {
  for (unsigned scenario = 0; scenario < SCENARIOS; ++scenario) {
    Code code;
    std::array<u64, 4> guest_stack{};
    CONTEXT context{};
    context.Rip = 0x1234;
    context.Rsp = reinterpret_cast<u64>(guest_stack.data() + 4);
    const auto original_sp = context.Rsp;
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = scenario == 1 ? 1 : 0;
    record.ExceptionInformation[1] = 0x10000;
    if (scenario == 2)
      record.ExceptionCode = 0x80000003; // Breakpoint.
    if (scenario == 3)
      record.NumberParameters = 1;
    if (scenario == 4)
      record.ExceptionInformation[0] = 8; // Execute AV.
    if (scenario == 5)
      record.ExceptionFlags = EXCEPTION_UNWINDING;
    if (scenario == 6)
      record.ExceptionFlags = EXCEPTION_EXIT_UNWIND;
    unsigned calls = 0;
    std::function<FakeCall(u64)> cb = [&](u64 rip) {
      assert(rip == 0x1234);
      ++calls;
      FakeCall result{};
      result.call_rip = scenario == 7 ? 0 : 0x5678;
      result.ret_rip = scenario == 8 ? 0 : 0x1238;
      return result;
    };
    code.registers[1] = reinterpret_cast<u64>(&record);
    code.registers[3] = reinterpret_cast<u64>(&context);
    // PRODUCTION
    const bool handled = scenario < 2;
    assert(calls == (scenario < 2 || scenario >= 7 ? 1U : 0U));
    assert(code.registers[code.eax.index] ==
           (handled ? ExceptionContinueExecution : ExceptionContinueSearch));
    assert(context.Rip == (handled ? 0x5678U : 0x1234U));
    assert(context.Rsp == original_sp - (handled ? 8U : 0U));
    assert(guest_stack[3] == (handled ? 0x1238U : 0U));
  }
}
