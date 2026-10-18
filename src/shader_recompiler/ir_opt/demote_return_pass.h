// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Optimization {
/// What DemoteReturnPass did with a program. Only the Rejected* verdicts leave a terminal
/// discard arm whose lanes stop before the derivatives of their quad neighbours.
enum class DemoteReturnVerdict : std::uint8_t {
    NotNeeded,      ///< Not a fragment shader, no demote, or nothing that needs helper lanes.
    Merged,         ///< Terminal discard returns removed.
    NoTerminalArm,  ///< Demotes, but no demoted lane returns early.
    RejectedLoop,
    RejectedPhi,
    RejectedSubgroup,
    RejectedSideEffect,
    RejectedArm,    ///< A demoted lane returns early from an arm the pass cannot rewrite.
    RejectedUnstructured,
};

[[nodiscard]] std::string_view Name(DemoteReturnVerdict verdict) noexcept;

[[nodiscard]] constexpr bool IsRejected(DemoteReturnVerdict verdict) noexcept {
    return verdict >= DemoteReturnVerdict::RejectedLoop;
}

// Emission-time canonicalization for hosts with native helper-preserving demote.
// Eligible: terminal arms with identities/output writes whose lanes are demoted, in the
// arm or by an earlier arm with the same predicate (guest @P KIL ... @P EXIT).
// Returns the number of early returns removed; zero leaves the program untouched.
std::size_t DemoteReturnPass(IR::Program& program, DemoteReturnVerdict* verdict = nullptr);
}
