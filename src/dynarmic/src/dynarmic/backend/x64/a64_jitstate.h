// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"

#include "dynarmic/backend/x64/nzcv_util.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"

namespace Dynarmic::Backend::X64 {

class BlockOfCode;

#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4324)  // Structure was padded due to alignment specifier
#endif

struct A64JitState {
    using ProgramCounterType = u64;

    A64JitState() { ResetRSB(); }

    // Exclusive state stuff
    static constexpr u64 RESERVATION_GRANULE_MASK = 0xFFFF'FFFF'FFFF'FFF0ull;
    // Return stack buffer
    static constexpr size_t RSB_SIZE = 8;  // MUST be a power of 2.
    static constexpr size_t RSB_PTR_MASK = RSB_SIZE - 1;

    std::array<u64, 31> reg{};
    u64 sp = 0;
    u64 pc = 0;
    u32 cpsr_nzcv = 0;
    u32 fpsr_exc = 0;
    u32 fpsr_qc = 0;
    u32 fpcr = 0;

    alignas(16) std::array<u64, 64> vec{};  // Extension registers.

    // For internal use (See: BlockOfCode::RunCode)
    u32 guest_MXCSR = 0x00001f80;
    u32 asimd_MXCSR = 0x00009fc0;
    volatile u32 halt_reason = 0;

    // Exclusive state
    u8 exclusive_state = 0;
    u32 rsb_ptr = 0;
    std::array<u64, RSB_SIZE> rsb_location_descriptors;
    std::array<u64, RSB_SIZE> rsb_codeptrs;

    /// Per-core values that emitted code reads through ABI_JIT_PTR instead of embedding them,
    /// so one block of host code runs on any core of the process. Reset() keeps them.
    struct CoreBindings {
        u64 callbacks_arg = 0;        ///< `this` passed to UserCallbacks member functions
        u64 lookup_block_arg = 0;     ///< Argument of the dispatcher's block lookup
        u64 user_config = 0;          ///< const A64::UserConfig* of this core
        u64 exclusive_address = 0;    ///< VAddr* of this core's slot in the global monitor
        u64 exclusive_value = 0;      ///< Vector* of this core's slot in the global monitor
        u64 tpidr_el0 = 0;            ///< u64*
        u64 tpidrro_el0 = 0;          ///< const u64*
        u64 fast_dispatch_table = 0;  ///< FastDispatchTable::Entry* of this core
    };
    CoreBindings bindings;

    u32 GetPstate() const {
        return NZCV::FromX64(cpsr_nzcv);
    }

    void SetPstate(u32 new_pstate) {
        cpsr_nzcv = NZCV::ToX64(new_pstate);
    }

    void ResetRSB() {
        rsb_location_descriptors.fill(0xFFFFFFFFFFFFFFFFull);
        rsb_codeptrs.fill(0);
    }

    u32 GetFpcr() const;
    u32 GetFpsr() const;
    void SetFpcr(u32 value);
    void SetFpsr(u32 value);

    u64 GetUniqueHash() const noexcept {
        const u64 fpcr_u64 = u64(fpcr & A64::LocationDescriptor::fpcr_mask) << A64::LocationDescriptor::fpcr_shift;
        const u64 pc_u64 = pc & A64::LocationDescriptor::pc_mask;
        return pc_u64 | fpcr_u64;
    }
};

#ifdef _MSC_VER
#    pragma warning(pop)
#endif

using CodePtr = const void*;

/// Offsets in A64JitState of the core bindings, as emitted code addresses them off ABI_JIT_PTR.
namespace A64BindingOffset {
#define DYNARMIC_A64_BINDING_OFFSET(name) \
    inline constexpr u32 name = static_cast<u32>(offsetof(A64JitState, bindings) + offsetof(A64JitState::CoreBindings, name))
DYNARMIC_A64_BINDING_OFFSET(callbacks_arg);
DYNARMIC_A64_BINDING_OFFSET(lookup_block_arg);
DYNARMIC_A64_BINDING_OFFSET(user_config);
DYNARMIC_A64_BINDING_OFFSET(exclusive_address);
DYNARMIC_A64_BINDING_OFFSET(exclusive_value);
DYNARMIC_A64_BINDING_OFFSET(tpidr_el0);
DYNARMIC_A64_BINDING_OFFSET(tpidrro_el0);
DYNARMIC_A64_BINDING_OFFSET(fast_dispatch_table);
#undef DYNARMIC_A64_BINDING_OFFSET
}  // namespace A64BindingOffset

}  // namespace Dynarmic::Backend::X64
