// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include "common/common_types.h"

/// What each thread is doing right now in work that can take long (a pipeline's translation, the
/// driver's PSO creation, a wait for one), so a freeze report can say who is stuck in what and for
/// how long. A Scope costs a clock read and a few relaxed stores; nothing locks or allocates
/// after a thread's first Scope.
namespace Common::StallWatch {

enum class Work : u8 {
    None,
    PipelineDxil,   ///< a = VS hash, b = PS hash: SPIR-V -> signed DXIL of a graphics pipeline
    MesaTranslate,  ///< Mesa's translation, inside PipelineDxil or ComputeDxil
    ValidatorLock,  ///< waiting for the shared DXIL validator
    ValidatorSign,  ///< the validator signing one stage
    DriverPso,      ///< a = VS hash, b = PS hash: the driver's CreateGraphicsPipelineState
    ComputeDxil,    ///< a = shader hash
    ComputePso,     ///< a = shader hash: the driver's CreateComputePipelineState
    PipelineWait,   ///< a = VS hash, b = PS hash: the GPU thread blocked on a pipeline
    ComputeWait,    ///< a = shader hash: the command recorder blocked on a compute PSO
};

[[nodiscard]] const char* WorkName(Work work) noexcept;

/// Marks the calling thread as doing `work` until the end of the scope; the work it was doing
/// before (an outer scope) is restored then. Without hashes, it keeps the outer scope's.
class Scope {
public:
    explicit Scope(Work work, u64 a = 0, u64 b = 0) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    int slot;
    Work previous_work{};
    u64 previous_a{};
    u64 previous_b{};
    u64 previous_start_ms{};
};

/// One line per thread busy for at least `min_ms`, oldest first; empty when none. Only the
/// watching thread reads the slots, so a half-written slot can at worst show the wrong hash once.
[[nodiscard]] std::string Describe(u64 min_ms);

/// When the oldest work in progress started (NowMs), 0 when every thread is idle.
[[nodiscard]] u64 OldestStartMs();

/// Steady clock in ms, as the slots use it.
[[nodiscard]] u64 NowMs() noexcept;

} // namespace Common::StallWatch
