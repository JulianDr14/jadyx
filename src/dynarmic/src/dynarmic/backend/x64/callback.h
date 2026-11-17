// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <functional>
#include <vector>

#include "common/common_types.h"
#include "dynarmic/backend/x64/xbyak.h"

namespace Dynarmic::Backend::X64 {

using RegList = std::vector<Xbyak::Reg64>;

class BlockOfCode;

class Callback {
public:
    virtual ~Callback();

    void EmitCall(BlockOfCode& code) const {
        EmitCall(code, [](RegList) {});
    }

    virtual void EmitCall(BlockOfCode& code, std::function<void(RegList)> fn) const = 0;
    virtual void EmitCallWithReturnPointer(BlockOfCode& code, std::function<void(Xbyak::Reg64, RegList)> fn) const = 0;
};

class SimpleCallback final : public Callback {
public:
    template<typename Function>
    SimpleCallback(Function fn)
            : fn(reinterpret_cast<void (*)()>(fn)) {}

    using Callback::EmitCall;

    void EmitCall(BlockOfCode& code, std::function<void(RegList)> fn) const override;
    void EmitCallWithReturnPointer(BlockOfCode& code, std::function<void(Xbyak::Reg64, RegList)> fn) const override;

private:
    void (*fn)();
};

/// Calls `fn` with a fixed first argument. The argument is either an immediate baked into the
/// code, or a u64 read from the JIT state (ABI_JIT_PTR + offset) at call time, which lets cores
/// that share code each pass their own object.
class ArgCallback final : public Callback {
public:
    template<typename Function>
    ArgCallback(Function fn, u64 arg)
            : fn(reinterpret_cast<void (*)()>(fn)), arg(arg) {}

    /// The same function, its argument loaded from the JIT state at `state_offset`.
    ArgCallback WithArgFromJitState(u32 state_offset) const {
        ArgCallback result = *this;
        result.arg = state_offset;
        result.arg_in_state = true;
        return result;
    }

    /// Immediate argument, or the state offset when IsArgInJitState().
    u64 Arg() const { return arg; }
    bool IsArgInJitState() const { return arg_in_state; }

    using Callback::EmitCall;

    void EmitCall(BlockOfCode& code, std::function<void(RegList)> fn) const override;
    void EmitCallWithReturnPointer(BlockOfCode& code, std::function<void(Xbyak::Reg64, RegList)> fn) const override;

private:
    void EmitLoadArg(BlockOfCode& code, Xbyak::Reg64 reg) const;

    void (*fn)();
    u64 arg;
    bool arg_in_state = false;
};

}  // namespace Dynarmic::Backend::X64
