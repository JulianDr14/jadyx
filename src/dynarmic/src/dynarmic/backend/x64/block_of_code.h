// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <vector>

#include "common/common_types.h"
#include "common/x64/xbyak.h"
#include "dynarmic/mcl/bit.hpp"
#include "dynarmic/backend/x64/xbyak.h"
#include "dynarmic/backend/x64/abi.h"
#include "dynarmic/backend/x64/callback.h"
#include "dynarmic/backend/x64/constant_pool.h"
#include "dynarmic/backend/x64/host_feature.h"
#include "dynarmic/backend/x64/jitstate_info.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/ir/cond.h"

namespace Dynarmic::Backend::X64 {

using CodePtr = const void*;

struct RunCodeCallbacks {
    ArgCallback LookupBlock;
    ArgCallback AddTicks;
    ArgCallback GetTicksRemaining;
    bool enable_cycle_counting;
};

class BlockOfCode final : public Xbyak::CodeGenerator {
public:
    BlockOfCode(RunCodeCallbacks cb, JitStateInfo jsi, size_t total_code_size, std::function<void(BlockOfCode&)> rcp);
    BlockOfCode(const BlockOfCode&) = delete;
    ~BlockOfCode();

    /// Call when external emitters have finished emitting their preludes.
    void PreludeComplete();

    /// Change permissions to RW. This is required to support systems with W^X enforced.
    void EnableWriting();
    /// Change permissions to RX. This is required to support systems with W^X enforced.
    void DisableWriting();
    /// Between EnableWriting and DisableWriting: makes [where, where + size) writable too, for writes
    /// behind the current code pointer (block linking patches). No-op without paged W^X.
    void MakeWritable(CodePtr where, size_t size);

    /// Clears this block of code and resets code pointer to beginning.
    void ClearCache();
    /// Calculates how much space is remaining to use.
    size_t SpaceRemaining() const;
    /// Ensure at least codesize bytes of code cache memory are committed at the current code_ptr.
    void EnsureMemoryCommitted(size_t codesize);

    /// Runs emulated code from code_ptr.
    HaltReason RunCode(void* jit_state, CodePtr code_ptr) const;
    /// Runs emulated code from code_ptr for a single cycle.
    HaltReason StepCode(void* jit_state, CodePtr code_ptr) const;
    /// Code emitter: Returns to dispatcher
    void ReturnFromRunCode(bool mxcsr_already_exited = false);
    /// Code emitter: Returns to dispatcher, forces return to host
    void ForceReturnFromRunCode(bool mxcsr_already_exited = false);
    /// Code emitter: Makes guest MXCSR the current MXCSR
    void SwitchMxcsrOnEntry();
    /// Code emitter: Makes saved host MXCSR the current MXCSR
    void SwitchMxcsrOnExit();
    /// Code emitter: Enter standard ASIMD MXCSR region
    void EnterStandardASIMD();
    /// Code emitter: Leave standard ASIMD MXCSR region
    void LeaveStandardASIMD();
    /// Code emitter: Updates cycles remaining my calling cb.AddTicks and cb.GetTicksRemaining
    /// @note this clobbers ABI caller-save registers
    void UpdateTicks();
    /// Code emitter: Performs a block lookup based on current state
    /// @note this clobbers ABI caller-save registers
    void LookupBlock();

    /// Code emitter: Load required flags for conditional cond from rax into host rflags
    void LoadRequiredFlagsForCondFromRax(IR::Cond cond);

    /// @brief Code emitter: Calls the function
    template<typename F>
        requires std::is_pointer_v<F> && std::is_function_v<std::remove_pointer_t<F>>
    void CallFunction(F fn) {
        ::Common::X64::CallFarFunction(*this, fn);
    }

    /// @brief Code emitter: Calls the lambda. Lambda must not have any captures.
    template<typename Lambda>
    void CallLambda(Lambda l) {
        ::Common::X64::CallFarFunction(*this, Common::FptrCast(l));
    }

    void ZeroExtendFrom(size_t bitsize, Xbyak::Reg64 reg) {
        switch (bitsize) {
        case 8:
            movzx(reg.cvt32(), reg.cvt8());
            return;
        case 16:
            movzx(reg.cvt32(), reg.cvt16());
            return;
        case 32:
            mov(reg.cvt32(), reg.cvt32());
            return;
        case 64:
            return;
        default:
            UNREACHABLE();
        }
    }

    Xbyak::Address Const(const Xbyak::AddressFrame& frame, u64 lower, u64 upper = 0);

    template<size_t esize>
    Xbyak::Address BConst(const Xbyak::AddressFrame& frame, u64 value) {
        return Const(frame, mcl::bit::replicate_element<u64>(esize, value),
                     mcl::bit::replicate_element<u64>(esize, value));
    }

    CodePtr GetCodeBegin() const;
    size_t GetTotalCodeSize() const;

    const void* GetReturnFromRunCodeAddress() const {
        return return_from_run_code[0];
    }

    const void* GetForceReturnFromRunCodeAddress() const {
        return return_from_run_code[FORCE_RETURN];
    }

    void int3() { db(0xCC); }

    /// Allocate memory of `size` bytes from the same block of memory the code is in.
    /// This is useful for objects that need to be placed close to or within code.
    /// The lifetime of this memory is the same as the code around it.
    void* AllocateFromCodeSpace(size_t size);
    /// Where to write `p`, a pointer into this block of code: itself, or its alias in the writable
    /// view when the code memory is dual-mapped (its executable view is read-only).
    template<typename T>
    T* WriteAlias(T* p) const noexcept {
        return reinterpret_cast<T*>(reinterpret_cast<u8*>(p) + writeOffset_);
    }

    /// Stores `value` into the naturally aligned field at `where`, in emitted code, with one atomic
    /// store: a core running that code sees the old or the new value, never a mix. Under paged
    /// W^X it must run between EnableWriting and DisableWriting.
    template<typename T>
        requires(std::is_integral_v<T> && (sizeof(T) == 4 || sizeof(T) == 8))
    void WriteCodeAtomic(CodePtr where, T value) {
        ASSERT(reinterpret_cast<std::uintptr_t>(where) % sizeof(T) == 0);
        MakeWritable(where, sizeof(T));
        T* const field = WriteAlias(static_cast<T*>(const_cast<void*>(where)));
        std::atomic_ref<T>{*field}.store(value, std::memory_order_release);
    }

    /// True when emitted code is written without changing page protection (dual-mapped memory,
    /// or no W^X at all): code that other threads are running then never turns non-executable.
    bool WritesWithoutProtectionChanges() const noexcept;

    /// Multiplies the global code cache cap (SetCodeCacheCap) for this cache: one cache that
    /// serves several cores may hold the sum of what each of them would.
    void SetCapScale(size_t scale) noexcept;

    /// This cache's emission state and the memory where its next byte goes, for a crash report.
    /// Allocates nothing and takes no lock it could wait on: a faulting thread may hold any.
    int DescribeState(char* out, size_t size) const noexcept;

    /// Where the next byte emitted is written (the write view of a dual-mapped cache).
    const u8* EmitWritePointer() const noexcept {
        return top_ + size_ + writeOffset_;
    }

    /// Marks the calling thread as writing code into this cache for its lifetime: a fault it takes
    /// meanwhile is retried after a wait (see GetCodeEmitFaults). Nests.
    class EmitScope {
    public:
        explicit EmitScope(const BlockOfCode& code) noexcept;
        ~EmitScope();
        EmitScope(const EmitScope&) = delete;
        EmitScope& operator=(const EmitScope&) = delete;

    private:
        const BlockOfCode* previous;
    };

    void SetCodePtr(CodePtr code_ptr);
    void EnsurePatchLocationSize(CodePtr begin, size_t size);

    /// Marks writes into blocks already emitted (link patches), for the emit fault context.
    void SetPatching(bool on) noexcept {
        patching_ = on;
    }
    bool Patching() const noexcept {
        return patching_;
    }

    static const Xbyak::Reg64 ABI_JIT_PTR;
    // ABI registers
#ifdef _WIN32
    static const Xbyak::Reg64 ABI_RETURN;
    static const Xbyak::Reg64 ABI_PARAM1;
    static const Xbyak::Reg64 ABI_PARAM2;
    static const Xbyak::Reg64 ABI_PARAM3;
    static const Xbyak::Reg64 ABI_PARAM4;
    static const std::array<Xbyak::Reg64, ABI_PARAM_COUNT> ABI_PARAMS;
#else
    static const Xbyak::Reg64 ABI_RETURN;
    static const Xbyak::Reg64 ABI_RETURN2;
    static const Xbyak::Reg64 ABI_PARAM1;
    static const Xbyak::Reg64 ABI_PARAM2;
    static const Xbyak::Reg64 ABI_PARAM3;
    static const Xbyak::Reg64 ABI_PARAM4;
    static const Xbyak::Reg64 ABI_PARAM5;
    static const Xbyak::Reg64 ABI_PARAM6;
    static const std::array<Xbyak::Reg64, ABI_PARAM_COUNT> ABI_PARAMS;
#endif

    JitStateInfo GetJitStateInfo() const { return jsi; }

    bool HasHostFeature(HostFeature feature) const noexcept;

private:
    using RunCodeFuncType = HaltReason (*)(void*, CodePtr);
    bool patching_ = false;
    /// EnsureMemoryCommitted's commit, before the dual-mapped check.
    void CommitCodeMemory(size_t codesize);
    static constexpr size_t MXCSR_ALREADY_EXITED = 1 << 0;
    static constexpr size_t FORCE_RETURN = 1 << 1;

    // The code region's memory state comes before constant_pool: its constructor already commits
    // and writes code memory (EnsureMemoryCommitted), so this state must be initialized by then.
    // Declared after it, committed_size was read uninitialized there; with a large garbage value
    // nothing was committed and the pool's first write faulted.
#ifdef _WIN32
    size_t committed_size = 0;
#endif
#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
    // Paged W^X (see EnableWriting). Pages in [wx_exec_begin, wx_exec_end) hold emitted code and
    // are RX unless being written; committed pages past wx_exec_end are always RW. The constant
    // pool sits below wx_exec_begin and stays RW: it is only ever read as data.
    const u8* wx_exec_begin = nullptr;
    const u8* wx_exec_end = nullptr;
    const u8* wx_write_begin = nullptr;
    std::vector<const u8*> wx_extra_pages;
    bool wx_writing = false;
    // Dual-mapped code memory (see the allocator): writes go through writeOffset_ and no page
    // ever changes protection. Set before constant_pool, which writes code memory first.
    bool AdoptWriteView();
    const bool dual_mapped = AdoptWriteView();
    void ReleaseMemoryPastCap();
    bool release_failed = false;
    /// Dual-mapped: the code below this offset was checked to be mapped in both views
    /// (EnsureMemoryCommitted). Reset when chunks may be unmapped.
    size_t mapped_verified_end = 0;
#endif
    bool prelude_complete = false;
    // SetCodeCacheCap as this cache applies it: the cap it last saw, and where its code ends.
    size_t CapEnd(size_t cap) const;
    mutable size_t cap_epoch = 0;
    mutable size_t cap_end = SIZE_MAX;
    size_t cap_scale = 1;
    /// s_code_cache_cap times cap_scale, saturating.
    size_t ScaledCap() const noexcept;

    ConstantPool constant_pool;
    JitStateInfo jsi;
    std::array<const void*, 4> return_from_run_code;
    RunCodeFuncType run_code = nullptr;
    RunCodeFuncType step_code = nullptr;
    RunCodeCallbacks cb;
    CodePtr code_begin = nullptr;

    void GenRunCode(std::function<void(BlockOfCode&)> rcp);
};

}  // namespace Dynarmic::Backend::X64
