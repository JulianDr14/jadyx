// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <tuple>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"
#include <boost/container/static_vector.hpp>

#include "dynarmic/backend/block_range_information.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/emit_x64.h"
#include "dynarmic/backend/x64/reg_alloc.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::Backend::X64 {

struct A64EmitContext final : public EmitContext {
    A64EmitContext(const A64::UserConfig& conf, RegAlloc& reg_alloc, IR::Block& block, boost::container::stable_vector<Xbyak::Label>& shared_labels);

    A64::LocationDescriptor Location() const;
    bool IsSingleStep() const;
    FP::FPCR FPCR(bool fpcr_controlled = true) const override;

    bool HasOptimization(OptimizationFlag flag) const override {
        return conf.HasOptimization(flag);
    }

    const A64::UserConfig& conf;
};

/// What an A64 emitter reports to the code cache that owns it.
struct A64EmitX64Hooks {
    /// A block was unlinked (invalidated): indirect-branch caches must drop it.
    std::function<void(IR::LocationDescriptor)> block_invalidated;
    /// Handles a fastmem fault; empty runs the emitter's own FastmemCallback.
    std::function<FakeCall(u64 rip)> fastmem_fault;
};

/// Emits A64 blocks that any core of a process can run: per-core values (callback objects, the
/// user config, exclusive monitor slots, TPIDR pointers, the fast-dispatch table) are read from
/// A64JitState::bindings at run time, and block links are retargeted with single atomic stores.
class A64EmitX64 final : public EmitX64 {
public:
    A64EmitX64(BlockOfCode& code, A64::UserConfig conf, A64EmitX64Hooks hooks = {});
    ~A64EmitX64() override;

    /// Emit host machine code for a basic block with intermediate representation `block`.
    /// @note block is modified.
    BlockDescriptor Emit(IR::Block& block) noexcept;

    void ClearCache() override;

    void InvalidateCacheRanges(const boost::icl::interval_set<u64>& ranges);

//protected:
    void GenMemory128Accessors();
    void GenFastmemFallbacks();
    void GenPageTableThunks();
    void GenTerminalHandlers();
    /// Shared page table access for an unordered load or store, nullptr to emit it inline.
    const void* PageTableThunk(bool ordered, bool write, size_t bitsize, int vaddr_idx, int value_idx) const;

    // Microinstruction emitters
    void EmitPushRSB(EmitContext& ctx, IR::Inst* inst);
#define OPCODE(...)
#define A32OPC(...)
#define A64OPC(name, type, ...) void EmitA64##name(A64EmitContext& ctx, IR::Inst* inst);
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC

    // Helpers
    std::string LocationDescriptorToFriendlyName(const IR::LocationDescriptor&) const override;

    // Fastmem information
    using DoNotFastmemMarker = std::tuple<IR::LocationDescriptor, unsigned>;
    struct FastmemPatchInfo {
        u64 resume_rip;
        u64 callback;
        DoNotFastmemMarker marker;
        bool recompile;
    };
    std::optional<DoNotFastmemMarker> ShouldFastmem(A64EmitContext& ctx, IR::Inst* inst) const;
    FakeCall FastmemCallback(u64 rip);

    // Per-core values the shared memory emitters need, read from the core's JIT state.
    template<auto mfp>
    ArgCallback CoreCallback() const {
        return DevirtualizeFromJitState<mfp>(conf.callbacks, A64BindingOffset::callbacks_arg);
    }
    void EmitLoadUserConfig(Xbyak::Reg64 reg);
    void EmitLoadExclusiveAddressPointer(Xbyak::Reg64 reg);
    void EmitLoadExclusiveValuePointer(Xbyak::Reg64 reg);
    /// A64 code may run on any core: an exclusive store clears every slot holding the address,
    /// its own included (as ExclusiveMonitor::CheckAndClear does).
    std::optional<size_t> ExclusiveClearSkipsProcessor() const {
        return std::nullopt;
    }

    // Memory access helpers
    void EmitCheckMemoryAbort(A64EmitContext& ctx, IR::Inst* inst, Xbyak::Label* end = nullptr);
    template<std::size_t bitsize, auto callback>
    void EmitMemoryRead(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitMemoryWrite(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveReadMemory(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveWriteMemory(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveReadMemoryInline(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveWriteMemoryInline(A64EmitContext& ctx, IR::Inst* inst);

    // Terminal instruction emitters
    bool EmitLeafTerminal(IR::Term::LeafTerminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) noexcept override;
    bool EmitTerminal(IR::Term::Terminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) noexcept override;

    // Patching. Every site keeps its target in one aligned field (see link_site.h), so linking and
    // unlinking is an atomic store and never re-emits code another core may be running.
    // EmitPatchJg/Jz/Jmp emit the whole site, registered, with its unlinked path; EmitPatchMovRcx
    // emits only the load (PushRSBHelper registers it).
    void Unpatch(const IR::LocationDescriptor& target_desc) override;
    void ApplyPatch(PatchSite site, const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) override;
    void EmitPatchJg(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) override;
    void EmitPatchJz(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) override;
    void EmitPatchJmp(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) override;
    void EmitPatchMovRcx(CodePtr target_code_ptr = nullptr) override;
    void EmitConditionalLink(PatchKind kind, const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr);
    /// Code that stores the guest PC of `target_desc` and leaves through `exit`.
    void EmitStorePcAndExit(const IR::LocationDescriptor& target_desc, bool force_return);

//data
    const A64::UserConfig conf;
    A64EmitX64Hooks hooks;
    RegAlloc reg_alloc; //reusable reg alloc
    BlockRangeInformation<u64> block_ranges;
    ::Common::unordered_map<u64, FastmemPatchInfo> fastmem_patch_info;
    ::Common::unordered_map<std::tuple<bool, size_t, int, int>, void (*)()> read_fallbacks;
    ::Common::unordered_map<std::tuple<bool, size_t, int, int>, void (*)()> write_fallbacks;
    ::Common::unordered_map<std::tuple<bool, size_t, int, int>, void (*)()> exclusive_write_fallbacks;
    // [write][log2(bitsize / 8)][vaddr register][value register]
    std::array<const void*, 2 * 5 * 16 * 16> page_table_thunks{};
    ::Common::unordered_set<DoNotFastmemMarker> do_not_fastmem;
    boost::container::stable_vector<Xbyak::Label> shared_labels;
    const void* terminal_handler_pop_rsb_hint = nullptr;
    const void* terminal_handler_fast_dispatch_hint = nullptr;
    void (*memory_read_128)() = nullptr;
    void (*memory_write_128)() = nullptr;
    void (*memory_exclusive_write_128)() = nullptr;
};

}  // namespace Dynarmic::Backend::X64
