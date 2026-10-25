// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Optimization {
/// What WorkgroupSplitPass did with a compute program.
enum class WorkgroupSplitVerdict : std::uint8_t {
    Split,
    NotCompute,
    BadFactor,      ///< The factor is not a power of two of at least 2.
    NotLinear,      ///< Local size has a Y or Z extent.
    SharedMemory,
    Barrier,
    NoStrideLoop,   ///< No loop starts at the local id and steps by the workgroup width.
    BadStrideLoop,  ///< A phi fed by the local id is not such a loop, or the loop has no bound.
    NoGuard,        ///< A stride loop runs before any check of its index against its bound.
    OtherLocalUse,  ///< The local id feeds something other than its loops and their bound checks.
    PackedId,       ///< A whole id vector is used, not its components.
    UnconfinedWrite, ///< A write runs outside the stride loops and any empty-group leader check.
};

[[nodiscard]] std::string_view Name(WorkgroupSplitVerdict verdict) noexcept;

/// Dispatch amplification for persistent compute shaders: a workgroup of width W walking its
/// items in a grid-stride loop (i = local id; ...; i += W) becomes `factor` workgroups that act
/// as one virtual workgroup of width W * factor. Every real group reads
///   workgroup id x = real id x / factor,  local id x = real local x + (real id x % factor) * W
/// and the loop steps by W * factor, so the items of each original group spread over `factor`
/// times as many waves; the caller dispatches `factor` times as many groups along X and keeps
/// the guest-visible group count unchanged.
///
/// Only programs whose result cannot depend on the group width qualify: X-only local size, no
/// shared memory or workgroup barriers, and every use of local id X being a stride loop's start
/// or a bound check of it (against the loop's bound). A leader check (local id == 0) is refused
/// unless it is joined with the loop bound being 0 (`lid == 0 && bound == 0`): alone, the
/// leader's slice can finish before the other slices of its group, which breaks the usual "last
/// group done" counter that follows it; with an empty bound no slice has work to race. Every
/// write (store, atomic, image write) must sit in a stride loop or behind such a leader check,
/// or the extra threads would repeat it. Subgroup operations keep their meaning since a real
/// group is still one or more whole subgroups. Anything else is left untouched with its verdict.
WorkgroupSplitVerdict WorkgroupSplitPass(IR::Program& program, std::uint32_t factor);
} // namespace Shader::Optimization
