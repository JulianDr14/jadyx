// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

/// Tessellation run without the host tessellator (RuntimeInfo::tess_emulation), for hosts whose
/// hull and domain stages emit nothing (D3D12 on the console):
///   1. the vertex shader runs alone on the patch vertices (RuntimeInfo::tess_capture) and stores
///      the slots of four words the next stage reads at its capture slot: the vertex relative to
///      the first one (non-indexed draws) or its index (indexed draws), plus the instance times
///      VERTICES_PER_INSTANCE_WORD (storage writes: the console's stream output counts the
///      vertices but leaves the buffer untouched);
///   2. the control shader runs as compute, one invocation per output vertex, reading the captured
///      vertices and writing one record per patch (below);
///   3. a compute tessellator reads the tess levels of each record and writes the domain points
///      (u and v in 16.16 fixed point, then the patch) and an index list;
///   4. the evaluation shader runs as a vertex shader of an indexed draw over those indices.
namespace Shader::TessellationEmulation {

/// Descriptor set (register space in D3D12) of the bindings below.
constexpr u32 DESCRIPTOR_SET = 26;
/// Control: the captured vertices. Evaluation: the patch records.
constexpr u32 INPUT_BINDING = 0;
/// Vertex: the capture it writes. Control: the patch records it writes.
constexpr u32 OUTPUT_BINDING = 1;
/// Evaluation: the domain points.
constexpr u32 POINTS_BINDING = 2;
/// Vertex and control: a uniform buffer with the constants below.
constexpr u32 CONSTANTS_BINDING = 3;
/// Control: the draw's index buffer, from 4 bytes below its first index at most.
constexpr u32 INDICES_BINDING = 4;

/// Constants, in 32-bit words.
constexpr u32 TOTAL_PATCHES_WORD = 0;
constexpr u32 PATCHES_PER_INSTANCE_WORD = 1;
/// Capture slots of an instance: its vertices, or the largest index plus one.
constexpr u32 VERTICES_PER_INSTANCE_WORD = 2;
/// Record the invocations past the last patch write to, so the whole group reaches its barriers.
constexpr u32 TRASH_RECORD_WORD = 3;
/// Vertex: capture slots the buffer holds; vertices past them are not stored.
constexpr u32 CAPTURE_VERTICES_WORD = 4;
/// Control: bytes of an index (2 or 4), 0 for a non-indexed draw, and the byte of the first index
/// in INDICES_BINDING.
constexpr u32 INDEX_SIZE_WORD = 5;
constexpr u32 FIRST_INDEX_BYTE_WORD = 6;
constexpr u32 CONSTANT_WORDS = 7;

/// Patch record, in 32-bit words: the tess levels in IR::Patch order (outer left, top, right,
/// bottom, inner U, V), which is the order the tessellator reads them in, the primitive ID (the
/// patch within its instance), then the generic patch slots, then the output vertices.
constexpr u32 PRIMITIVE_WORD = 6;
constexpr u32 LEVEL_WORDS = 8;
constexpr u32 SLOT_WORDS = 4;
/// Words of a domain point.
constexpr u32 POINT_WORDS = 3;
/// Invocations a control shader group aims at; it holds whole patches.
constexpr u32 GROUP_INVOCATIONS = 64;

[[nodiscard]] constexpr u32 PatchesPerGroup(u32 output_vertices) {
    return output_vertices == 0 || output_vertices >= GROUP_INVOCATIONS
               ? 1
               : GROUP_INVOCATIONS / output_vertices;
}

[[nodiscard]] constexpr u32 RecordWords(u32 patch_slots, u32 output_vertices, u32 output_slots) {
    return LEVEL_WORDS + (patch_slots + output_vertices * output_slots) * SLOT_WORDS;
}

} // namespace Shader::TessellationEmulation
