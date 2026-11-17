// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <cstring>
#include <memory>
#include <mutex>

#include <boost/icl/interval_set.hpp>
#include "common/assert.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/llvm_disassemble.h"

#include "dynarmic/backend/x64/a64_code_cache.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/exclusive_monitor_friend.h"
#include "dynarmic/backend/x64/fast_dispatch_table.h"
#include "dynarmic/common/atomic.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/jit_profile.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opt_passes.h"

namespace Dynarmic::A64 {

using namespace Backend::X64;

struct Jit::Impl final {
    friend class Jit;
public:
    explicit Impl(UserConfig conf)
        : conf(conf)
        , cache(A64CodeCache::Acquire(conf, &GetCurrentBlockThunk))
        , fast_dispatch(MakeFastDispatchTable(conf, cache->Code()))
    {
        BindCore();
        core = cache->Attach(jit_state, fast_dispatch.get());
    }

    ~Impl() = default;

    HaltReason Run() {
        ASSERT(!is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        is_executing = true;
        HaltReason hr;
        {
            const auto execution = cache->Execute(*core);
            // TODO: Check code alignment
            const CodePtr current_code_ptr = [this] {
                // RSB optimization
                const u32 new_rsb_ptr = (jit_state.rsb_ptr - 1) & A64JitState::RSB_PTR_MASK;
                if (jit_state.GetUniqueHash() == jit_state.rsb_location_descriptors[new_rsb_ptr]) {
                    jit_state.rsb_ptr = new_rsb_ptr;
                    return CodePtr(jit_state.rsb_codeptrs[new_rsb_ptr]);
                }
                return CodePtr((uintptr_t(GetCurrentBlock()) + 15) & ~uintptr_t(15));
            }();
            hr = cache->Code().RunCode(&jit_state, current_code_ptr);
        }
        PerformRequestedCacheInvalidation(hr);
        is_executing = false;
        return hr;
    }

    HaltReason Step() {
        ASSERT(!is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        is_executing = true;
        HaltReason hr;
        {
            const auto execution = cache->Execute(*core);
            hr = cache->Code().StepCode(&jit_state, GetCurrentSingleStep());
        }
        PerformRequestedCacheInvalidation(hr);
        is_executing = false;
        return hr;
    }

    void ClearCache() {
        std::unique_lock lock{invalidation_mutex};
        // The latest request counts: a clear after it satisfies every earlier one too.
        clear_generation = cache->Generation();
        invalidate_entire_cache = true;
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void InvalidateCacheRange(u64 start_address, size_t length) {
        std::unique_lock lock{invalidation_mutex};
        const auto end_address = static_cast<u64>(start_address + length - 1);
        const auto range = boost::icl::discrete_interval<u64>::closed(start_address, end_address);
        invalid_cache_ranges.add(range);
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void Reset() {
        ASSERT(!is_executing);
        // The bindings describe this core, not its guest state.
        const auto bindings = jit_state.bindings;
        jit_state = {};
        jit_state.bindings = bindings;
    }

    void HaltExecution(HaltReason hr) {
        Atomic::Or(&jit_state.halt_reason, static_cast<u32>(hr));
    }

    void ClearHalt(HaltReason hr) {
        Atomic::And(&jit_state.halt_reason, ~static_cast<u32>(hr));
    }

    u64 GetSP() const {
        return jit_state.sp;
    }

    void SetSP(u64 value) {
        jit_state.sp = value;
    }

    u64 GetPC() const {
        return jit_state.pc;
    }

    void SetPC(u64 value) {
        jit_state.pc = value;
    }

    u64 GetRegister(size_t index) const {
        if (index == 31)
            return GetSP();
        return jit_state.reg.at(index);
    }

    void SetRegister(size_t index, u64 value) {
        if (index == 31)
            return SetSP(value);
        jit_state.reg.at(index) = value;
    }

    std::array<u64, 31> GetRegisters() const {
        return jit_state.reg;
    }

    void SetRegisters(const std::array<u64, 31>& value) {
        jit_state.reg = value;
    }

    Vector GetVector(size_t index) const {
        return {jit_state.vec.at(index * 2), jit_state.vec.at(index * 2 + 1)};
    }

    void SetVector(size_t index, Vector value) {
        jit_state.vec.at(index * 2) = value[0];
        jit_state.vec.at(index * 2 + 1) = value[1];
    }

    std::array<Vector, 32> GetVectors() const {
        std::array<Vector, 32> ret;
        static_assert(sizeof(ret) == sizeof(jit_state.vec));
        std::memcpy(ret.data(), jit_state.vec.data(), sizeof(jit_state.vec));
        return ret;
    }

    void SetVectors(const std::array<Vector, 32>& value) {
        static_assert(sizeof(value) == sizeof(jit_state.vec));
        std::memcpy(jit_state.vec.data(), value.data(), sizeof(jit_state.vec));
    }

    u32 GetFpcr() const {
        return jit_state.GetFpcr();
    }

    void SetFpcr(u32 value) {
        jit_state.SetFpcr(value);
    }

    u32 GetFpsr() const {
        return jit_state.GetFpsr();
    }

    void SetFpsr(u32 value) {
        jit_state.SetFpsr(value);
    }

    u32 GetPstate() const {
        return jit_state.GetPstate();
    }

    void SetPstate(u32 value) {
        jit_state.SetPstate(value);
    }

    void ClearExclusiveState() {
        jit_state.exclusive_state = 0;
    }

    bool IsExecuting() const {
        return is_executing;
    }

    std::string Disassemble() const {
        const BlockOfCode& code = cache->Code();
        const size_t size = reinterpret_cast<const char*>(code.getCurr()) - reinterpret_cast<const char*>(code.GetCodeBegin());
        auto const* p = reinterpret_cast<const char*>(code.GetCodeBegin());
        return Common::DisassembleX64(p, p + size);
    }

private:
    static std::unique_ptr<FastDispatchTable> MakeFastDispatchTable(const UserConfig& conf, const BlockOfCode& code) {
        if (!conf.HasOptimization(OptimizationFlag::FastDispatch)) {
            return nullptr;
        }
        // Hashed as the emitted dispatcher hashes it.
        return std::make_unique<FastDispatchTable>(code.HasHostFeature(HostFeature::SSE42));
    }

    /// What the (possibly shared) code reads to act as this core.
    void BindCore() {
        auto& bindings = jit_state.bindings;
        bindings.callbacks_arg = reinterpret_cast<u64>(conf.callbacks);
        bindings.lookup_block_arg = reinterpret_cast<u64>(this);
        bindings.user_config = reinterpret_cast<u64>(&conf);
        if (conf.global_monitor) {
            bindings.exclusive_address = reinterpret_cast<u64>(GetExclusiveMonitorAddressPointer(conf.global_monitor, conf.processor_id));
            bindings.exclusive_value = reinterpret_cast<u64>(GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id));
        }
        bindings.tpidr_el0 = reinterpret_cast<u64>(conf.tpidr_el0);
        bindings.tpidrro_el0 = reinterpret_cast<u64>(conf.tpidrro_el0);
        bindings.fast_dispatch_table = fast_dispatch ? reinterpret_cast<u64>(fast_dispatch->Data()) : 0;
    }

    static CodePtr GetCurrentBlockThunk(void* thisptr) {
        Jit::Impl* this_ = static_cast<Jit::Impl*>(thisptr);
        return this_->GetCurrentBlock();
    }

    IR::LocationDescriptor GetCurrentLocation() const {
        return IR::LocationDescriptor{jit_state.GetUniqueHash()};
    }

    CodePtr GetCurrentBlock() {
        return GetBlock(GetCurrentLocation());
    }

    CodePtr GetCurrentSingleStep() {
        return GetBlock(A64::LocationDescriptor{GetCurrentLocation()}.SetSingleStepping(true));
    }

    /// Code for `descriptor`, compiled if needed. With `expected` (a prewarm) the block is compiled
    /// only from guest code matching the profile and never at the cost of clearing the cache; an
    /// already compiled block counts as warm.
    CodePtr GetBlock(IR::LocationDescriptor descriptor, const BlockProfile* expected = nullptr) {
        cache->Sync(*core);
        if (const CodePtr existing = cache->Find(descriptor)) {
            return existing;
        }

        const JitProfile::Timer compile_timer{JitProfile::Phase::Compile};
        for (;;) {
            const auto ticket = cache->BeginCompile(*core, expected == nullptr);
            if (!ticket) {
                return nullptr;
            }
            BlockProfile profile{descriptor.Value()};
            bool valid_profile = true;
            A64CodeCache::EmitResult result;
            {
                // Held only to translate and emit; BeginCompile above is what may wait for a clear.
                const auto claim = cache->ClaimCompile(descriptor);
                if (cache->IsShared()) {
                    // Another core may have emitted it since the lookup above, or while we waited.
                    if (const CodePtr compiled = cache->Find(descriptor)) {
                        return compiled;
                    }
                }
                if (!claim.Owned()) {
                    continue;  // The core we waited for did not emit it: compile it ourselves.
                }
                if (!Translate(descriptor, expected, profile, valid_profile)) {
                    return nullptr;
                }
                {
                    const JitProfile::Timer timer{JitProfile::Phase::Optimize};
                    Optimization::Optimize(ir_block, conf, cache->GetPolyfillOptions());
                }
                result = cache->Emit(ir_block, *ticket);
            }
            if (!result.entrypoint) {
                continue;  // Guest code invalidated while translating, or the cache filled up.
            }
            if (result.compiled && block_profile_callback && valid_profile && profile.code_bytes != 0 &&
                !A64::LocationDescriptor{descriptor}.SingleStepping()) {
                block_profile_callback(profile);
            }
            return result.entrypoint;
        }
    }

    /// Translates `descriptor` into ir_block, recording the guest words in `profile` when they are
    /// wanted. False when a prewarm profile does not match the guest code now in memory.
    bool Translate(IR::LocationDescriptor descriptor, const BlockProfile* expected, BlockProfile& profile, bool& valid_profile) {
        const bool capture = expected || bool(block_profile_callback);
        const auto get_code = [&](u64 vaddr) -> std::optional<u32> {
            if (expected && profile.code_bytes >= expected->code_bytes) {
                valid_profile = false;
                return std::nullopt;
            }
            const auto word = conf.callbacks->MemoryReadCode(vaddr);
            if (capture) {
                if (!word || profile.code_bytes >= BlockProfile::MaxCodeBytes) {
                    valid_profile = false;
                } else {
                    profile.AddWord(*word);
                }
            }
            return word;
        };
        // LocationDescriptor ctor() does important ops (like tflags) do not skip
        auto const arch_descriptor = A64::LocationDescriptor{descriptor};
        // A profile can outlive its code (a rebuilt homebrew keeps its title and build ids), and
        // translating what is there now can stop at an instruction the translator does not handle.
        // A64 blocks read their words in order, so the hash is checked before translating.
        if (expected) {
            BlockProfile current{descriptor.Value()};
            for (u64 vaddr = arch_descriptor.PC(); current.code_bytes < expected->code_bytes; vaddr += 4) {
                const auto word = conf.callbacks->MemoryReadCode(vaddr);
                if (!word) {
                    return false;
                }
                current.AddWord(*word);
            }
            if (current.code_hash != expected->code_hash) {
                return false;
            }
        }
        ir_block.Reset(arch_descriptor);
        {
            const JitProfile::Timer timer{JitProfile::Phase::Translate};
            A64::Translate(ir_block, arch_descriptor, get_code, {conf.define_unpredictable_behaviour, conf.wall_clock_cntpct});
        }
        return !expected || (valid_profile && profile.code_bytes == expected->code_bytes &&
                             profile.code_hash == expected->code_hash);
    }

    void PerformRequestedCacheInvalidation(HaltReason hr) {
        if (Has(hr, HaltReason::CacheInvalidation)) {
            std::unique_lock lock{invalidation_mutex};

            ClearHalt(HaltReason::CacheInvalidation);

            if (!invalidate_entire_cache && invalid_cache_ranges.empty()) {
                return;
            }

            const JitProfile::Timer invalidate_timer{JitProfile::Phase::Invalidate};
            jit_state.ResetRSB();
            // A clear skipped because one ran since the request does not cover ranges asked after it.
            const bool cleared = invalidate_entire_cache && cache->Clear(*core, clear_generation);
            if (!cleared && !invalid_cache_ranges.empty()) {
                cache->InvalidateRanges(*core, invalid_cache_ranges);
            }
            invalid_cache_ranges.clear();
            invalidate_entire_cache = false;
        }
    }

    IR::Block ir_block = {LocationDescriptor(0, FP::FPCR(0), false)};
    const UserConfig conf;
    A64JitState jit_state;
    std::shared_ptr<A64CodeCache> cache;
    std::unique_ptr<FastDispatchTable> fast_dispatch;
    std::unique_ptr<A64CodeCache::Core> core;  ///< Declared last: detaches before the rest goes.
    bool is_executing = false;
    std::function<void(const BlockProfile&)> block_profile_callback;
    bool invalidate_entire_cache = false;
    std::uint64_t clear_generation = 0;
    boost::icl::interval_set<u64> invalid_cache_ranges;
    std::mutex invalidation_mutex;
};

Jit::Jit(UserConfig conf)
        : impl(std::make_unique<Jit::Impl>(conf)) {}

Jit::~Jit() = default;

void Jit::SetBlockProfileCallback(std::function<void(const BlockProfile&)> callback) {
    ASSERT(!impl->is_executing);
    impl->block_profile_callback = std::move(callback);
}

bool Jit::PrecompileBlock(const BlockProfile& block) {
    ASSERT(!impl->is_executing);
    const LocationDescriptor descriptor{IR::LocationDescriptor{block.descriptor}};
    // Do not evacuate already warmed code if a profile exceeds the cache budget.
    if (descriptor.SingleStepping() || (descriptor.PC() & 3) ||
        descriptor.UniqueHash() != block.descriptor || block.code_bytes == 0 ||
        block.code_bytes > BlockProfile::MaxCodeBytes || (block.code_bytes & 3) ||
        impl->cache->SpaceRemaining() < A64CodeCache::minimum_free_space) {
        return false;
    }
    // Apply what was requested before the warm (code loaded or reprotected at boot) now, as Run
    // would: left pending, the first Run drops the blocks warmed in those ranges.
    impl->PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&impl->jit_state.halt_reason)));
    return impl->GetBlock(descriptor, &block) != nullptr;
}

std::size_t Jit::GetCodeCacheSpaceRemaining() const {
    ASSERT(!impl->is_executing);
    return impl->cache->SpaceRemaining();
}

HaltReason Jit::Run() {
    return impl->Run();
}

HaltReason Jit::Step() {
    return impl->Step();
}

void Jit::ClearCache() {
    impl->ClearCache();
}

void Jit::InvalidateCacheRange(u64 start_address, size_t length) {
    impl->InvalidateCacheRange(start_address, length);
}

void Jit::Reset() {
    impl->Reset();
}

void Jit::HaltExecution(HaltReason hr) {
    impl->HaltExecution(hr);
}

void Jit::ClearHalt(HaltReason hr) {
    impl->ClearHalt(hr);
}

u64 Jit::GetSP() const {
    return impl->GetSP();
}

void Jit::SetSP(u64 value) {
    impl->SetSP(value);
}

u64 Jit::GetPC() const {
    return impl->GetPC();
}

void Jit::SetPC(u64 value) {
    impl->SetPC(value);
}

u64 Jit::GetRegister(size_t index) const {
    return impl->GetRegister(index);
}

void Jit::SetRegister(size_t index, u64 value) {
    impl->SetRegister(index, value);
}

std::array<u64, 31> Jit::GetRegisters() const {
    return impl->GetRegisters();
}

void Jit::SetRegisters(const std::array<u64, 31>& value) {
    impl->SetRegisters(value);
}

Vector Jit::GetVector(size_t index) const {
    return impl->GetVector(index);
}

void Jit::SetVector(size_t index, Vector value) {
    impl->SetVector(index, value);
}

std::array<Vector, 32> Jit::GetVectors() const {
    return impl->GetVectors();
}

void Jit::SetVectors(const std::array<Vector, 32>& value) {
    impl->SetVectors(value);
}

u32 Jit::GetFpcr() const {
    return impl->GetFpcr();
}

void Jit::SetFpcr(u32 value) {
    impl->SetFpcr(value);
}

u32 Jit::GetFpsr() const {
    return impl->GetFpsr();
}

void Jit::SetFpsr(u32 value) {
    impl->SetFpsr(value);
}

u32 Jit::GetPstate() const {
    return impl->GetPstate();
}

void Jit::SetPstate(u32 value) {
    impl->SetPstate(value);
}

void Jit::ClearExclusiveState() {
    impl->ClearExclusiveState();
}

bool Jit::IsExecuting() const {
    return impl->IsExecuting();
}

std::string Jit::Disassemble() const {
    return impl->Disassemble();
}

}  // namespace Dynarmic::A64
